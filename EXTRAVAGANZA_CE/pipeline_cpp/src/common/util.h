// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shared helpers for the EXTRAVAGANZA_CE pipeline: file IO, CP932 <-> UTF-8,
// Japanese-text classification, timestamped logging, and JSON
// pretty-printing.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace exc {

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
// Atomic replace: write <path>.tmp then rename over <path>, so a run that
// dies mid-write never leaves a half-written cache behind.
void write_file_atomic(const std::string& path, const std::string& content);

// Text-mode write: every '\n' goes out as '\r\n'.  The pretty-printed JSON
// artifacts (extracted/translated text, caches, batch logs) are read and
// diffed as Windows text files, so their line endings must be CRLF.  (Compact
// JSON contains no newlines at all, so it can use write_file directly.)
void write_file_text(const std::string& path, const std::string& content);
void write_file_atomic_text(const std::string& path, const std::string& content);

// ---- CP932 <-> UTF-8 -------------------------------------------------------

// Strict decode: returns nullopt on any invalid or unmapped sequence.
// Strictness is the validity gate the extractor relies on, so it must not be
// relaxed.
//
// NOTE: 0x80, 0xA0 and 0xFD-0xFF ARE valid single CP932 characters
// (U+0080, U+F8F0, U+F8F1-U+F8F3).  Treating them as invalid dropped ~13% of
// the extracted strings in the BLACKCyc games, so they are mapped explicitly;
// MultiByteToWideChar(932) does not accept all of them.
std::optional<std::string> cp932_to_utf8_strict(const std::uint8_t* p, std::size_t n);
inline std::optional<std::string> cp932_to_utf8_strict(const Bytes& b) {
    return cp932_to_utf8_strict(b.data(), b.size());
}

// Lossy decode: invalid bytes become U+FFFD instead of failing the whole run.
std::string cp932_to_utf8_replace(const std::uint8_t* p, std::size_t n);

// Lossy encode UTF-8 -> CP932: unmappable characters become '?'.
Bytes utf8_to_cp932_replace(const std::string& s);

// Strict encode: nullopt when any character has no CP932 mapping.
std::optional<Bytes> utf8_to_cp932_strict(const std::string& s);

// ---- Unicode helpers -------------------------------------------------------

// Decode the UTF-8 codepoint at s[i]; advances i past it.  Invalid bytes are
// consumed one at a time and returned as their raw value (never happens on
// our own transcoder output).
char32_t utf8_next(const std::string& s, std::size_t& i);

// Whole-string codepoint view.  Every length test, slice and index over game
// text is in CHARACTERS -- line budgets, name-plate limits and prompt
// truncation all count characters -- so text work goes through these rather
// than std::string's byte operations.
std::vector<char32_t> utf8_decode(const std::string& s);
std::string utf8_encode(const std::vector<char32_t>& cps);
std::string utf8_encode_cp(char32_t cp);

// Count of Japanese characters (hiragana / katakana / CJK unified + ext-A) --
// the density test behind extraction and needs_translation().
int jp_char_count(const std::string& utf8);
inline bool is_jp(const std::string& utf8) { return jp_char_count(utf8) > 0; }

// True for the FULL Unicode whitespace set, U+3000 IDEOGRAPHIC SPACE included
// -- the CJK engine pads with U+3000, so an ASCII-only notion of space is not
// enough here.  This predicate is what makes trim() below correct.
bool is_unicode_space(char32_t cp);

// Whitespace trim over the full Unicode whitespace set.  This is NOT the same
// as trimming ASCII whitespace: U+3000 IDEOGRAPHIC SPACE, which the scripts
// use freely as padding, must be stripped too -- an ASCII-only trim would key
// the cache and the glossary lookup on a string the engine never emits.
std::string trim(const std::string& utf8);
std::string trim_left(const std::string& utf8);
std::string trim_right(const std::string& utf8);

// ---- logging ---------------------------------------------------------------

// Pipeline log line: "HH:MM:SS INFO <message>" on stdout.
void log_info(const std::string& msg);
// Bare line, no timestamp -- for report output the log format would clutter.
void print_line(const std::string& msg);
// Same, without the trailing newline (progress counters overwriting a line).
void print_inline(const std::string& msg);

// Set the console to UTF-8 so Japanese text prints legibly.  Call first in
// main().
void setup_console_utf8();

// Thousands-separated integer, e.g. 141239 -> "141,239".
std::string comma(long long n);

// ---- JSON ------------------------------------------------------------------

// Pretty printer for the artifacts that are meant to be read and diffed by
// hand (extracted/translated text, caches, batch logs).  Japanese stays raw
// UTF-8 rather than \u-escaped, which boost::json::serialize already does.
std::string json_pretty(const boost::json::value& v, int indent = 2);

// Compact form, separators ", " and ": " -- boost::json::serialize emits ","
// and ":" with no spaces, so it cannot be used directly.
std::string json_dump(const boost::json::value& v);

boost::json::value json_parse_file(const std::string& path);

}  // namespace exc
