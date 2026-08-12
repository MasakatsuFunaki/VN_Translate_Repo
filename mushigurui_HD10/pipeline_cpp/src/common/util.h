// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shared helpers for the mushigurui_HD10 pipeline: file IO, CP932 <-> UTF-8,
// Japanese-text classification, logging in the pipeline's console format,
// and JSON pretty-printing (2-space indent).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace mgi {

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
// can never leave a half-written resume cache behind.
void write_file_atomic(const std::string& path, const std::string& content);

// Text-mode write: every '\n' goes out as '\r\n'.  The pretty-printed JSON
// artifacts are CRLF files -- they are hand-inspected and diffed on Windows,
// and the checked-in copies are stored that way.  (Compact JSON contains no
// newlines at all, so it can use write_file directly.)
void write_file_text(const std::string& path, const std::string& content);
void write_file_atomic_text(const std::string& path, const std::string& content);

// ---- CP932 <-> UTF-8 -------------------------------------------------------

// Strict decode: returns nullopt on any invalid or unmapped sequence.  Strict
// decode is the validity gate that tells script text apart from opcode bytes,
// so it must not be relaxed.
//
// NOTE: CP932 maps 0x80, 0xA0 and 0xFD-0xFF as single characters
// (U+0080, U+F8F0, U+F8F1-U+F8F3).  Treating them as invalid dropped ~13% of
// the extracted strings in the BLACKCyc games, so they are mapped explicitly;
// MultiByteToWideChar(932) does not agree on all of them.
std::optional<std::string> cp932_to_utf8_strict(const std::uint8_t* p, std::size_t n);
inline std::optional<std::string> cp932_to_utf8_strict(const Bytes& b) {
    return cp932_to_utf8_strict(b.data(), b.size());
}

// Lossy decode: invalid bytes become U+FFFD, one per undecodable byte.
std::string cp932_to_utf8_replace(const std::uint8_t* p, std::size_t n);

// Encode UTF-8 -> CP932, substituting '?' for unmappable characters.
Bytes utf8_to_cp932_replace(const std::string& s);

// Strict encode: nullopt when any character has no CP932 mapping.
std::optional<Bytes> utf8_to_cp932_strict(const std::string& s);

// ---- Unicode helpers -------------------------------------------------------

// Decode the UTF-8 codepoint at s[i]; advances i past it.  Invalid bytes are
// consumed one at a time and returned as their raw value (never happens on
// our own transcoder output).
char32_t utf8_next(const std::string& s, std::size_t& i);

// Count of Japanese characters (hiragana / katakana / CJK unified + ext-A) --
// the single definition of "needs translating", shared by the extractor's
// has_jp gate and the translator's needs_translation gate.
int jp_char_count(const std::string& utf8);
inline bool is_jp(const std::string& utf8) { return jp_char_count(utf8) > 0; }

// True for the whitespace codepoints the trimmers below recognise: the FULL
// Unicode whitespace set -- category Zs (U+3000 IDEOGRAPHIC SPACE included)
// plus the bidi WS/B/S controls.  <cctype>'s isspace() is not a substitute.
bool is_unicode_space(char32_t cp);

// Whitespace trimming over the FULL Unicode whitespace set.  This is NOT the
// same as trimming ASCII whitespace: U+3000 IDEOGRAPHIC SPACE must be stripped
// too, because CJK script text is padded with it freely -- an ASCII-only trim
// would key the TSV on a string the engine never emits.
std::string trim(const std::string& utf8);
std::string trim_left(const std::string& utf8);
std::string trim_right(const std::string& utf8);

// Split on runs of whitespace, never producing an empty token.  Distinct from
// an isspace()-based loop -- U+3000 and U+00A0 are separators here but not to
// <cctype>.
std::vector<std::string> split_whitespace(const std::string& utf8);

// First n CODEPOINTS, not n bytes.  Used for the log-line truncations
// (descriptions and JP/EN samples routinely contain Japanese, so a byte substr
// would both cut a different amount and split a sequence).
std::string utf8_prefix(const std::string& utf8, std::size_t n_codepoints);

// ---- logging ---------------------------------------------------------------

// Pipeline log format: "HH:MM:SS INFO <message>" on stdout.
void log_info(const std::string& msg);
// Bare line, no timestamp -- for banners and prompts.
void print_line(const std::string& msg);
// Write without a trailing newline and flush, for progress on one line.
void print_inline(const std::string& msg);

// Set the console to UTF-8 so Japanese text prints legibly.  Call first in
// main().
void setup_console_utf8();

// Thousands-separated integer, e.g. 13105 -> "13,105".
std::string comma(long long n);

// ---- JSON ------------------------------------------------------------------

// Shortest decimal string that round-trips, in plain (non-scientific) form.
// boost::json serialises every double through ryu, which ALWAYS emits
// scientific notation with a capital 'E' (1.0 -> "1E0", 0.97 -> "9.7E-1");
// the JSON artifacts must carry "1.0" and "0.97" so the confidence and bbox
// values stay readable and re-parse identically everywhere.  Both json_pretty
// and json_dump route doubles through this.
std::string format_float(double d);

// Pretty-printed JSON with raw (non-escaped) UTF-8 and a 2-space indent, for
// the files meant to be read by a human (cache, batch logs).
std::string json_pretty(const boost::json::value& v, int indent = 2);

// COMPACT JSON with raw (non-escaped) UTF-8.  The required separators are
// ", " and ": "; boost::json::serialize emits "," and ":" with no spaces, so
// serialize() alone does NOT produce the expected form.
std::string json_dump(const boost::json::value& v);

boost::json::value json_parse_file(const std::string& path);

}  // namespace mgi
