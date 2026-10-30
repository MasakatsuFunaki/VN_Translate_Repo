// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Narrative-CG pipeline: scan arcs for CGs with narrative JP text, render EN
// with FreeType, repack into game arcs.
#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

namespace snu::cg {

struct CgOptions {
    std::string game_dir;     // C:\SakuraNoUta
    std::string project_dir;  // SakuraNoUta/ (for script_output, fonts config)
    bool scan_only = false;
    bool repack_only = false;
    bool no_resume = false;
};

int run_find_narrative_cg(const CgOptions& opt);

// ---- resume-set coverage --------------------------------------------------

// (archive, entry) -- the pair narrative_scanned.json records, and the key the
// scan skips on.
using ScanKey = std::pair<std::string, std::string>;
using ScanKeySet = std::set<ScanKey>;

// True while the resume set does not cover this entry.  The scan skips on
// exactly this question, so the count below cannot drift away from it.
bool needs_scan(const ScanKeySet& done, const ScanKey& key);

// How many targets the resume set leaves.  Zero means the scan sends nothing
// and therefore needs no API key.  This is an upper bound on the requests, not
// an exact count: the per-image decode and dimension checks run later and can
// still drop a target.
std::size_t count_left_to_scan(const std::vector<ScanKey>& targets,
                               const ScanKeySet& done);

// ---- translate-pass coverage ----------------------------------------------

// The painted BMP a candidate records, as a path that is really there, or an
// empty string when neither location holds it.  `patched_dir` is the folder
// this run paints into.
//
// The recorded path is absolute and wins whenever it still resolves.  A
// candidates file written before the project folder moved records a root that
// no longer exists, so the same file name is looked for in `patched_dir` as
// well -- otherwise a moved checkout cannot see the images it already paid
// for, and paints and bills every one of them a second time.
std::string resolve_patched_path(const std::string& recorded,
                                 const std::string& patched_dir);

// True while the translate pass has not already painted this candidate: it
// needs both a region list and a patched BMP still on disk to count as done.
// The pass skips on exactly this question, so the count below cannot drift
// away from it.
bool needs_translation(const boost::json::object& candidate,
                       const std::string& patched_dir);

// How many candidates the translate pass would send.  Zero means it makes no
// request and therefore needs no API key.  A hand-painted entry is rendered
// locally and never counted, so injecting the override table into a fresh
// candidates list does not by itself ask for a key.  Like the scan count this
// is an upper bound: a candidate whose entry cannot be decoded is dropped
// later, without a request.
std::size_t count_left_to_translate(const boost::json::array& candidates,
                                    const std::string& patched_dir);

}  // namespace snu::cg
