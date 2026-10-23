// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// translator_logic.h — pure, testable lookup core (no Win32 deps).
//
// Loads `translations.tsv` (UTF-8) into an unordered_map<string, string>
// and exposes O(1) JP -> EN lookup.  The same logic is exercised by the
// gtest harness under tests/.
#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tr {

// Parse TSV bytes -> map.  Each line: <jp>\t<en>\n.  Escapes recognized
// in BOTH columns: \\\\, \\t, \\r, \\n.  Empty lines and lines without a
// tab are skipped silently.
//
// Returns the number of entries inserted (skips duplicates: first wins).
size_t load_tsv(std::string_view tsv_bytes,
                std::unordered_map<std::string, std::string>& out);

// Convenience: read the file at `path` (UTF-8) and parse it.
// On failure returns SIZE_MAX and `out` is unmodified.
size_t load_tsv_file(const std::string& path,
                     std::unordered_map<std::string, std::string>& out);

// Lookup `jp` in the table.  If found, fill `out` and return true.
// If not found, return false.
bool lookup(const std::unordered_map<std::string, std::string>& table,
            std::string_view jp, std::string& out);

// True if `s` contains at least one Japanese codepoint when interpreted
// as UTF-8 (hiragana, katakana, CJK basic + ext-A).  Used to skip
// pure-ASCII engine strings cheaply.
bool has_japanese_utf8(std::string_view s);

}  // namespace tr
