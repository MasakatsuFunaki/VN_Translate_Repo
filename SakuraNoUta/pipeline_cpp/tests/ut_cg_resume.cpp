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
// an archive and a decodable entry -- so the loops are not executed here.
// Instead each count is checked against needs_scan / needs_translation, which
// are the functions the loops themselves call to decide a skip, rather than
// against a second copy of their bodies.
#include "cg/cg_pipeline.h"

#include <boost/json.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace bj = boost::json;
namespace cg = snu::cg;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kManualOverrideVariantCount = 6;
constexpr unsigned kCandidateMixCount = 5;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view utf8_name)
        : path_(fs::temp_directory_path() / fs::u8path(utf8_name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

// Per-test scratch files and directories, cleaned up automatically on scope exit.
struct CgResumeScratch {
    ScopedTempDir dir{"snu_ut_cg_resume"};
    std::string painted_file;
    std::string absent_file;
    std::string patched_dir;
    std::string moved_root_file;
    std::string relocated_file;

    CgResumeScratch() {
        const fs::path painted_path = dir.path() / fs::u8path("patched.bmp");
        {
            std::ofstream out(painted_path, std::ios::binary);
            out << "BM";
        }
        painted_file = painted_path.u8string();
        absent_file = (dir.path() / fs::u8path("absent.bmp")).u8string();

        const fs::path out_dir_path = dir.path() / fs::u8path("out");
        std::error_code ec;
        fs::create_directories(out_dir_path, ec);
        patched_dir = out_dir_path.u8string();

        moved_root_file =
            (dir.path() / fs::u8path("old_root") /
             fs::u8path("narrative_patched") / fs::u8path("moved.bmp"))
                .u8string();

        const fs::path relocated_path = out_dir_path / fs::u8path("moved.bmp");
        {
            std::ofstream out(relocated_path, std::ios::binary);
            out << "BM";
        }
        relocated_file = relocated_path.u8string();
    }
};

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
        key("data02099.arc", "title"),
        key("data02099.arc", "menu"),
        key("data02500.arc", "title"),
        key("data02500.arc", "ev01"),
        key("data02500.arc", "ev02"),
    };
    return v;
}

bj::object candidate(const std::string& arc, const std::string& entry,
                     bool has_regions, const std::string& patched) {
    bj::object c;
    c["arc"] = arc;
    c["entry"] = entry;
    if (has_regions) {
        c["regions"] = bj::array{};
    } else {
        c["regions"] = nullptr;
    }
    if (!patched.empty()) {
        c["patched_bmp"] = patched;
    }
    return c;
}

bj::object auto_candidate(const std::string& entry, bool has_regions,
                          const std::string& patched) {
    return candidate("data02500.arc", entry, has_regions, patched);
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
    done.erase(key("data02500.arc", "ev02"));
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), done), 1u);
}

// The first run of a fresh project has no resume file at all.
TEST(ResumeCoverage, AnEmptyResumeSetLeavesEveryTarget) {
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), {}), sample_targets().size());
}

// The key is the (archive, entry) pair: one archive's entry never covers the
// same name in another.
TEST(ResumeCoverage, CoverageIsPerArchiveNotPerEntryName) {
    const cg::ScanKeySet done = {key("data02099.arc", "title")};
    EXPECT_TRUE(cg::needs_scan(done, key("data02500.arc", "title")));
    EXPECT_FALSE(cg::needs_scan(done, key("data02099.arc", "title")));
}

// A resume file carries entries from earlier runs, and an archive may no
// longer hold them.  Those must not be subtracted from the work that is left.
TEST(ResumeCoverage, ResumeEntriesOutsideTheTargetsChangeNothing) {
    const cg::ScanKeySet done = {key("data09999.arc", "gone"),
                                 key("data09999.arc", "also_gone")};
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), done), sample_targets().size());
}

// Every mix of covered and uncovered targets, counted both ways.
TEST(ResumeCoverage, TheScanCountAndTheScanSkipAgreeOnEveryMix) {
    const std::vector<cg::ScanKey>& targets = sample_targets();
    const unsigned n = static_cast<unsigned>(targets.size());
    for (unsigned mask = 0; mask < (1u << n); ++mask) {
        cg::ScanKeySet done;
        for (unsigned i = 0; i < n; ++i) {
            if ((mask & (1u << i)) != 0u) {
                done.insert(targets[i]);
            }
        }
        EXPECT_EQ(cg::count_left_to_scan(targets, done), left_by_the_skip(targets, done))
            << "coverage mask " << mask;
    }
}

// A candidate counts as done only with both halves: a region list and a
// patched BMP still on disk.
TEST(TranslateCoverage, APaintedCandidateNeedsNoFurtherRequest) {
    const CgResumeScratch scratch;
    EXPECT_FALSE(cg::needs_translation(
        auto_candidate("ev01", true, scratch.painted_file), scratch.patched_dir));
}

// The patched BMP can be deleted between runs; the candidate is then painted
// again, which costs a request.
TEST(TranslateCoverage, ACandidateWhosePatchedBmpIsGoneIsPaintedAgain) {
    const CgResumeScratch scratch;
    EXPECT_TRUE(cg::needs_translation(
        auto_candidate("ev01", true, scratch.absent_file), scratch.patched_dir));
}

// A scan hit is recorded with a null region list, which is the state the
// translate pass exists to fill in.
TEST(TranslateCoverage, AScanHitWithNoRegionsStillNeedsTranslation) {
    const CgResumeScratch scratch;
    EXPECT_TRUE(cg::needs_translation(
        auto_candidate("ev01", false, scratch.painted_file), scratch.patched_dir));
    EXPECT_TRUE(
        cg::needs_translation(auto_candidate("ev02", false, ""), scratch.patched_dir));
}

// A candidates file every entry of which is already painted leaves the pass
// nothing to send, so the run needs no key.
TEST(TranslateCoverage, AFullyPaintedCandidateListSendsNothing) {
    const CgResumeScratch scratch;
    const bj::array candidates{
        auto_candidate("ev01", true, scratch.painted_file),
        auto_candidate("ev02", true, scratch.painted_file),
    };
    EXPECT_EQ(cg::count_left_to_translate(candidates, scratch.patched_dir), 0u);
}

// One unpainted candidate is work, and work still needs the key.
TEST(TranslateCoverage, OneUnpaintedCandidateIsStillCounted) {
    const CgResumeScratch scratch;
    const bj::array candidates{
        auto_candidate("ev01", true, scratch.painted_file),
        auto_candidate("ev02", false, ""),
    };
    EXPECT_EQ(cg::count_left_to_translate(candidates, scratch.patched_dir), 1u);
}

// The scan injects every override table entry the resume set does not already
// hold, so a fresh candidates list is never empty in this game.  Those entries
// are painted from the table itself and cost no request, so they must not on
// their own make the run ask for a key -- while still being unpainted, so the
// pass does reach and render them.
TEST(TranslateCoverage, TheInjectedManualOverridesAskForNoKey) {
    const CgResumeScratch scratch;
    bj::array candidates;
    for (char n = 'a'; n <= 'f'; ++n) {
        candidates.push_back(
            candidate("data02501.arc", std::string("sp0065") + n, false, ""));
    }
    ASSERT_EQ(candidates.size(), kManualOverrideVariantCount);
    EXPECT_EQ(cg::count_left_to_translate(candidates, scratch.patched_dir), 0u);
    for (const auto& cv : candidates) {
        EXPECT_TRUE(cg::needs_translation(cv.get_object(), scratch.patched_dir));
    }
}

// An override entry costs nothing even beside real work, which still counts.
TEST(TranslateCoverage, AnOverrideBesideRealWorkCountsOnlyTheRealWork) {
    const CgResumeScratch scratch;
    const bj::array candidates{
        candidate("data02501.arc", "sp0065a", false, ""),
        auto_candidate("ev01", false, ""),
    };
    EXPECT_EQ(cg::count_left_to_translate(candidates, scratch.patched_dir), 1u);
}

// Every mix of painted and unpainted candidates, counted both ways.
TEST(TranslateCoverage, TheTranslateCountAndTheTranslateSkipAgreeOnEveryMix) {
    const CgResumeScratch scratch;
    for (unsigned mask = 0; mask < (1u << kCandidateMixCount); ++mask) {
        bj::array candidates;
        std::size_t by_the_skip = 0;
        for (unsigned i = 0; i < kCandidateMixCount; ++i) {
            const bool painted = (mask & (1u << i)) != 0u;
            candidates.push_back(auto_candidate(
                "ev0" + std::to_string(i), painted,
                painted ? scratch.painted_file : scratch.absent_file));
            if (cg::needs_translation(candidates.back().get_object(),
                                      scratch.patched_dir)) {
                ++by_the_skip;
            }
        }
        EXPECT_EQ(cg::count_left_to_translate(candidates, scratch.patched_dir),
                  by_the_skip)
            << "painted mask " << mask;
    }
}

// A recorded path that still resolves is the answer, byte for byte: a correct
// record must keep working exactly as it did.
TEST(PaintedImageLocation, ARecordedPathThatStillResolvesIsUsedUnchanged) {
    const CgResumeScratch scratch;
    EXPECT_EQ(cg::resolve_patched_path(scratch.painted_file, scratch.patched_dir),
              scratch.painted_file);
}

// Moving the project folder leaves every recorded root behind, and the images
// are already paid for.  Finding them by name where this run paints is what
// stops the pass buying all of them a second time.
TEST(PaintedImageLocation, APathUnderAMovedRootIsFoundWhereTheRunPaints) {
    const CgResumeScratch scratch;
    ASSERT_FALSE(fs::exists(fs::u8path(scratch.moved_root_file)));
    ASSERT_TRUE(fs::exists(fs::u8path(scratch.relocated_file)));
    EXPECT_EQ(cg::resolve_patched_path(scratch.moved_root_file, scratch.patched_dir),
              scratch.relocated_file);
    EXPECT_FALSE(cg::needs_translation(
        auto_candidate("ev01", true, scratch.moved_root_file), scratch.patched_dir));
}

// An image in neither location is genuinely gone, and is painted again.
TEST(PaintedImageLocation, AnImageInNeitherLocationIsReportedMissing) {
    const CgResumeScratch scratch;
    EXPECT_TRUE(
        cg::resolve_patched_path(scratch.absent_file, scratch.patched_dir).empty());
    EXPECT_TRUE(cg::resolve_patched_path("", scratch.patched_dir).empty());
}
