// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shared helpers for the FRATERNITE_HD pipeline: file IO, CP932 <-> UTF-8,
// Japanese-text classification, logging in the pipeline's stdout format,
// and JSON pretty-printing.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace frat {

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
// run never leaves a half-written cache behind.
void write_file_atomic(const std::string& path, const std::string& content);

// CRLF text write.  Every pretty-printed JSON artifact under script_output/ is
// stored with CRLF line endings, so writing one with bare LF would rewrite the
// whole file on the next run and make its digest unstable.  (Compact JSON
// contains no newlines at all, so it can use write_file directly.)
void write_file_text(const std::string& path, const std::string& content);
void write_file_atomic_text(const std::string& path, const std::string& content);

// ---- CP932 <-> UTF-8 -------------------------------------------------------

// Strict decode: returns nullopt on any invalid or unmapped sequence.
//
// NOTE: CP932 maps 0x80, 0xA0 and 0xFD-0xFF to real single characters
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
//
// This is table-driven rather than a WideCharToMultiByte(932) call.  The OS
// codec BEST-FITS 458 BMP codepoints (U+00E9 -> 'e', U+00A9 -> 'c', the whole
// accented-Latin block -> bare ASCII) and reports used_default == FALSE for
// them, so an unmappable character silently becomes a different, plausible
// one; WC_NO_BEST_FIT_CHARS overcorrects the other way, rejecting six
// codepoints CP932 does map and emitting 0xFAxx where CP932 assigns 0xEExx for
// the NEC-selected IBM kanji.  The game is French-titled, so accented Latin in
// the English side is routine -- a best-fit would silently ship "Fraternite"
// instead of the honest "Fraternit?" the engine can actually render.
std::optional<Bytes> utf8_to_cp932_strict(const std::string& s);

// True when the codepoint has a CP932 mapping (same table as the encoder).
bool cp932_encodable(char32_t cp);

// ---- Unicode helpers -------------------------------------------------------

// Decode the UTF-8 codepoint at s[i]; advances i past it.  Invalid bytes are
// consumed one at a time and returned as their raw value (never happens on
// our own transcoder output).
char32_t utf8_next(const std::string& s, std::size_t& i);

// Whole-string codepoint view.  Every length, slice and index in this pipeline
// is counted in CODEPOINTS (the text is Japanese, so a byte count would be
// 2-3x off), which is why they all go through these rather than std::string's
// byte operations.
std::vector<char32_t> utf8_decode(const std::string& s);
std::string utf8_encode(const std::vector<char32_t>& cps);
std::string utf8_encode_cp(char32_t cp);

// String length in codepoints, not bytes.
std::size_t char_len(const std::string& utf8);
// Substring over a codepoint range (end == npos means "to the end").
std::string cp_substr(const std::string& utf8, std::size_t cp_begin,
                      std::size_t cp_end = std::string::npos);

// Count of Japanese characters (hiragana / katakana / CJK unified + ext-A) --
// the shared basis for the extractor's has_japanese and the translator's
// has_real_japanese gates.
int jp_char_count(const std::string& utf8);
inline bool is_jp(const std::string& utf8) { return jp_char_count(utf8) > 0; }

// The whitespace set the trim helpers below use: the FULL Unicode whitespace
// set -- category Zs plus the bidi WS/B/S controls.
bool is_unicode_space(char32_t cp);

// Whitespace trimming over that same full Unicode set.  This is NOT the same as
// trimming ASCII whitespace: U+3000 IDEOGRAPHIC SPACE must go too, because CJK
// engines pad with it freely -- an ASCII-only trim would key the TSV and every
// other lookup table on a string the engine never emits.
std::string trim(const std::string& utf8);
std::string trim_left(const std::string& utf8);
std::string trim_right(const std::string& utf8);

// Quoted, escaped display form of a string: quote selection, backslash/quote/
// control escapes, non-ASCII emitted raw.  The speaker gate prints names this
// way so an invisible character in a name is visible in its report.
std::string quote_repr(const std::string& utf8);

// Right-/left-align into a field of `width`.  Counts CODEPOINTS (R1); a string
// longer than the field is returned unchanged rather than truncated.
std::string pad_left_cp(const std::string& utf8, std::size_t width);
std::string pad_right_cp(const std::string& utf8, std::size_t width);

// ---- logging ---------------------------------------------------------------

// Pipeline log format: "HH:MM:SS INFO <message>" on stdout.
void log_info(const std::string& msg);
// Same, with a WARNING level token.  These lines are NOT interchangeable with
// log_info: the level is what makes a degraded batch findable in a run log.
void log_warning(const std::string& msg);
// Bare line, no timestamp -- for report and table output.
void print_line(const std::string& msg);
// Line fragment: no newline, flushed immediately (progress counters).
void print_inline(const std::string& msg);

// Set the console to UTF-8 so Japanese text prints legibly instead of as
// mojibake.  Call first in main().
void setup_console_utf8();

// Thousands-separated integer: 1234567 -> "1,234,567".
std::string comma(long long n);

// ---- JSON ------------------------------------------------------------------

// Pretty-printed JSON with raw (non-escaped) UTF-8, so the Japanese in
// script_output/ stays readable.  boost::json::serialize already emits raw
// UTF-8; this adds the indent=N layout on top.
std::string json_pretty(const boost::json::value& v, int indent = 2);

// The COMPACT form of the same output format.  Its separators are ", " and
// ": " -- boost::json::serialize emits "," and ":" with no spaces, so
// serialize() alone does NOT produce the pipeline's on-disk format.
std::string json_dump(const boost::json::value& v);

boost::json::value json_parse_file(const std::string& path);

}  // namespace frat
