// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Narrative-CG scanner / translator.
//
// Scans the WillPlus CPK archives for images carrying Japanese text worth
// showing in English -- narrative prose, chapter cards, menu and button labels
// -- translates them with Claude vision, and paints the English back onto the
// extracted BMP.
//
// CPK extraction is delegated to TOOLS/garbro/extract_cpk.exe; CPK *repacking*
// is not implemented, so the patched BMPs are inspection-only.
//
// There is no local OCR pre-filter, so every image over the size threshold
// costs a vision call.  --scan-only and the confirmation prompt are what bound
// the spend, and narrative_scanned.json keeps a resumed scan from paying for
// the same image twice.
#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

namespace crc::cg {

struct CgOptions {
    std::string game_dir;     // C:\Games\CROSS_CHANNEL  (the archives live in .\data)
    std::string project_dir;  // CROSS_CHANNEL\  (for script_output)
    bool scan_only = false;
    bool no_resume = false;
    // Skip the interactive "send to Claude?" prompt (for non-tty runs).
    bool assume_yes = false;
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
// request and therefore needs no API key.  Like the scan count this is an
// upper bound: a candidate whose image cannot be found or decoded is dropped
// later, without a request.
std::size_t count_left_to_translate(const boost::json::array& candidates,
                                    const std::string& patched_dir);

// ---- helpers exposed for the unit tests -----------------------------------

// extract_cpk entry names become filenames: every byte 0x00-0x1F plus
// " < > | : * ? \ / maps to '_'.
std::string safe_filename(const std::string& name);

// First balanced JSON array in `resp` (bracket-depth walk, string-literal and
// backslash aware), or nullopt.  Replaced a greedy \[.*\] regex that broke
// when Claude appended prose containing brackets.
std::string first_json_array(const std::string& resp, bool* found);

}  // namespace crc::cg
