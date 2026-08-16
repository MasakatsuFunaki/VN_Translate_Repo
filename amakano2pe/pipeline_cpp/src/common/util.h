// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shared helpers for the amakano2pe pipeline: file IO, CP932 <-> UTF-8,
// Japanese-text classification, timestamped logging, and JSON
// pretty-printing.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace ama {

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
// Atomic replace: write <path>.tmp then rename over <path>, so an interrupted
// run never leaves a half-written file behind.
void write_file_atomic(const std::string& path, const std::string& content);

// CRLF text write.  The pretty-printed JSON artifacts (translation cache,
// per-batch logs) are stored with Windows line endings -- that is their on-disk
// format, and every '\n' has to be translated on the way out.  (Compact JSON
// contains no newlines at all, so it can use write_file directly.)
void write_file_text(const std::string& path, const std::string& content);
void write_file_atomic_text(const std::string& path, const std::string& content);

// ---- CP932 <-> UTF-8 -------------------------------------------------------

// Strict decode: returns nullopt on any invalid or unmapped sequence.
//
// NOTE: 0x80, 0xA0 and 0xFD-0xFF ARE valid single CP932 characters
// (U+0080, U+F8F0, U+F8F1-U+F8F3).  Treating them as invalid dropped ~13% of
// the extracted strings in the BLACKCyc games, so they are mapped explicitly;
// MultiByteToWideChar(932) does not accept all of them.
std::optional<std::string> cp932_to_utf8_strict(const std::uint8_t* p, std::size_t n);
inline std::optional<std::string> cp932_to_utf8_strict(const Bytes& b) {
    return cp932_to_utf8_strict(b.data(), b.size());
}

// Lossy decode: every undecodable byte becomes U+FFFD.
std::string cp932_to_utf8_replace(const std::uint8_t* p, std::size_t n);

// Encode UTF-8 -> CP932; unmappable characters become '?'.
Bytes utf8_to_cp932_replace(const std::string& s);

// ---- Unicode helpers -------------------------------------------------------

// Decode the UTF-8 codepoint at s[i]; advances i past it.  Invalid bytes are
// consumed one at a time and returned as their raw value (never happens on
// our own transcoder output).
char32_t utf8_next(const std::string& s, std::size_t& i);

// Count of Japanese characters (hiragana / katakana / CJK unified + ext-A).
// This is what decides whether a line is translatable at all, so the extract
// and translate steps must classify a string identically.
int jp_char_count(const std::string& utf8);
inline bool is_jp(const std::string& utf8) { return jp_char_count(utf8) > 0; }

// True for the FULL Unicode whitespace set -- not just ASCII blanks.  U+3000
// IDEOGRAPHIC SPACE counts, because CJK engines pad with it freely.
bool is_unicode_space(char32_t cp);

// Whitespace trim (both ends / left / right) over the FULL Unicode whitespace
// set, as decided by is_unicode_space.  This is NOT the same as trimming ASCII
// whitespace: U+3000 IDEOGRAPHIC SPACE must be removed too, since CJK scripts
// pad with it freely -- an ASCII-only trim would key the TSV on a string the
// engine never emits.
std::string trim(const std::string& utf8);
std::string trim_left(const std::string& utf8);
std::string trim_right(const std::string& utf8);

// ---- logging ---------------------------------------------------------------

// Pipeline log format: "HH:MM:SS INFO <message>" on stdout.
void log_info(const std::string& msg);
// Bare line on stdout, no timestamp -- for banners and the gate's report.
void print_line(const std::string& msg);

// Set the console to UTF-8 so Japanese text prints legibly.  Call first in
// main().
void setup_console_utf8();

// Thousands-separated integer: 5295712 -> "5,295,712".
std::string comma(long long n);

// ---- JSON ------------------------------------------------------------------

// Pretty-printed JSON with raw (unescaped) UTF-8 -- boost::json::serialize
// already emits UTF-8; this adds the 2-space indentation of the artifacts that
// are meant to be read by a human (cache, batch logs).
std::string json_pretty(const boost::json::value& v, int indent = 2);

// COMPACT JSON with raw UTF-8.  Required separators are ", " between members
// and ": " after a key -- boost::json::serialize emits "," and ":" with no
// spaces, so serialize() alone does not produce the pipeline's on-disk format.
std::string json_dump(const boost::json::value& v);

boost::json::value json_parse_file(const std::string& path);

}  // namespace ama
