// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

namespace oretsuba {

using Bytes = std::vector<std::uint8_t>;

constexpr int kDefaultJsonIndent = 2;

Bytes ReadFileBytes(const std::string& path);
void WriteFileBytes(const std::string& path, const void* data, std::size_t size);
void WriteFileBytes(const std::string& path, const Bytes& bytes);
void WriteFileBytes(const std::string& path, std::string_view content);
void WriteFileAtomic(const std::string& path, std::string_view content);
void WriteFileText(const std::string& path, std::string_view content);
void WriteFileAtomicText(const std::string& path, std::string_view content);

std::optional<std::string> Cp932ToUtf8Strict(const std::uint8_t* data, std::size_t size);
std::optional<std::string> Cp932ToUtf8Strict(const Bytes& bytes);
std::optional<std::string> Cp932ToUtf8Strict(std::string_view raw_bytes);
std::string Cp932ToUtf8(const std::uint8_t* data, std::size_t size);
std::string Cp932ToUtf8(const Bytes& bytes);
std::string Cp932ToUtf8(std::string_view raw_bytes);
Bytes Utf8ToCp932(std::string_view utf8);

char32_t Utf8NextCodepoint(std::string_view utf8, std::size_t& index);
int JapaneseCharCount(std::string_view utf8);
bool HasJapanese(std::string_view utf8);
bool HasSobJapanese(std::string_view utf8);
bool IsPrintableScriptText(std::string_view utf8);
bool IsUnicodeSpace(char32_t codepoint);
std::string TrimUnicode(std::string_view utf8);

void LogInfo(std::string_view message);
void PrintLine(std::string_view message);
void SetupConsoleUtf8();
std::string FormatWithCommas(long long value);

std::string JsonPretty(const boost::json::value& value, int indent = kDefaultJsonIndent);
std::string JsonDump(const boost::json::value& value);
boost::json::value JsonParseFile(const std::string& path);

inline Bytes read_file(const std::string& path) {
    return ReadFileBytes(path);
}

inline void write_file(const std::string& path, const void* data, std::size_t size) {
    WriteFileBytes(path, data, size);
}

inline void write_file(const std::string& path, const Bytes& bytes) {
    WriteFileBytes(path, bytes);
}

inline void write_file(const std::string& path, std::string_view content) {
    WriteFileBytes(path, content);
}

inline void write_file_atomic(const std::string& path, std::string_view content) {
    WriteFileAtomic(path, content);
}

inline void write_file_text(const std::string& path, std::string_view content) {
    WriteFileText(path, content);
}

inline void write_file_atomic_text(const std::string& path, std::string_view content) {
    WriteFileAtomicText(path, content);
}

inline std::optional<std::string> cp932_to_utf8_strict(const std::uint8_t* data, std::size_t size) {
    return Cp932ToUtf8Strict(data, size);
}

inline std::optional<std::string> cp932_to_utf8_strict(const Bytes& bytes) {
    return Cp932ToUtf8Strict(bytes);
}

inline std::string cp932_to_utf8_replace(const std::uint8_t* data, std::size_t size) {
    return Cp932ToUtf8(data, size);
}

inline Bytes utf8_to_cp932_replace(std::string_view utf8) {
    return Utf8ToCp932(utf8);
}

inline char32_t utf8_next(std::string_view utf8, std::size_t& index) {
    return Utf8NextCodepoint(utf8, index);
}

inline int jp_char_count(std::string_view utf8) {
    return JapaneseCharCount(utf8);
}

inline bool is_jp(std::string_view utf8) {
    return HasJapanese(utf8);
}

inline bool is_unicode_space(char32_t codepoint) {
    return IsUnicodeSpace(codepoint);
}

inline std::string trim(std::string_view utf8) {
    return TrimUnicode(utf8);
}

inline void log_info(std::string_view message) {
    LogInfo(message);
}

inline void print_line(std::string_view message) {
    PrintLine(message);
}

inline void setup_console_utf8() {
    SetupConsoleUtf8();
}

inline std::string comma(long long value) {
    return FormatWithCommas(value);
}

inline std::string json_pretty(const boost::json::value& value, int indent = kDefaultJsonIndent) {
    return JsonPretty(value, indent);
}

inline std::string json_dump(const boost::json::value& value) {
    return JsonDump(value);
}

inline boost::json::value json_parse_file(const std::string& path) {
    return JsonParseFile(path);
}

}  // namespace oretsuba
