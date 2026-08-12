// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shared helpers for the shingakkou pipeline: file IO, CP932 <-> UTF-8,
// Japanese-text classification, timestamped logging, and JSON
// pretty-printing.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace shin {

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
// Atomic replace: write <path>.tmp then rename over <path>.
void write_file_atomic(const std::string& path, const std::string& content);

// Text-mode write: every '\n' becomes '\r\n'.  Every pretty-printed JSON
// output of this pipeline is CRLF; that is the on-disk format, so a writer
// that emits bare LF produces a file the rest of the tooling treats as
// different content.  (Compact JSON contains no newlines at all, so it can use
// write_file directly.)
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

// Encode UTF-8 -> CP932, substituting '?' for unmappable characters.
Bytes utf8_to_cp932_replace(const std::string& s);

// Strict encode: nullopt when any character has no CP932 mapping.
std::optional<Bytes> utf8_to_cp932_strict(const std::string& s);

// Text-mode write prefixed with a UTF-8 BOM.  The translation TSV is the only
// output in the whole repo that needs it, and the proxy DLL's TSV parse depends
// on both halves -- drop the BOM and the first JP key never matches at runtime;
// drop the CRLF and no row does.
inline void write_file_text_bom(const std::string& p, const std::string& c) {
    // The BOM carries no '\n', so prepending before the LF->CRLF pass is safe.
    write_file_text(p, std::string("\xEF\xBB\xBF") + c);
}

// ---- Unicode helpers -------------------------------------------------------

// Decode the UTF-8 codepoint at s[i]; advances i past it.  Invalid bytes are
// consumed one at a time and returned as their raw value (never happens on
// our own transcoder output).
char32_t utf8_next(const std::string& s, std::size_t& i);

// String length in CODEPOINTS, not bytes.  Six separate length tests in this
// pipeline gate on it (speaker split <=20, is_control_code <50, the <=3 rule,
// the gate's repr field widths), and each one silently reclassifies real data
// if it counts bytes -- a 10-character Japanese speaker name is 30 bytes.
std::size_t char_len(const std::string& utf8);

// Prefix of the first n CODEPOINTS.  Used for the API prompt's
// 100-char context truncation, so a byte cut would both shorten the context
// Claude sees and put a split UTF-8 sequence in the request body.
std::string utf8_prefix(const std::string& utf8, std::size_t n);

// One UTF-16 CODE UNIT -> UTF-8.
//
// Extraction builds each script string one raw code unit at a time, so a
// surrogate PAIR becomes two lone-surrogate codepoints rather than one astral
// character.  Combining them "correctly" would emit different bytes than the
// JSON keys the rest of the pipeline matches on.  (sin_text.dat contains zero
// surrogates, so the WTF-8 path is dead in practice -- it exists to keep the
// mapping honest.)
void append_utf16_unit_as_utf8(std::uint16_t u, std::string& out);

// Count of Japanese characters (hiragana / katakana / CJK unified + ext-A) --
// the shared basis for the extractor's has-JP test and the translator's
// needs-translation test.
int jp_char_count(const std::string& utf8);
inline bool is_jp(const std::string& utf8) { return jp_char_count(utf8) > 0; }

// True for the FULL Unicode whitespace set, U+3000 IDEOGRAPHIC SPACE included
// -- not just the ASCII blanks.  This is the shared predicate behind trim* and
// split_whitespace below.
bool is_unicode_space(char32_t cp);

// Trim whitespace off both ends / the left / the right.  Whitespace here is the
// FULL Unicode set, not the ASCII one: U+3000 IDEOGRAPHIC SPACE must go too,
// because the CJK engine pads with it -- an ASCII-only trim would key the TSV
// on a string the engine never emits, and every lookup would miss.
std::string trim(const std::string& utf8);
std::string trim_left(const std::string& utf8);
std::string trim_right(const std::string& utf8);

// Split on runs of whitespace, never producing an empty token.  Distinct from
// a <cctype> isspace() loop -- U+3000 and U+00A0 are separators here but not
// to <cctype>.
std::vector<std::string> split_whitespace(const std::string& utf8);

// ---- logging ---------------------------------------------------------------

// Pipeline log format: "HH:MM:SS INFO <message>" on stdout.
void log_info(const std::string& msg);
// Bare line, no timestamp.
void print_line(const std::string& msg);
// Write with no trailing newline, flushed immediately (in-place progress).
void print_inline(const std::string& msg);

// Set the console to UTF-8 so Japanese text prints legibly.  Call first in
// main().
void setup_console_utf8();

// Thousands-separated integer: 52840 -> "52,840".
std::string comma(long long n);

// ---- JSON ------------------------------------------------------------------

// Pretty printer for the human-readable outputs (cache, batch logs): 2-space
// indent, and raw UTF-8 rather than \u escapes so Japanese stays legible in
// the file.
std::string json_pretty(const boost::json::value& v, int indent = 2);

// COMPACT serialiser, also raw UTF-8.  The required separators are ", " and
// ": "; boost::json::serialize emits "," and ":" with no spaces, so
// serialize() alone does NOT produce this pipeline's output format.
std::string json_dump(const boost::json::value& v);

boost::json::value json_parse_file(const std::string& path);

}  // namespace shin
