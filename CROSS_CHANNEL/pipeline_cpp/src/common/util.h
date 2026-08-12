// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shared helpers for the CROSS_CHANNEL pipeline: file IO, CP932 <-> UTF-8,
// Japanese-text classification, timestamped logging, and JSON serialisation in
// the exact shapes the pipeline's artefacts use.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace crc {

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
// Atomic replace: write <path>.tmp then rename over <path>, so a crash mid-run
// can never leave a half-written cache behind.
void write_file_atomic(const std::string& path, const std::string& content);

// TEXT-mode write: every '\n' becomes '\r\n'.  Every pretty-printed JSON
// artefact in script_output/ is CRLF-terminated and is diffed and hand-edited
// as such, so the writers must keep producing CRLF.  (Compact JSON contains no
// newlines at all, so it can use write_file directly.)
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

// Lossy decode: invalid bytes become U+FFFD, one per undecodable byte.
std::string cp932_to_utf8_replace(const std::uint8_t* p, std::size_t n);

// Encode UTF-8 -> CP932, substituting '?' for anything unmappable.
Bytes utf8_to_cp932_replace(const std::string& s);

// Strict encode: nullopt when any character has no CP932 mapping.
std::optional<Bytes> utf8_to_cp932_strict(const std::string& s);

// ---- Unicode helpers -------------------------------------------------------

// Decode the UTF-8 codepoint at s[i]; advances i past it.  Invalid bytes are
// consumed one at a time and returned as their raw value (never happens on
// our own transcoder output).
char32_t utf8_next(const std::string& s, std::size_t& i);

// Count of Japanese characters (hiragana / katakana / CJK unified + ext-A) --
// the same predicate step 1's has-Japanese test and step 2's needs_translation
// are built on.
int jp_char_count(const std::string& utf8);
inline bool is_jp(const std::string& utf8) { return jp_char_count(utf8) > 0; }

// True for every codepoint in the full Unicode whitespace set -- exactly what
// the trim helpers below treat as whitespace.
bool is_unicode_space(char32_t cp);

// Whitespace trim (both ends / left / right), over the FULL Unicode whitespace
// set.  This is NOT the same as trimming ASCII whitespace: U+3000 IDEOGRAPHIC
// SPACE has to go too, because the CJK engine pads with it and CJK scripts use
// it freely -- an ASCII-only trim would key the TSV on a string the engine
// never emits.
std::string trim(const std::string& utf8);
std::string trim_left(const std::string& utf8);
std::string trim_right(const std::string& utf8);

// ---- logging ---------------------------------------------------------------

// Pipeline log line: "HH:MM:SS INFO <message>" on stdout.
void log_info(const std::string& msg);
// Bare line, no timestamp -- for output whose exact shape is the interface
// (the speaker gate's report).
void print_line(const std::string& msg);
// Write without a trailing newline and flush, so in-progress counters appear
// immediately rather than sitting in the stdout buffer.
void print_inline(const std::string& msg);

// Set the console to UTF-8 so Japanese text prints legibly.  Call first in
// main().
void setup_console_utf8();

// Thousands-separated integer: 1234567 -> "1,234,567".
std::string comma(long long n);

// ---- JSON ------------------------------------------------------------------

// Pretty printer for the human-readable artefacts (cache, batch logs): indent
// `indent` spaces per level, and raw UTF-8 rather than \u escapes so the
// Japanese stays readable in an editor -- boost::json::serialize already emits
// UTF-8 directly.
std::string json_pretty(const boost::json::value& v, int indent = 2);

// The COMPACT form used by the single-line artefacts.  Their separators are
// ", " and ": "; boost::json::serialize emits "," and ":" with no spaces, so
// serialize() alone does NOT produce the format those files are written in.
std::string json_dump(const boost::json::value& v);

boost::json::value json_parse_file(const std::string& path);

}  // namespace crc
