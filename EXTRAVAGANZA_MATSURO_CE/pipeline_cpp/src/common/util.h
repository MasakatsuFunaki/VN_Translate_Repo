// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shared helpers for the EXTRAVAGANZA_MATSURO_CE pipeline: file IO, CP932 <-> UTF-8,
// Japanese-text classification, timestamped logging, and JSON pretty-printing
// (2-space indent).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace exm {

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
// Atomic replace: write <path>.tmp then rename over <path>, so a crash mid-write
// never leaves a truncated file behind.
void write_file_atomic(const std::string& path, const std::string& content);

// TEXT-mode write: every '\n' becomes '\r\n'.  The pretty-printed JSON files are
// the pipeline's hand-inspectable artifacts (cache, batch logs, extracted and
// translated text) and must stay CRLF so Windows editors and diff tools show
// them as one line per entry.  (Compact JSON contains no newlines at all, so it
// can use write_file directly.)
void write_file_text(const std::string& path, const std::string& content);
void write_file_atomic_text(const std::string& path, const std::string& content);

// ---- CP932 <-> UTF-8 -------------------------------------------------------

// Strict decode: returns nullopt on any invalid or unmapped sequence.
//
// NOTE: 0x80, 0xA0 and 0xFD-0xFF ARE valid single characters in CP932
// (U+0080, U+F8F0, U+F8F1-U+F8F3).  Treating them as invalid dropped ~13% of
// the extracted strings in the BLACKCyc games, so they are mapped explicitly;
// MultiByteToWideChar(932) does not handle all of them the same way.
std::optional<std::string> cp932_to_utf8_strict(const std::uint8_t* p, std::size_t n);
inline std::optional<std::string> cp932_to_utf8_strict(const Bytes& b) {
    return cp932_to_utf8_strict(b.data(), b.size());
}

// Lossy decode: invalid bytes become U+FFFD.
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

// Count of Japanese characters (hiragana / katakana / CJK unified + ext-A) --
// the shared basis for the extractor's has_jp flag and the translator's
// needs_translation().
int jp_char_count(const std::string& utf8);
inline bool is_jp(const std::string& utf8) { return jp_char_count(utf8) > 0; }

// True for every Unicode whitespace codepoint (category Zs plus the bidi
// WS/B/S controls), not just the ASCII ones -- U+3000 IDEOGRAPHIC SPACE
// included.  The trim helpers below depend on that full coverage.
bool is_unicode_space(char32_t cp);

// Trim whitespace from both ends / the left / the right.  These must recognise
// the FULL Unicode whitespace set, U+3000 IDEOGRAPHIC SPACE above all: CJK
// engines pad text with it, so an ASCII-only trim would leave it in place and
// key the TSV lookup on a string the engine never emits.
std::string trim(const std::string& utf8);
std::string trim_left(const std::string& utf8);
std::string trim_right(const std::string& utf8);

// ---- logging ---------------------------------------------------------------

// Pipeline log format: "HH:MM:SS INFO <message>" on stdout.
void log_info(const std::string& msg);
// Bare line, no timestamp -- for report output that is read as a block.
void print_line(const std::string& msg);
// Write without a trailing newline and flush (progress counters).
void print_inline(const std::string& msg);

// Set the console to UTF-8 so Japanese text prints legibly instead of as
// mojibake.  Call first in main().
void setup_console_utf8();

// Thousands-separated integer, e.g. 323299 -> "323,299".
std::string comma(long long n);

// ---- JSON ------------------------------------------------------------------

// Pretty printer for the human-readable artifacts (cache, batch logs): raw
// UTF-8, never \uXXXX escapes, so the Japanese stays legible in an editor.
// boost::json::serialize already emits raw UTF-8; this adds the indenting.
std::string json_pretty(const boost::json::value& v, int indent = 2);

// COMPACT form, same raw-UTF-8 rule.  The required separators are ", " and
// ": " (with the spaces); boost::json::serialize emits "," and ":" with no
// spaces, so serialize() alone will not do.
std::string json_dump(const boost::json::value& v);

boost::json::value json_parse_file(const std::string& path);

}  // namespace exm
