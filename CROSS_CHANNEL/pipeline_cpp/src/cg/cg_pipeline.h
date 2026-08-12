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

#include <string>

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

// ---- helpers exposed for the unit tests -----------------------------------

// extract_cpk entry names become filenames: every byte 0x00-0x1F plus
// " < > | : * ? \ / maps to '_'.
std::string safe_filename(const std::string& name);

// First balanced JSON array in `resp` (bracket-depth walk, string-literal and
// backslash aware), or nullopt.  Replaced a greedy \[.*\] regex that broke
// when Claude appended prose containing brackets.
std::string first_json_array(const std::string& resp, bool* found);

}  // namespace crc::cg
