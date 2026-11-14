// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shared helpers for the Adieu pipeline: file IO, CP932 <-> UTF-8, UTF-16LE
// decoding, Japanese-text classification, timestamped logging, and JSON
// pretty-printing.
//
// Shared verbatim with EXTRAVAGANZA_MATSURO_CE (namespace renamed) plus the
// UTF-16 helpers Adieu's SiglusEngine string pool needs.  The CP932 tables
// are load-bearing across games -- do not re-derive them.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace adi {

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
// run never leaves a half-written file where the next step expects valid JSON.
void write_file_atomic(const std::string& path, const std::string& content);

// CRLF write.  Every JSON artifact this pipeline produces -- extracted_text,
// translated_text, the translation cache and the per-run test logs -- is
// CRLF-terminated, and the committed reference files are keyed on that.
// (Compact JSON contains no newlines at all, so it can use write_file
// directly.)
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

// Strict encode: nullopt when any character has no CP932 mapping.
std::optional<Bytes> utf8_to_cp932_strict(const std::string& s);

// ---- UTF-16LE --------------------------------------------------------------

// UTF-16LE decode with U+FFFD substitution.  Surrogate pairs are combined;
// ONE U+FFFD is emitted per invalid code unit (lone high surrogate, lone low
// surrogate) and one for a trailing odd byte, then decoding resyncs at the
// next unit.  Noncharacters (U+FFFE/U+FFFF) pass through unchanged.
std::string utf16le_decode_replace(const std::uint8_t* p, std::size_t n);

// Thrown by units_to_utf8_direct when a raw code unit lands in D800..DFFF.
// A lone surrogate has no valid UTF-8 form; emitting WTF-8 instead would put
// bytes in extracted_text.json that no JSON reader can round-trip, so the
// decode fails loudly here rather than downstream.
struct LoneSurrogateError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ONE codepoint per u16, no surrogate pairing.  This is what siglus_decoder's
// SS_V2 branch needs -- it is NOT UTF-16 decoding, and using
// utf16le_decode_replace here would change the decoded text of 180 of Adieu's
// 194 scenes.
std::string units_to_utf8_direct(const std::uint16_t* w, std::size_t n);

// UTF-8 decode with U+FFFD substitution: one U+FFFD per maximal invalid
// subpart, per Unicode's substitution-of-maximal-subparts rule.
std::string utf8_decode_replace(const std::uint8_t* p, std::size_t n);

// ---- Unicode helpers -------------------------------------------------------

// Decode the UTF-8 codepoint at s[i]; advances i past it.  Invalid bytes are
// consumed one at a time and returned as their raw value (never happens on
// our own transcoder output).
char32_t utf8_next(const std::string& s, std::size_t& i);

// Codepoint count (R1).  std::string::size() counts BYTES and would
// mis-answer every length threshold in this pipeline.
std::size_t char_len(const std::string& utf8);

// Count of Japanese characters (hiragana / katakana / CJK unified + ext-A) --
// the same ranges extract::has_japanese and translate::has_real_japanese use.
int jp_char_count(const std::string& utf8);
inline bool is_jp(const std::string& utf8) { return jp_char_count(utf8) > 0; }

// True for the FULL Unicode whitespace set -- category Zs plus the bidi
// WS/B/S controls -- U+3000 IDEOGRAPHIC SPACE included.
bool is_unicode_space(char32_t cp);

// Trim whitespace from both ends / the left / the right, using the full
// Unicode whitespace set of is_unicode_space.  This is NOT the same as
// trimming ASCII whitespace: U+3000 IDEOGRAPHIC SPACE must be trimmed too,
// because CJK engines pad with it freely -- an ASCII-only trim would key the
// TSV on a string the engine never emits.
std::string trim(const std::string& utf8);
std::string trim_left(const std::string& utf8);
std::string trim_right(const std::string& utf8);

// The ARGUMENT form of the right-hand trim: removes only the characters given
// and knows nothing about whitespace.  Used to trim NUL padding (note:
// `chars` may contain NUL) and stray '\n' / '\r'.
std::string trim_right_chars(const std::string& utf8, const std::string& chars);

// ---- logging ---------------------------------------------------------------

// Pipeline log format: "HH:MM:SS INFO <message>" on stdout.
void log_info(const std::string& msg);
// Bare line, no timestamp -- for report output that is read as one block.
void print_line(const std::string& msg);
// Write with no trailing newline, flushed immediately (progress output).
void print_inline(const std::string& msg);

// Set the console to UTF-8 so Japanese text prints legibly.  Call first in
// main().
void setup_console_utf8();

// Thousands-separated integer: 1234567 -> "1,234,567".
std::string comma(long long n);

// ---- JSON ------------------------------------------------------------------

// Pretty-printed JSON in raw UTF-8 -- never \uXXXX escapes, because these
// files are read by humans (the cache and the batch logs) and by tools that
// grep them for Japanese.  boost::json::serialize already emits UTF-8; this
// adds the indentation.
std::string json_pretty(const boost::json::value& v, int indent = 2);

// COMPACT JSON in raw UTF-8.  The required separators are ", " and ": ";
// boost::json::serialize emits "," and ":" with no spaces, so serialize()
// alone does NOT produce the expected layout.
std::string json_dump(const boost::json::value& v);

boost::json::value json_parse_file(const std::string& path);

}  // namespace adi
