// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shared helpers: file IO, CP932 codec, Unicode, logging, JSON.
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
// Write via .tmp + rename so an interrupted run cannot leave a half-written file.
void write_file_atomic(const std::string& path, const std::string& content);

// CRLF variants — script_output/ JSON uses CRLF for stable digests.
void write_file_text(const std::string& path, const std::string& content);
void write_file_atomic_text(const std::string& path, const std::string& content);

// ---- CP932 <-> UTF-8 -------------------------------------------------------

// Strict decode; nullopt on invalid sequences.
// Maps 0x80/0xA0/0xFD-0xFF explicitly — MultiByteToWideChar(932) disagrees on them.
std::optional<std::string> cp932_to_utf8_strict(const std::uint8_t* p, std::size_t n);
inline std::optional<std::string> cp932_to_utf8_strict(const Bytes& b) {
    return cp932_to_utf8_strict(b.data(), b.size());
}

std::string cp932_to_utf8_replace(const std::uint8_t* p, std::size_t n);
Bytes utf8_to_cp932_replace(const std::string& s);

// Table-driven, not WideCharToMultiByte — the OS codec best-fits accented
// Latin to bare ASCII, which would silently corrupt "Fraternité".
std::optional<Bytes> utf8_to_cp932_strict(const std::string& s);

bool cp932_encodable(char32_t cp);

// ---- Unicode helpers -------------------------------------------------------

// Decode one codepoint at s[i]; advances i past it.
char32_t utf8_next(const std::string& s, std::size_t& i);

std::vector<char32_t> utf8_decode(const std::string& s);
std::string utf8_encode(const std::vector<char32_t>& cps);
std::string utf8_encode_cp(char32_t cp);

std::size_t char_len(const std::string& utf8);
std::string cp_substr(const std::string& utf8, std::size_t cp_begin,
                      std::size_t cp_end = std::string::npos);

int jp_char_count(const std::string& utf8);
inline bool is_jp(const std::string& utf8) { return jp_char_count(utf8) > 0; }

// Full Unicode whitespace (category Zs + bidi controls).
bool is_unicode_space(char32_t cp);

// Full-Unicode trim — U+3000 must go; CJK engines pad with it.
std::string trim(const std::string& utf8);
std::string trim_left(const std::string& utf8);
std::string trim_right(const std::string& utf8);

// Escaped display form for diagnostics; non-ASCII emitted raw.
std::string quote_repr(const std::string& utf8);

std::string pad_left_cp(const std::string& utf8, std::size_t width);
std::string pad_right_cp(const std::string& utf8, std::size_t width);

// ---- logging ---------------------------------------------------------------

void log_info(const std::string& msg);
void log_warning(const std::string& msg);
void print_line(const std::string& msg);
void print_inline(const std::string& msg);
void setup_console_utf8();
std::string comma(long long n);

// ---- JSON ------------------------------------------------------------------

std::string json_pretty(const boost::json::value& v, int indent = 2);
// Compact form with ", " and ": " separators (not bare serialize()).
std::string json_dump(const boost::json::value& v);
boost::json::value json_parse_file(const std::string& path);

}  // namespace frat
