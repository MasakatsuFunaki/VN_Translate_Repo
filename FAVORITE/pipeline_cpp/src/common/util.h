// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shared helpers for the FAVORITE pipeline: file IO, CP932 <-> UTF-8,
// Japanese-text classification, timestamped logging, and JSON
// pretty-printing (2-space indent).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace fav {

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
// Atomic replace: write <path>.tmp then rename over <path>, so a crash
// mid-write can never leave a half-written file behind.
void write_file_atomic(const std::string& path, const std::string& content);

// Text-mode write: every '\n' becomes '\r\n'.  The pretty-printed JSON
// artefacts (the resume cache and the per-batch logs) are Windows text files
// -- they get opened in editors and diffed run-to-run -- so they must carry
// CRLF.  (Compact JSON contains no newlines at all, so it uses write_file.)
void write_file_text(const std::string& path, const std::string& content);
void write_file_atomic_text(const std::string& path, const std::string& content);

// ---- CP932 <-> UTF-8 -------------------------------------------------------

// Strict decode: returns nullopt on any invalid or unmapped sequence.
//
// NOTE: 0x80, 0xA0 and 0xFD-0xFF ARE valid single CP932 characters
// (U+0080, U+F8F0, U+F8F1-U+F8F3).  Treating them as invalid dropped ~13% of
// the extracted strings in the BLACKCyc games, so they are mapped explicitly;
// MultiByteToWideChar(932) does not agree on all of them.
std::optional<std::string> cp932_to_utf8_strict(const std::uint8_t* p, std::size_t n);
inline std::optional<std::string> cp932_to_utf8_strict(const Bytes& b) {
    return cp932_to_utf8_strict(b.data(), b.size());
}

// Lossy decode: invalid bytes become U+FFFD.
std::string cp932_to_utf8_replace(const std::uint8_t* p, std::size_t n);

// Encode UTF-8 -> CP932, substituting '?' for anything unmappable.
Bytes utf8_to_cp932_replace(const std::string& s);

// ---- Unicode helpers -------------------------------------------------------

// Decode the UTF-8 codepoint at s[i]; advances i past it.  Invalid bytes are
// consumed one at a time and returned as their raw value (never happens on
// our own transcoder output).
char32_t utf8_next(const std::string& s, std::size_t& i);

// Count of Japanese characters (hiragana / katakana / CJK unified + ext-A).
// This is the single JP test the pipeline uses: the extractor's tally and the
// translator's needs_translation() are both this function.
int jp_char_count(const std::string& utf8);
inline bool is_jp(const std::string& utf8) { return jp_char_count(utf8) > 0; }

// True for the full Unicode whitespace set: category Zs -- U+3000 IDEOGRAPHIC
// SPACE included -- plus the bidi WS/B/S controls.
bool is_unicode_space(char32_t cp);

// Trim leading and trailing whitespace over the FULL Unicode set, not just
// ASCII: U+3000 IDEOGRAPHIC SPACE must go too, because that is exactly what
// FVP pads its name-window strings with -- an ASCII-only trim would key the
// TSV on a string the engine never emits.
std::string trim(const std::string& utf8);

// ---- logging ---------------------------------------------------------------

// Pipeline log format: "HH:MM:SS INFO <message>" on stdout.
void log_info(const std::string& msg);
// Bare line, no timestamp -- for banners and the gate's check report.
void print_line(const std::string& msg);

// Set the console to UTF-8 so Japanese text prints legibly.  Call first in
// main().
void setup_console_utf8();

// Thousands-separated integer: 61164 -> "61,164".
std::string comma(long long n);

// ---- JSON ------------------------------------------------------------------

// Pretty form, for the artefacts that are meant to be read by a human (resume
// cache, batch logs).  Japanese stays raw UTF-8 rather than \u-escaped --
// boost::json::serialize already does that; this only adds the indentation.
std::string json_pretty(const boost::json::value& v, int indent = 2);

// Compact form, for the multi-MB artefacts (extracted_text.json,
// translated_text.json).  The required separators are ", " and ": ";
// boost::json::serialize emits "," and ":" with no spaces, so it cannot be
// used on its own.
std::string json_dump(const boost::json::value& v);

boost::json::value json_parse_file(const std::string& path);

}  // namespace fav
