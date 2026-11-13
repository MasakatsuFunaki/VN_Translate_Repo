// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Coverage of the two questions that decide whether a narrative-CG run needs
// an API key.  The scan sends a vision request only for a target
// narrative_scanned.json does not already cover; the translate pass sends one
// only for a candidate it has not already painted and no override table entry
// covers.  A count that disagrees with its loop either demands a key the run
// never uses, or resolves no client and lets the per-image handlers report the
// failure while the run still ends successfully.
//
// What these cases assert is coupling, not independence.  Neither loop is
// reachable from a unit test -- both are internal to cg_pipeline.cpp and need
// an archive, an extractor and a decodable image -- so the loops are not
// executed here.  Instead each count is checked against needs_scan /
// needs_translation, which are the functions the loops themselves call to
// decide a skip, rather than against a second copy of their bodies.
#include "cg/cg_pipeline.h"

#include "scoped_temp_dir.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace bj = boost::json;
namespace cg = mgi::cg::detail;
namespace fs = std::filesystem;

namespace {

constexpr unsigned kCombinatorialCandidateCount = 5u;

cg::ScanKey key(const std::string& arc, const std::string& entry) {
    return cg::ScanKey{arc, entry};
}

// Counted through the scan's own skip condition: the loop advances past an
// entry with `if (!needs_scan(done, {arc, name})) continue;`.
std::size_t left_by_the_skip(const std::vector<cg::ScanKey>& targets,
                             const cg::ScanKeySet& done) {
    std::size_t left = 0;
    for (const auto& k : targets) {
        if (cg::needs_scan(done, k)) {
            ++left;
        }
    }
    return left;
}

const std::vector<cg::ScanKey>& sample_targets() {
    static const std::vector<cg::ScanKey> v = {
        key("sys0.gpk", "title"),
        key("sys0.gpk", "menu"),
        key("ev0.gpk", "title"),
        key("ev0.gpk", "ev01"),
        key("ev0.gpk", "ev02"),
    };
    return v;
}

class PatchedImageWorkspace {
public:
    explicit PatchedImageWorkspace(std::string_view prefix)
        : temp_dir_(prefix),
          painted_file_(temp_dir_.join("standalone/patched.png")),
          absent_file_(temp_dir_.join("standalone/absent.png")),
          patched_dir_(temp_dir_.join("patched_out")),
          moved_in_patched_dir_(patched_dir_ / fs::u8path("moved.png")),
          path_under_moved_root_(temp_dir_.join("old_root/narrative_patched/moved.png")) {
        fs::create_directories(painted_file_.parent_path());
        fs::create_directories(patched_dir_);
        write_placeholder_png(painted_file_);
        write_placeholder_png(moved_in_patched_dir_);
    }

    PatchedImageWorkspace(const PatchedImageWorkspace&) = delete;
    PatchedImageWorkspace& operator=(const PatchedImageWorkspace&) = delete;

    std::string painted_file() const { return painted_file_.u8string(); }
    std::string absent_file() const { return absent_file_.u8string(); }
    std::string patched_dir() const { return patched_dir_.u8string(); }
    std::string same_name_in_patched_dir() const { return moved_in_patched_dir_.u8string(); }
    std::string path_under_moved_root() const { return path_under_moved_root_.u8string(); }

private:
    static void write_placeholder_png(const fs::path& file_path) {
        std::ofstream out(file_path, std::ios::binary);
        out << "PNG";
    }

    mgi::test_util::ScopedTempDir temp_dir_;
    fs::path painted_file_;
    fs::path absent_file_;
    fs::path patched_dir_;
    fs::path moved_in_patched_dir_;
    fs::path path_under_moved_root_;
};

bj::object candidate(const std::string& entry,
                     bool has_regions,
                     const std::string& patched) {
    bj::object c;
    c["arc"] = "ev0.gpk";
    c["entry"] = entry;
    c["regions"] = has_regions ? bj::value(bj::array{}) : bj::value(nullptr);
    if (!patched.empty()) {
        c["patched_img"] = patched;
    }
    return c;
}

}  // namespace

// A resume file that covers every target leaves nothing to send, and a run
// with nothing to send needs no key.
TEST(ResumeCoverage, AFullyCoveredResumeSetLeavesNothingToScan) {
    const cg::ScanKeySet done(sample_targets().begin(), sample_targets().end());
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), done), 0u);
}

// One target short of complete is still work, and work still needs the key.
TEST(ResumeCoverage, OneUncoveredTargetIsStillCounted) {
    cg::ScanKeySet done(sample_targets().begin(), sample_targets().end());
    done.erase(key("ev0.gpk", "ev02"));
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), done), 1u);
}

// The first run of a fresh project has no resume file at all.
TEST(ResumeCoverage, AnEmptyResumeSetLeavesEveryTarget) {
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), {}), sample_targets().size());
}

// The key is the (archive, entry) pair: one archive's entry never covers the
// same name in another.
TEST(ResumeCoverage, CoverageIsPerArchiveNotPerEntryName) {
    const cg::ScanKeySet done = {key("sys0.gpk", "title")};
    EXPECT_TRUE(cg::needs_scan(done, key("ev0.gpk", "title")));
    EXPECT_FALSE(cg::needs_scan(done, key("sys0.gpk", "title")));
}

// A resume file carries entries from earlier runs, and an archive may no
// longer hold them.  Those must not be subtracted from the work that is left.
TEST(ResumeCoverage, ResumeEntriesOutsideTheTargetsChangeNothing) {
    const cg::ScanKeySet done = {key("retired.gpk", "gone"),
                                 key("retired.gpk", "also_gone")};
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), done), sample_targets().size());
}

// Every mix of covered and uncovered targets, counted both ways.
TEST(ResumeCoverage, TheScanCountAndTheScanSkipAgreeOnEveryMix) {
    const std::vector<cg::ScanKey>& targets = sample_targets();
    const unsigned n = static_cast<unsigned>(targets.size());
    for (unsigned mask = 0; mask < (1u << n); ++mask) {
        cg::ScanKeySet done;
        for (unsigned i = 0; i < n; ++i) {
            if (mask & (1u << i)) {
                done.insert(targets[i]);
            }
        }
        EXPECT_EQ(cg::count_left_to_scan(targets, done), left_by_the_skip(targets, done))
            << "coverage mask " << mask;
    }
}

// A candidate counts as done only with both halves: a region list and a
// patched image still on disk.
TEST(TranslateCoverage, APaintedCandidateNeedsNoFurtherRequest) {
    const PatchedImageWorkspace ws("mgi_ut_cg_resume_painted");
    EXPECT_FALSE(cg::needs_translation(candidate("ev01", true, ws.painted_file()),
                                       ws.patched_dir()));
}

// The patched image can be deleted between runs; the candidate is then painted
// again, which costs a request.
TEST(TranslateCoverage, ACandidateWhosePatchedImageIsGoneIsPaintedAgain) {
    const PatchedImageWorkspace ws("mgi_ut_cg_resume_gone");
    EXPECT_TRUE(cg::needs_translation(candidate("ev01", true, ws.absent_file()),
                                      ws.patched_dir()));
}

// A scan hit is recorded with a null region list, which is the state the
// translate pass exists to fill in.
TEST(TranslateCoverage, AScanHitWithNoRegionsStillNeedsTranslation) {
    const PatchedImageWorkspace ws("mgi_ut_cg_resume_no_regions");
    EXPECT_TRUE(cg::needs_translation(candidate("ev01", false, ws.painted_file()),
                                      ws.patched_dir()));
    EXPECT_TRUE(cg::needs_translation(candidate("ev02", false, ""), ws.patched_dir()));
}

// A candidates file every entry of which is already painted leaves the pass
// nothing to send, so the run needs no key.
TEST(TranslateCoverage, AFullyPaintedCandidateListSendsNothing) {
    const PatchedImageWorkspace ws("mgi_ut_cg_resume_all_painted");
    bj::array candidates{candidate("ev01", true, ws.painted_file()),
                         candidate("ev02", true, ws.painted_file())};
    EXPECT_EQ(cg::count_left_to_translate(candidates, ws.patched_dir()), 0u);
}

// One unpainted candidate is work, and work still needs the key.  The override
// table is empty in this game, so every unpainted candidate is counted.
TEST(TranslateCoverage, OneUnpaintedCandidateIsStillCounted) {
    const PatchedImageWorkspace ws("mgi_ut_cg_resume_one_unpainted");
    bj::array candidates{candidate("ev01", true, ws.painted_file()),
                         candidate("ev02", false, "")};
    EXPECT_EQ(cg::count_left_to_translate(candidates, ws.patched_dir()), 1u);
}

// Every mix of painted and unpainted candidates, counted both ways.
TEST(TranslateCoverage, TheTranslateCountAndTheTranslateSkipAgreeOnEveryMix) {
    const PatchedImageWorkspace ws("mgi_ut_cg_resume_mix");
    for (unsigned mask = 0; mask < (1u << kCombinatorialCandidateCount); ++mask) {
        bj::array candidates;
        std::size_t by_the_skip = 0;
        for (unsigned i = 0; i < kCombinatorialCandidateCount; ++i) {
            const bool painted = (mask & (1u << i)) != 0;
            candidates.push_back(candidate(
                "ev0" + std::to_string(i),
                painted,
                painted ? ws.painted_file() : ws.absent_file()));
            if (cg::needs_translation(candidates.back().get_object(), ws.patched_dir())) {
                ++by_the_skip;
            }
        }
        EXPECT_EQ(cg::count_left_to_translate(candidates, ws.patched_dir()), by_the_skip)
            << "painted mask " << mask;
    }
}

// A recorded path that still resolves is the answer, byte for byte: a correct
// record must keep working exactly as it did.
TEST(PaintedImageLocation, ARecordedPathThatStillResolvesIsUsedUnchanged) {
    const PatchedImageWorkspace ws("mgi_ut_cg_resume_recorded");
    EXPECT_EQ(cg::resolve_patched_path(ws.painted_file(), ws.patched_dir()),
              ws.painted_file());
}

// Moving the project folder leaves every recorded root behind, and the images
// are already paid for.  Finding them by name where this run paints is what
// stops the pass buying all of them a second time.
TEST(PaintedImageLocation, APathUnderAMovedRootIsFoundWhereTheRunPaints) {
    const PatchedImageWorkspace ws("mgi_ut_cg_resume_moved_root");
    ASSERT_FALSE(fs::exists(fs::u8path(ws.path_under_moved_root())));
    ASSERT_TRUE(fs::exists(fs::u8path(ws.same_name_in_patched_dir())));
    EXPECT_EQ(cg::resolve_patched_path(ws.path_under_moved_root(), ws.patched_dir()),
              ws.same_name_in_patched_dir());
    EXPECT_FALSE(cg::needs_translation(
        candidate("ev01", true, ws.path_under_moved_root()), ws.patched_dir()));
}

// An image in neither location is genuinely gone, and is painted again.
TEST(PaintedImageLocation, AnImageInNeitherLocationIsReportedMissing) {
    const PatchedImageWorkspace ws("mgi_ut_cg_resume_missing");
    EXPECT_TRUE(cg::resolve_patched_path(ws.absent_file(), ws.patched_dir()).empty());
    EXPECT_TRUE(cg::resolve_patched_path("", ws.patched_dir()).empty());
}
