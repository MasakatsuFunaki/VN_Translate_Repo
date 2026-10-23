// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Resume-set coverage, the one question that decides whether a narrative-CG
// run needs an API key here: the scan sends a vision request only for a target
// narrative_scanned.json does not already cover.  A count that disagrees with
// the loop either demands a key the run never uses, or resolves no client and
// lets the per-image handlers report the failure while the run still ends
// successfully.  This game scans only -- it has no translate pass, so it has
// no second count.
//
// What these cases assert is coupling, not independence.  The loop is not
// reachable from a unit test -- it is internal to cg_pipeline.cpp and needs an
// archive and a decodable entry -- so it is not executed here.  Instead the
// count is checked against needs_scan, which is the function the loop itself
// calls to decide a skip, rather than against a second copy of its body.
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "cg/cg_pipeline.h"

namespace cg = snt::cg;

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
        key("sakuranotoki.pfs", "image\\ev\\title.png"),
        key("sakuranotoki.pfs", "image\\cg\\menu.png"),
        key("sakuranotoki.pfs.000", "image\\ev\\title.png"),
        key("sakuranotoki.pfs.000", "image\\ev\\ev01.png"),
        key("sakuranotoki.pfs.000", "image\\ev\\ev02.png"),
    };
    return v;
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
    done.erase(key("sakuranotoki.pfs.000", "image\\ev\\ev02.png"));
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), done), 1u);
}

// The first run of a fresh project has no resume file at all.
TEST(ResumeCoverage, AnEmptyResumeSetLeavesEveryTarget) {
    EXPECT_EQ(cg::count_left_to_scan(sample_targets(), {}), sample_targets().size());
}

// The key is the (archive, entry) pair: one archive's entry never covers the
// same name in another.
TEST(ResumeCoverage, CoverageIsPerArchiveNotPerEntryName) {
    const cg::ScanKeySet done = {key("sakuranotoki.pfs", "image\\ev\\title.png")};
    EXPECT_TRUE(cg::needs_scan(done, key("sakuranotoki.pfs.000", "image\\ev\\title.png")));
    EXPECT_FALSE(cg::needs_scan(done, key("sakuranotoki.pfs", "image\\ev\\title.png")));
}

// A resume file carries entries from earlier runs, and an archive may no
// longer hold them.  Those must not be subtracted from the work that is left.
TEST(ResumeCoverage, ResumeEntriesOutsideTheTargetsChangeNothing) {
    const cg::ScanKeySet done = {key("retired.pfs", "image\\ev\\gone.png"),
                                 key("retired.pfs", "image\\ev\\also_gone.png")};
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
