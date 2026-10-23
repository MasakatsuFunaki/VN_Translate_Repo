// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Narrative-CG scanner.
//
// Scans the Artemis PFS archives for CG images containing narrative Japanese
// text -- poems, literary quotes, chapter title cards; NOT credits, legal
// text, UI buttons or shop signs painted into backgrounds -- and records the
// hits in script_output/narrative_candidates.json.
//
// Detection is entirely Claude vision.  A local-OCR pre-filter stage would
// cut vision-token spend by rejecting text-free images before the API call,
// but no OCR engine is wired in, so the pre-filter passes everything through.
// Only the cheap dimension/prefix checks below bound the token cost.
//
// Scan-only: it reports which images are worth patching, and the
// translate/render pass is run separately.
#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace snt::cg {

struct CgOptions {
    std::string game_dir;
    std::string project_dir;  // sakuranotoki/ (for script_output)
    bool scan_only = false;
    bool no_resume = false;
    std::optional<int> max_images;
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

}  // namespace snt::cg
