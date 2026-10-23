// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shared helpers for the sakuranotoki pipeline: file IO, CP932 <-> UTF-8,
// Japanese-text classification, timestamped logging, and JSON
// pretty-printing (2-space indent).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace snt {

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
// Atomic replace: write <path>.tmp then rename over <path>, so a run killed
// mid-write leaves the previous file intact instead of a truncated one.
void write_file_atomic(const std::string& path, const std::string& content);

// Text-mode write: every '\n' becomes '\r\n'.  The pretty-printed JSON
// artifacts (cache, batch logs, narrative_candidates.json) must carry CRLF --
// they are read and hand-edited on Windows, and a resumed run has to reparse
// exactly what the previous run wrote.  (Compact JSON contains no newlines at
// all, so it can use write_file directly.)
void write_file_text(const std::string& path, const std::string& content);
void write_file_atomic_text(const std::string& path, const std::string& content);

// ---- CP932 <-> UTF-8 -------------------------------------------------------

// Strict decode: returns nullopt on any invalid or unmapped sequence.
//
// NOTE: 0x80, 0xA0 and 0xFD-0xFF ARE valid single CP932 characters
// (U+0080, U+F8F0, U+F8F1-U+F8F3).  Treating them as invalid dropped ~13% of
// the extracted strings in the BLACKCyc games, so they are mapped explicitly;
// MultiByteToWideChar(932) does not handle all of them correctly.
std::optional<std::string> cp932_to_utf8_strict(const std::uint8_t* p, std::size_t n);
inline std::optional<std::string> cp932_to_utf8_strict(const Bytes& b) {
    return cp932_to_utf8_strict(b.data(), b.size());
}

// Lossy decode: invalid bytes become U+FFFD, one per undecodable byte.
std::string cp932_to_utf8_replace(const std::uint8_t* p, std::size_t n);

// Encode UTF-8 -> CP932, substituting '?' for unmappable characters.
Bytes utf8_to_cp932_replace(const std::string& s);

// ---- Unicode helpers -------------------------------------------------------

// Decode the UTF-8 codepoint at s[i]; advances i past it.  Invalid bytes are
// consumed one at a time and returned as their raw value (never happens on
// our own transcoder output).
char32_t utf8_next(const std::string& s, std::size_t& i);

// Count of Japanese characters (hiragana / katakana / CJK unified + ext-A).
// This is the single definition of "is this text worth translating", shared by
// the step-1 capture filter and step 2's needs_translation.
int jp_char_count(const std::string& utf8);
inline bool is_jp(const std::string& utf8) { return jp_char_count(utf8) > 0; }

// True for every codepoint in the full Unicode whitespace set (see the table in
// util.cpp), U+3000 IDEOGRAPHIC SPACE included.
bool is_unicode_space(char32_t cp);

// Trim over the full Unicode whitespace set, not just ASCII whitespace:
// U+3000 IDEOGRAPHIC SPACE must go too, because CJK engines pad with it --
// an ASCII-only trim would key the TSV on a string the engine never emits.
std::string trim(const std::string& utf8);
std::string trim_left(const std::string& utf8);
std::string trim_right(const std::string& utf8);

// ---- logging ---------------------------------------------------------------

// Pipeline log format: "HH:MM:SS INFO <message>" on stdout.
void log_info(const std::string& msg);
// Bare line, no timestamp -- for banners and tables.
void print_line(const std::string& msg);
// No newline, flushed immediately -- for progress that a later log_info
// completes on the same line.
void print_inline(const std::string& msg);

// Set the console to UTF-8 so Japanese text prints legibly.  Call first in
// main(), before anything writes to stdout.
void setup_console_utf8();

// Thousands-separated integer, e.g. 49511 -> "49,511".
std::string comma(long long n);

// ---- JSON ------------------------------------------------------------------

// Pretty-print with `indent` spaces per level.  Japanese stays raw UTF-8 (no
// \uXXXX escaping) so the human-readable artifacts -- cache, batch logs --
// can be read and diffed directly.
std::string json_pretty(const boost::json::value& v, int indent = 2);

// Compact form, raw UTF-8, with ", " between elements and ": " after keys.
// boost::json::serialize emits "," and ":" with no spaces, so it is NOT a
// drop-in substitute: the spaced separators are the format of the on-disk
// files, and a resumed run has to reparse what an earlier run wrote.
std::string json_dump(const boost::json::value& v);

boost::json::value json_parse_file(const std::string& path);

}  // namespace snt
