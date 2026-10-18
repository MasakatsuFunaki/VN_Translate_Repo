// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shared helpers: file IO, CP932 <-> UTF-8, JP-text classification, logging,
// JSON pretty-printing.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace snu {

using Bytes = std::vector<std::uint8_t>;

// ---- file IO ---------------------------------------------------------------

Bytes read_file(const std::string& path);
void write_file(const std::string& path, const void* data, std::size_t size);
inline void write_file(const std::string& path, const Bytes& b) {
    write_file(path, b.data(), b.size());
}
inline void write_file(const std::string& path, const std::string& s) {
    write_file(path, s.data(), s.size());
}
// Atomic replace: write <path>.tmp then rename over <path>.  Every step is
// resume-safe, so a Ctrl+C mid-write must never leave a truncated artifact.
void write_file_atomic(const std::string& path, const std::string& content);

// ---- CP932 <-> UTF-8 -------------------------------------------------------

// Strict CP932 decode: returns nullopt on any invalid or unmapped byte.
// Pre-validates structure before MultiByteToWideChar(MB_ERR_INVALID_CHARS).
std::optional<std::string> cp932_to_utf8_strict(const std::uint8_t* p, std::size_t n);
inline std::optional<std::string> cp932_to_utf8_strict(const Bytes& b) {
    return cp932_to_utf8_strict(b.data(), b.size());
}

// Lossy decode: invalid bytes become U+FFFD.
std::string cp932_to_utf8_replace(const std::uint8_t* p, std::size_t n);

// Encode UTF-8 -> CP932; unmappable characters become '?'.
Bytes utf8_to_cp932_replace(const std::string& s);

// ---- Unicode helpers -------------------------------------------------------

// Decode the UTF-8 codepoint at s[i]; advances i past it.
char32_t utf8_next(const std::string& s, std::size_t& i);

// Count of Japanese characters (hiragana / katakana / CJK).  Shared by
// extraction and translation so classification stays consistent.
int jp_char_count(const std::string& utf8);
inline bool is_jp(const std::string& utf8) { return jp_char_count(utf8) > 0; }

// Reject strings containing control chars other than \t \r \n (extraction
// filter: they are bytecode operands that happen to decode as CP932).
bool is_clean_text(const std::string& utf8);

// ---- logging ---------------------------------------------------------------

// Progress format: "HH:MM:SS INFO <message>" on stdout.
void log_info(const std::string& msg);
// Bare line, no timestamp -- banners and summaries.
void print_line(const std::string& msg);
// Write without a trailing newline and flush (progress counters that overwrite
// themselves); stdout is otherwise block-buffered when redirected to a log.
void print_inline(const std::string& msg);

// Set the console to UTF-8 so Japanese text prints legibly instead of as
// mojibake under the default OEM codepage.  Call first in main().
void setup_console_utf8();

// ---- JSON ------------------------------------------------------------------

// 2-space-indented JSON with raw UTF-8 (no \uXXXX) for diffable artifacts.
std::string json_pretty(const boost::json::value& v, int indent = 2);

boost::json::value json_parse_file(const std::string& path);

}  // namespace snu
