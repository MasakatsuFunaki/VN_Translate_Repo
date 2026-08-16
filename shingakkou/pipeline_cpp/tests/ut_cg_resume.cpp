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
#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "cg/cg_pipeline.h"

namespace bj = boost::json;
namespace cg = shin::cg::detail;
namespace fs = std::filesystem;

namespace {

cg::ScanKey key(const std::string& arc, const std::string& entry) {
    return cg::ScanKey{arc, entry};
}

// Counted through the scan's own skip condition: the loop advances past an
// entry with `if (!needs_scan(done, {arc, name})) continue;`.
std::size_t left_by_the_skip(const std::vector<cg::ScanKey>& targets,
                             const cg::ScanKeySet& done) {
    std::size_t left = 0;
    for (const auto& k : targets)
        if (cg::needs_scan(done, k)) ++left;
    return left;
}

const std::vector<cg::ScanKey>& sample_targets() {
    static const std::vector<cg::ScanKey> v = {
        key("sin_sysd.dat", "title"),
        key("sin_sysd.dat", "menu"),
        key("sin_cgev.dat", "title"),
        key("sin_cgev.dat", "ev01"),
        key("sin_cgev.dat", "ev02"),
    };
    return v;
}

// The skip test asks the filesystem, not the JSON, so a painted candidate
// needs a file that is really there.
const std::string& painted_file() {
    static const std::string path = [] {
        const fs::path p =
            fs::temp_directory_path() / "shin_ut_cg_resume_patched.bmp";
        std::ofstream(p, std::ios::binary) << "BM";
        return p.u8string();
    }();
    return path;
}

const std::string& absent_file() {
    static const std::string path = [] {
        const fs::path p =
            fs::temp_directory_path() / "shin_ut_cg_resume_absent.bmp";
        fs::remove(p);
        return p.u8string();
    }();
    return path;
}

// The folder a run paints into.  Empty except for the one file the moved-root
// cases put in it, so every other recorded path that is gone stays gone.
const std::string& patched_dir() {
    static const std::string path = [] {
        const fs::path p = fs::temp_directory_path() / "shin_ut_cg_resume_out";
        fs::remove_all(p);
        fs::create_directories(p);
        return p.u8string();
    }();
    return path;
}

// What a candidates file written before the project folder moved still holds:
// an absolute path under a root that is no longer on this machine.
const std::string& path_under_a_moved_root() {
    static const std::string path = [] {
        const fs::path root =
            fs::temp_directory_path() / "shin_ut_cg_resume_old_root";
        fs::remove_all(root);
        return (root / "narrative_patched" / "moved.bmp").u8string();
    }();
    return path;
}

// The same file name, where this run paints.
const std::string& same_name_in_the_patched_dir() {
    static const std::string path = [] {
        const fs::path p = fs::u8path(patched_dir()) / "moved.bmp";
        std::ofstream(p, std::ios::binary) << "BM";
        return p.u8string();
    }();
    return path;
}

bj::object candidate(const std::string& entry, bool has_regions,
                     const std::string& patched) {
    bj::object c;
    c["arc"] = "sin_cgev.dat";
    c["entry"] = entry;
    if (has_regions) c["regions"] = bj::array{};
    else c["regions"] = nullptr;
    if (!patched.empty()) c["patched_bmp"] = patched;
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
    done.erase(key("sin_cgev.dat", "ev02"));
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), done), 1u);
}

// The first run of a fresh project has no resume file at all.
TEST(ResumeCoverage, AnEmptyResumeSetLeavesEveryTarget) {
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), {}), sample_targets().size());
}

// The key is the (archive, entry) pair: one archive's entry never covers the
// same name in another.
TEST(ResumeCoverage, CoverageIsPerArchiveNotPerEntryName) {
    const cg::ScanKeySet done = {key("sin_sysd.dat", "title")};
    EXPECT_TRUE(cg::needs_scan(done, key("sin_cgev.dat", "title")));
    EXPECT_FALSE(cg::needs_scan(done, key("sin_sysd.dat", "title")));
}

// A resume file carries entries from earlier runs, and an archive may no
// longer hold them.  Those must not be subtracted from the work that is left.
TEST(ResumeCoverage, ResumeEntriesOutsideTheTargetsChangeNothing) {
    const cg::ScanKeySet done = {key("sin_retired.dat", "gone"),
                                 key("sin_retired.dat", "also_gone")};
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), done), sample_targets().size());
}

// Every mix of covered and uncovered targets, counted both ways.
TEST(ResumeCoverage, TheScanCountAndTheScanSkipAgreeOnEveryMix) {
    const std::vector<cg::ScanKey>& targets = sample_targets();
    const unsigned n = static_cast<unsigned>(targets.size());
    for (unsigned mask = 0; mask < (1u << n); ++mask) {
        cg::ScanKeySet done;
        for (unsigned i = 0; i < n; ++i)
            if (mask & (1u << i)) done.insert(targets[i]);
        EXPECT_EQ(cg::count_left_to_scan(targets, done), left_by_the_skip(targets, done))
            << "coverage mask " << mask;
    }
}

// A candidate counts as done only with both halves: a region list and a
// patched BMP still on disk.
TEST(TranslateCoverage, APaintedCandidateNeedsNoFurtherRequest) {
    EXPECT_FALSE(cg::needs_translation(candidate("ev01", true, painted_file()),
                                       patched_dir()));
}

// The patched BMP can be deleted between runs; the candidate is then painted
// again, which costs a request.
TEST(TranslateCoverage, ACandidateWhosePatchedBmpIsGoneIsPaintedAgain) {
    EXPECT_TRUE(cg::needs_translation(candidate("ev01", true, absent_file()),
                                      patched_dir()));
}

// A scan hit is recorded with a null region list, which is the state the
// translate pass exists to fill in.
TEST(TranslateCoverage, AScanHitWithNoRegionsStillNeedsTranslation) {
    EXPECT_TRUE(cg::needs_translation(candidate("ev01", false, painted_file()),
                                      patched_dir()));
    EXPECT_TRUE(cg::needs_translation(candidate("ev02", false, ""), patched_dir()));
}

// A candidates file every entry of which is already painted leaves the pass
// nothing to send, so the run needs no key.
TEST(TranslateCoverage, AFullyPaintedCandidateListSendsNothing) {
    bj::array candidates{candidate("ev01", true, painted_file()),
                         candidate("ev02", true, painted_file())};
    EXPECT_EQ(cg::count_left_to_translate(candidates, patched_dir()), 0u);
}

// One unpainted candidate is work, and work still needs the key.  The override
// table is empty in this game, so every unpainted candidate is counted.
TEST(TranslateCoverage, OneUnpaintedCandidateIsStillCounted) {
    bj::array candidates{candidate("ev01", true, painted_file()),
                         candidate("ev02", false, "")};
    EXPECT_EQ(cg::count_left_to_translate(candidates, patched_dir()), 1u);
}

// Every mix of painted and unpainted candidates, counted both ways.
TEST(TranslateCoverage, TheTranslateCountAndTheTranslateSkipAgreeOnEveryMix) {
    const unsigned n = 5;
    for (unsigned mask = 0; mask < (1u << n); ++mask) {
        bj::array candidates;
        std::size_t by_the_skip = 0;
        for (unsigned i = 0; i < n; ++i) {
            const bool painted = (mask & (1u << i)) != 0;
            candidates.push_back(candidate("ev0" + std::to_string(i), painted,
                                           painted ? painted_file() : absent_file()));
            if (cg::needs_translation(candidates.back().get_object(), patched_dir()))
                ++by_the_skip;
        }
        EXPECT_EQ(cg::count_left_to_translate(candidates, patched_dir()), by_the_skip)
            << "painted mask " << mask;
    }
}

// A recorded path that still resolves is the answer, byte for byte: a correct
// record must keep working exactly as it did.
TEST(PaintedImageLocation, ARecordedPathThatStillResolvesIsUsedUnchanged) {
    EXPECT_EQ(cg::resolve_patched_path(painted_file(), patched_dir()), painted_file());
}

// Moving the project folder leaves every recorded root behind, and the images
// are already paid for.  Finding them by name where this run paints is what
// stops the pass buying all of them a second time.
TEST(PaintedImageLocation, APathUnderAMovedRootIsFoundWhereTheRunPaints) {
    ASSERT_FALSE(fs::exists(fs::u8path(path_under_a_moved_root())));
    ASSERT_TRUE(fs::exists(fs::u8path(same_name_in_the_patched_dir())));
    EXPECT_EQ(cg::resolve_patched_path(path_under_a_moved_root(), patched_dir()),
              same_name_in_the_patched_dir());
    EXPECT_FALSE(cg::needs_translation(
        candidate("ev01", true, path_under_a_moved_root()), patched_dir()));
}

// An image in neither location is genuinely gone, and is painted again.
TEST(PaintedImageLocation, AnImageInNeitherLocationIsReportedMissing) {
    EXPECT_TRUE(cg::resolve_patched_path(absent_file(), patched_dir()).empty());
    EXPECT_TRUE(cg::resolve_patched_path("", patched_dir()).empty());
}
