// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shared helpers: file I/O, UTF-8 / JP classification, ASCII sanitization,
// console logging, and JSON pretty-printing.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace spot_checker {

using Bytes = std::vector<std::uint8_t>;

// ---- file I/O --------------------------------------------------------------

Bytes read_file(const std::string& path);
std::string read_file_text(const std::string& path);

void write_file(const std::string& path, const void* data, std::size_t size);
inline void write_file(const std::string& path, const Bytes& b) {
    write_file(path, b.data(), b.size());
}
inline void write_file(const std::string& path, const std::string& s) {
    write_file(path, s.data(), s.size());
}

// Atomic replace: write <path>.tmp then rename over <path>.
void write_file_atomic(const std::string& path, const std::string& content);

// ---- UTF-8 & string helpers ------------------------------------------------

char32_t utf8_next(const std::string& s, std::size_t& i);
std::size_t utf8_char_len(const std::string& s);
bool has_japanese(const std::string& utf8);
bool has_real_japanese(const std::string& utf8);

std::string trim_left(const std::string& s);
std::string trim_right(const std::string& s);
std::string trim(const std::string& s);

// Replace typographic dashes, ellipses, curly quotes, and ideographic spaces
// with plain ASCII equivalents safe for CP932 visual novel engines.
std::string sanitize_ascii(const std::string& text);

// ---- console & logging -----------------------------------------------------

void setup_console_utf8();
void log_info(const std::string& msg);
void print_line(const std::string& msg);

// ---- JSON ------------------------------------------------------------------

struct JsonFormatStyle {
    int indent = 1;
    bool use_crlf = true;
    bool trailing_newline = false;
};

std::string json_pretty(const boost::json::value& v, int indent = 2);
JsonFormatStyle detect_json_format_style(const std::string& path,
                                         int default_indent = 1,
                                         bool default_crlf = true);
void write_json_preserving_style(const std::string& path,
                                 const boost::json::value& v,
                                 int default_indent = 1,
                                 bool default_crlf = true);
boost::json::value json_parse_file(const std::string& path);

}  // namespace spot_checker
