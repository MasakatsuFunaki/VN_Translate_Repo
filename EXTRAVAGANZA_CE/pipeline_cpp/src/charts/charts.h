// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step 5: translate the flowchart (chart/*.fxf) files.
//
// FXF files are XOR-0xFF encrypted, CP932-encoded, and hold the episode
// selection flowmap -- node titles and hover descriptions.
//
// Format per line:
//   @nodeID flag X Y [nextIDs...] #色【title】 sentence sentence...
//                                 [!#色【title】 !sentence ...]
//
// The `!` prefix marks the alternate description shown in a different node
// state.  `#色` is a colour tag (黄=yellow, 赤=red, ...) and must survive
// verbatim.
//
// The English is converted to FULL-WIDTH before it is written back: the
// BLACKCyc engine crashes on single-byte ASCII in FXF display fields.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/util.h"

namespace exc::charts {

// XOR 0xFF, same both ways.
Bytes encrypt_fxf(const Bytes& data);
Bytes decrypt_fxf(const Bytes& data);

// ASCII -> full-width CP932.  `#` immediately followed by a non-ASCII
// character is a colour/marker tag and stays single-byte.
std::string to_fullwidth(const std::string& text);

// Kana + CJK only (no fullwidth forms, no CJK symbols).
bool has_japanese(const std::string& text);

// Split a node line into its structural prefix and its text segments.
// Returns false when the line is not a node line (does not start with '@').
bool parse_chart_line(const std::string& line, std::string* prefix,
                      std::vector<std::string>* segments);

// The pieces of one segment: '#色【title】 description'.
struct Segment {
    bool is_alt = false;
    std::string pre_title;  // '#色【'
    std::string title;
    std::string mid;  // '】' plus any following whitespace
    std::string desc;
};
Segment extract_translatable_text(const std::string& segment);

// Drop the per-line '!' markers (and the whitespace run before each) and trim:
// this is the form an alt-segment description is keyed on in the cache, so the
// same sentence is not translated twice once as main and once as alt text.
std::string strip_alt_marks(const std::string& s);

// Insert \r\n at full-width-space boundaries roughly every `max_chars`
// characters, truncating to `max_lines` so the popup does not overflow.
std::string wordwrap_fullwidth(const std::string& text, int max_chars = 42,
                               int max_lines = 5);

struct ChartOptions {
    std::string chart_dir;
    std::string backup_dir;
    std::string cache_file;
    bool retranslate = false;
    bool test_mode = false;  // dry run: list the texts, call no API
};

int run_translate_charts(const ChartOptions& opt);

}  // namespace exc::charts
