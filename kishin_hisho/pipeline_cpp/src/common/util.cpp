// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "util.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace ksh {

// ---- file IO ---------------------------------------------------------------

Bytes read_file(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    f.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    Bytes out(size);
    if (size) f.read(reinterpret_cast<char*>(out.data()), size);
    if (!f) throw std::runtime_error("read failed: " + path);
    return out;
}

void write_file(const std::string& path, const void* data, std::size_t size) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot create " + path);
    if (size) f.write(static_cast<const char*>(data), size);
    if (!f) throw std::runtime_error("write failed: " + path);
}

void write_file_text(const std::string& path, const std::string& content) {
    std::string out;
    out.reserve(content.size() + content.size() / 16);
    for (std::size_t i = 0; i < content.size(); ++i) {
        // Never double up an existing CRLF.
        if (content[i] == '\n' && (i == 0 || content[i - 1] != '\r')) out += '\r';
        out += content[i];
    }
    write_file(path, out);
}

// ---- logging ---------------------------------------------------------------

namespace {
std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}
}  // namespace

void log_info(const std::string& msg) {
    std::fputs((timestamp() + " INFO " + msg + "\n").c_str(), stdout);
    std::fflush(stdout);
}

void print_line(const std::string& msg) {
    std::fputs((msg + "\n").c_str(), stdout);
    std::fflush(stdout);
}

void setup_console_utf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

// ---- formatting ------------------------------------------------------------

std::string hexu(std::uint64_t v, int width) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*llX", width, static_cast<unsigned long long>(v));
    return buf;
}

std::string hexl(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string hexu_signed(std::int64_t v, int width) {
    const bool neg = v < 0;
    const auto mag = static_cast<unsigned long long>(neg ? -v : v);
    char buf[32];
    // The sign occupies one column of the padded field, so the magnitude gets
    // one digit less than `width` when negative.
    std::snprintf(buf, sizeof(buf), "%s%0*llX", neg ? "-" : "", neg ? width - 1 : width, mag);
    return buf;
}

std::string hex_bytes(const std::uint8_t* p, std::size_t n) {
    std::string out;
    out.reserve(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        if (i) out += ' ';
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02X", p[i]);
        out += buf;
    }
    return out;
}

std::string hex_lower(const std::uint8_t* p, std::size_t n) {
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02x", p[i]);
        out += buf;
    }
    return out;
}

std::string ljust(const std::string& s, std::size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

std::string bytes_repr(const std::uint8_t* p, std::size_t n) {
    const bool has_sq = std::find(p, p + n, '\'') != p + n;
    const bool has_dq = std::find(p, p + n, '"') != p + n;
    const char quote = (has_sq && !has_dq) ? '"' : '\'';
    std::string out;
    out += 'b';
    out += quote;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t c = p[i];
        if (c == static_cast<std::uint8_t>(quote) || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c < 0x20 || c >= 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += quote;
    return out;
}

}  // namespace ksh
