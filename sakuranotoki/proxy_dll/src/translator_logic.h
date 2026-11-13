// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// translator_logic.h -- pure, testable lookup core (no Win32 dependencies).
//
// Loads `translations.tsv` (UTF-8) into an unordered_map<string, string>
// and exposes O(1) JP -> EN lookup.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tr {

// Parses TSV bytes into `out`.  Each line has `<jp>\t<en>\n`.
// Strips a leading UTF-8 BOM if present, skips empty lines, `#` comment lines,
// and lines without a tab, and unescapes `\\`, `\t`, `\r`, `\n` in both columns.
// Returns the number of entries inserted (first occurrence wins on duplicates).
std::size_t load_tsv(std::string_view tsv_bytes,
                     std::unordered_map<std::string, std::string>& out);

// Reads the UTF-8 file at `path` and parses it into `out`.
// On failure returns `SIZE_MAX` and leaves `out` unmodified.
std::size_t load_tsv_file(const std::string& path,
                          std::unordered_map<std::string, std::string>& out);

// Looks up `jp` in `table`.  If found, writes the value to `out` and returns
// true; otherwise leaves `out` unchanged and returns false.
bool lookup(const std::unordered_map<std::string, std::string>& table,
            std::string_view jp, std::string& out);

// Returns true if `s` is valid UTF-8 up through the first Japanese codepoint
// (Hiragana, Katakana, CJK Unified Ideographs, or CJK Extension A).
bool has_japanese_utf8(std::string_view s);

}  // namespace tr
