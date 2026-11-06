// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Shared helpers for the WelcomeToAnzU_CE pipeline: file IO, CP932 <-> UTF-8,
// Japanese-text classification, timestamped logging, and JSON formatting.
#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace exm {

using Bytes = std::vector<std::uint8_t>;

constexpr int kDefaultJsonIndent = 2;

// Non-copyable, movable RAII wrapper for std::FILE*.
class ScopedFile {
public:
    ScopedFile() noexcept = default;
    explicit ScopedFile(std::FILE* fp) noexcept;
    ScopedFile(const std::string& path, const char* mode);
    ~ScopedFile();

    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;
    ScopedFile(ScopedFile&& other) noexcept;
    ScopedFile& operator=(ScopedFile&& other) noexcept;

    std::FILE* get() const noexcept { return fp_; }
    explicit operator bool() const noexcept { return fp_ != nullptr; }
    std::FILE* release() noexcept;
    void reset(std::FILE* fp = nullptr) noexcept;

private:
    std::FILE* fp_ = nullptr;
};

Bytes read_file(const std::string& path);
void write_file(const std::string& path, const void* data, std::size_t size);
inline void write_file(const std::string& path, const Bytes& b) {
    write_file(path, b.data(), b.size());
}
inline void write_file(const std::string& path, const std::string& s) {
    write_file(path, s.data(), s.size());
}
void write_file_atomic(const std::string& path, const std::string& content);

void write_file_text(const std::string& path, const std::string& content);
void write_file_atomic_text(const std::string& path, const std::string& content);

// Strict decode: returns nullopt on any invalid or unmapped sequence.
// Note that 0x80, 0xA0 and 0xFD-0xFF are valid single characters in CP932
// (U+0080, U+F8F0, U+F8F1-U+F8F3) and are mapped explicitly.
std::optional<std::string> cp932_to_utf8_strict(const std::uint8_t* p, std::size_t n);
inline std::optional<std::string> cp932_to_utf8_strict(const Bytes& b) {
    return cp932_to_utf8_strict(b.data(), b.size());
}

// Lossy decode: invalid bytes become U+FFFD.
std::string cp932_to_utf8_replace(const std::uint8_t* p, std::size_t n);

// Lossy encode UTF-8 -> CP932: unmappable characters become '?'.
Bytes utf8_to_cp932_replace(const std::string& s);
inline std::string utf8_to_cp932(const std::string& s) {
    Bytes b = utf8_to_cp932_replace(s);
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

// Strict encode: nullopt when any character has no CP932 mapping.
std::optional<Bytes> utf8_to_cp932_strict(const std::string& s);

// Decode the UTF-8 codepoint at s[i]; advances i past it.
char32_t utf8_next(const std::string& s, std::size_t& i);

// Count of Japanese characters (hiragana / katakana / CJK unified + ext-A).
int jp_char_count(const std::string& utf8);
inline bool is_jp(const std::string& utf8) { return jp_char_count(utf8) > 0; }

// True for every Unicode whitespace codepoint (category Zs plus bidi WS/B/S
// controls), including U+3000 IDEOGRAPHIC SPACE.
bool is_unicode_space(char32_t cp);

std::string trim(const std::string& utf8);
std::string trim_left(const std::string& utf8);
std::string trim_right(const std::string& utf8);

void log_info(const std::string& msg);
void print_line(const std::string& msg);
void print_inline(const std::string& msg);
void setup_console_utf8();

std::string comma(long long n);

std::string json_pretty(const boost::json::value& v, int indent = kDefaultJsonIndent);
std::string json_dump(const boost::json::value& v);
boost::json::value json_parse_file(const std::string& path);

}  // namespace exm
