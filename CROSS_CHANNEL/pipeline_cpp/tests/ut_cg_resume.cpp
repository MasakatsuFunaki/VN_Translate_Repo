// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Coverage of the two questions that decide whether a narrative-CG run needs
// an API key.  The scan sends a vision request only for a target
// narrative_scanned.json does not already cover; the translate pass sends one
// only for a candidate it has not already painted.
#include "cg/cg_pipeline.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace bj = boost::json;
namespace cg = crc::cg;
namespace fs = std::filesystem;

namespace {

constexpr unsigned int kMixCandidateCount = 5u;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0};
        const std::string name =
            "crc_ut_cg_resume_" + std::to_string(++seq);
        dir_ = fs::temp_directory_path() / fs::u8path(name);
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const { return dir_; }

private:
    fs::path dir_;
};

struct ResumePaths {
    ScopedTempDir root;
    std::string painted_file;
    std::string absent_file;
    std::string patched_dir;
    std::string moved_root_path;
    std::string same_name_in_patched_dir;

    ResumePaths() {
        const fs::path painted = root.path() / "patched.bmp";
        {
            std::ofstream out(painted, std::ios::binary);
            out << "BM";
        }
        painted_file = painted.u8string();
        absent_file = (root.path() / "absent.bmp").u8string();

        const fs::path out_dir = root.path() / "out";
        fs::create_directories(out_dir);
        patched_dir = out_dir.u8string();

        moved_root_path =
            (root.path() / "old_root" / "narrative_patched" / "moved.bmp").u8string();

        const fs::path moved_here = out_dir / "moved.bmp";
        {
            std::ofstream out(moved_here, std::ios::binary);
            out << "BM";
        }
        same_name_in_patched_dir = moved_here.u8string();
    }
};

cg::ScanKey key(const std::string& arc, const std::string& entry) {
    return cg::ScanKey{arc, entry};
}

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
        key("sys.cpk", "title"), key("sys.cpk", "menu"), key("ev.cpk", "title"),
        key("ev.cpk", "ev01"),   key("ev.cpk", "ev02"),
    };
    return v;
}

bj::object candidate(const std::string& entry, bool has_regions,
                     const std::string& patched) {
    bj::object c;
    c["arc"] = "ev.cpk";
    c["entry"] = entry;
    c["regions"] = has_regions ? bj::value(bj::array{}) : bj::value(nullptr);
    if (!patched.empty()) {
        c["patched_bmp"] = patched;
    }
    return c;
}

}  // namespace

TEST(ResumeCoverage, AFullyCoveredResumeSetLeavesNothingToScan) {
    const cg::ScanKeySet done(sample_targets().begin(), sample_targets().end());
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), done), 0u);
}

TEST(ResumeCoverage, OneUncoveredTargetIsStillCounted) {
    cg::ScanKeySet done(sample_targets().begin(), sample_targets().end());
    done.erase(key("ev.cpk", "ev02"));
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), done), 1u);
}

TEST(ResumeCoverage, AnEmptyResumeSetLeavesEveryTarget) {
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), {}), sample_targets().size());
}

TEST(ResumeCoverage, CoverageIsPerArchiveNotPerEntryName) {
    const cg::ScanKeySet done = {key("sys.cpk", "title")};
    EXPECT_TRUE(cg::needs_scan(done, key("ev.cpk", "title")));
    EXPECT_FALSE(cg::needs_scan(done, key("sys.cpk", "title")));
}

TEST(ResumeCoverage, ResumeEntriesOutsideTheTargetsChangeNothing) {
    const cg::ScanKeySet done = {key("retired.cpk", "gone"),
                                 key("retired.cpk", "also_gone")};
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), done), sample_targets().size());
}

TEST(ResumeCoverage, TheScanCountAndTheScanSkipAgreeOnEveryMix) {
    const std::vector<cg::ScanKey>& targets = sample_targets();
    const unsigned int n = static_cast<unsigned int>(targets.size());
    for (unsigned int mask = 0; mask < (1u << n); ++mask) {
        cg::ScanKeySet done;
        for (unsigned int i = 0; i < n; ++i) {
            if (mask & (1u << i)) {
                done.insert(targets[i]);
            }
        }
        EXPECT_EQ(cg::count_left_to_scan(targets, done), left_by_the_skip(targets, done))
            << "coverage mask " << mask;
    }
}

TEST(TranslateCoverage, APaintedCandidateNeedsNoFurtherRequest) {
    ResumePaths paths;
    EXPECT_FALSE(cg::needs_translation(candidate("ev01", true, paths.painted_file),
                                       paths.patched_dir));
}

TEST(TranslateCoverage, ACandidateWhosePatchedBmpIsGoneIsPaintedAgain) {
    ResumePaths paths;
    EXPECT_TRUE(cg::needs_translation(candidate("ev01", true, paths.absent_file),
                                      paths.patched_dir));
}

TEST(TranslateCoverage, AScanHitWithNoRegionsStillNeedsTranslation) {
    ResumePaths paths;
    EXPECT_TRUE(cg::needs_translation(candidate("ev01", false, paths.painted_file),
                                      paths.patched_dir));
    EXPECT_TRUE(cg::needs_translation(candidate("ev02", false, ""), paths.patched_dir));
}

TEST(TranslateCoverage, AFullyPaintedCandidateListSendsNothing) {
    ResumePaths paths;
    bj::array candidates{candidate("ev01", true, paths.painted_file),
                         candidate("ev02", true, paths.painted_file)};
    EXPECT_EQ(cg::count_left_to_translate(candidates, paths.patched_dir), 0u);
}

TEST(TranslateCoverage, OneUnpaintedCandidateIsStillCounted) {
    ResumePaths paths;
    bj::array candidates{candidate("ev01", true, paths.painted_file),
                         candidate("ev02", false, "")};
    EXPECT_EQ(cg::count_left_to_translate(candidates, paths.patched_dir), 1u);
}

TEST(TranslateCoverage, TheTranslateCountAndTheTranslateSkipAgreeOnEveryMix) {
    ResumePaths paths;
    for (unsigned int mask = 0; mask < (1u << kMixCandidateCount); ++mask) {
        bj::array candidates;
        std::size_t by_the_skip = 0;
        for (unsigned int i = 0; i < kMixCandidateCount; ++i) {
            const bool painted = (mask & (1u << i)) != 0;
            candidates.push_back(candidate(
                "ev0" + std::to_string(i), painted,
                painted ? paths.painted_file : paths.absent_file));
            if (cg::needs_translation(candidates.back().get_object(), paths.patched_dir)) {
                ++by_the_skip;
            }
        }
        EXPECT_EQ(cg::count_left_to_translate(candidates, paths.patched_dir), by_the_skip)
            << "painted mask " << mask;
    }
}

TEST(PaintedImageLocation, ARecordedPathThatStillResolvesIsUsedUnchanged) {
    ResumePaths paths;
    EXPECT_EQ(cg::resolve_patched_path(paths.painted_file, paths.patched_dir),
              paths.painted_file);
}

TEST(PaintedImageLocation, APathUnderAMovedRootIsFoundWhereTheRunPaints) {
    ResumePaths paths;
    ASSERT_FALSE(fs::exists(fs::u8path(paths.moved_root_path)));
    ASSERT_TRUE(fs::exists(fs::u8path(paths.same_name_in_patched_dir)));
    EXPECT_EQ(cg::resolve_patched_path(paths.moved_root_path, paths.patched_dir),
              paths.same_name_in_patched_dir);
    EXPECT_FALSE(cg::needs_translation(
        candidate("ev01", true, paths.moved_root_path), paths.patched_dir));
}

TEST(PaintedImageLocation, AnImageInNeitherLocationIsReportedMissing) {
    ResumePaths paths;
    EXPECT_TRUE(cg::resolve_patched_path(paths.absent_file, paths.patched_dir).empty());
    EXPECT_TRUE(cg::resolve_patched_path("", paths.patched_dir).empty());
}
