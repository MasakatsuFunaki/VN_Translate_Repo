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

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace snu {

namespace {

constexpr std::uint8_t kMaxAscii = 0x7F;
constexpr std::uint8_t kMinHalfwidthKana = 0xA1;
constexpr std::uint8_t kMaxHalfwidthKana = 0xDF;
constexpr std::uint8_t kMinLeadRange1 = 0x81;
constexpr std::uint8_t kMaxLeadRange1 = 0x9F;
constexpr std::uint8_t kMinLeadRange2 = 0xE0;
constexpr std::uint8_t kMaxLeadRange2 = 0xFC;
constexpr std::uint8_t kMinTrailRange1 = 0x40;
constexpr std::uint8_t kMaxTrailRange1 = 0x7E;
constexpr std::uint8_t kMinTrailRange2 = 0x80;
constexpr std::uint8_t kMaxTrailRange2 = 0xFC;
constexpr std::size_t kSingleByteStep = 1;
constexpr std::size_t kDoubleByteStep = 2;
constexpr UINT kCodePageCp932 = 932;

// UTF-8 encoding of U+FFFD REPLACEMENT CHARACTER.
constexpr std::array<char, 3> kUtf8ReplacementBytes = {
    static_cast<char>(0xEF),
    static_cast<char>(0xBF),
    static_cast<char>(0xBD),
};

constexpr std::uint8_t kUtf8AsciiLimit = 0x80;
constexpr std::uint8_t kUtf8TwoByteLead = 0xC0;
constexpr std::uint8_t kUtf8ThreeByteLead = 0xE0;
constexpr std::uint8_t kUtf8FourByteLead = 0xF0;
constexpr std::uint8_t kUtf8LeadPayloadBaseMask = 0x3F;
constexpr std::uint8_t kUtf8ContPayloadMask = 0x3F;
constexpr int kUtf8ContShiftBits = 6;
constexpr int kUtf8OneExtra = 1;
constexpr int kUtf8TwoExtra = 2;
constexpr int kUtf8ThreeExtra = 3;

constexpr char32_t kMinHiragana = 0x3040;
constexpr char32_t kMaxHiragana = 0x309F;
constexpr char32_t kMinKatakana = 0x30A0;
constexpr char32_t kMaxKatakana = 0x30FF;
constexpr char32_t kMinCjkExtA = 0x3400;
constexpr char32_t kMaxCjkExtA = 0x4DBF;
constexpr char32_t kMinCjkUnified = 0x4E00;
constexpr char32_t kMaxCjkUnified = 0x9FFF;
constexpr char32_t kMinPrintableControl = 0x20;

constexpr std::size_t kTimestampBufferSize = 16;

// Validate CP932 byte structure before handing to MultiByteToWideChar,
// because MB_ERR_INVALID_CHARS accepts some invalid bytes (e.g. lone 0x80).
bool cp932_structure_valid(const std::uint8_t* p, std::size_t n) {
    std::size_t i = 0;
    while (i < n) {
        const std::uint8_t b = p[i];
        if (b <= kMaxAscii || (b >= kMinHalfwidthKana && b <= kMaxHalfwidthKana)) {
            ++i;
            continue;
        }
        const bool is_lead =
            (b >= kMinLeadRange1 && b <= kMaxLeadRange1) ||
            (b >= kMinLeadRange2 && b <= kMaxLeadRange2);
        if (!is_lead || i + 1 >= n) return false;

        const std::uint8_t t = p[i + 1];
        const bool valid_trail =
            (t >= kMinTrailRange1 && t <= kMaxTrailRange1) ||
            (t >= kMinTrailRange2 && t <= kMaxTrailRange2);
        if (!valid_trail) return false;
        i += kDoubleByteStep;
    }
    return true;
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(
        CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[kTimestampBufferSize];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void pretty_impl(const boost::json::value& v, std::string& out,
                 int indent, int depth) {
    namespace bj = boost::json;
    const std::string pad(static_cast<std::size_t>(indent) * depth, ' ');
    const std::string pad1(static_cast<std::size_t>(indent) * (depth + 1), ' ');
    switch (v.kind()) {
    case bj::kind::object: {
        const auto& o = v.get_object();
        if (o.empty()) {
            out += "{}";
            return;
        }
        out += "{\n";
        bool first = true;
        for (const auto& kv : o) {
            if (!first) out += ",\n";
            first = false;
            out += pad1;
            out += bj::serialize(bj::value(kv.key()));
            out += ": ";
            pretty_impl(kv.value(), out, indent, depth + 1);
        }
        out += "\n" + pad + "}";
        return;
    }
    case bj::kind::array: {
        const auto& a = v.get_array();
        if (a.empty()) {
            out += "[]";
            return;
        }
        out += "[\n";
        bool first = true;
        for (const auto& e : a) {
            if (!first) out += ",\n";
            first = false;
            out += pad1;
            pretty_impl(e, out, indent, depth + 1);
        }
        out += "\n" + pad + "]";
        return;
    }
    default:
        out += bj::serialize(v);
    }
}

}  // namespace

Bytes read_file(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    f.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    Bytes out(size);
    if (size) {
        f.read(reinterpret_cast<char*>(out.data()),
               static_cast<std::streamsize>(size));
    }
    if (!f) throw std::runtime_error("read failed: " + path);
    return out;
}

void write_file(const std::string& path, const void* data, std::size_t size) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot create " + path);
    if (size) {
        f.write(static_cast<const char*>(data),
                static_cast<std::streamsize>(size));
    }
    if (!f) throw std::runtime_error("write failed: " + path);
}

void write_file_atomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    write_file(tmp, content);
    std::filesystem::rename(std::filesystem::u8path(tmp),
                            std::filesystem::u8path(path));
}

std::optional<std::string> cp932_to_utf8_strict(const std::uint8_t* p,
                                                std::size_t n) {
    if (n == 0) return std::string();
    if (!cp932_structure_valid(p, n)) return std::nullopt;
    const int len = MultiByteToWideChar(
        kCodePageCp932, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(p), static_cast<int>(n), nullptr, 0);
    if (len <= 0) return std::nullopt;
    std::wstring w(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(
        kCodePageCp932, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(p), static_cast<int>(n), w.data(), len);
    return wide_to_utf8(w);
}

std::string cp932_to_utf8_replace(const std::uint8_t* p, std::size_t n) {
    // Emit U+FFFD per undecodable byte; slide one byte on failure to keep
    // offsets aligned.
    std::string out;
    std::size_t i = 0;
    while (i < n) {
        const std::uint8_t b = p[i];
        const std::size_t step =
            (b <= kMaxAscii || (b >= kMinHalfwidthKana && b <= kMaxHalfwidthKana))
                ? kSingleByteStep
                : kDoubleByteStep;
        if (i + step <= n) {
            if (auto piece = cp932_to_utf8_strict(p + i, step)) {
                out += *piece;
                i += step;
                continue;
            }
        }
        out.append(kUtf8ReplacementBytes.data(), kUtf8ReplacementBytes.size());
        ++i;
    }
    return out;
}

Bytes utf8_to_cp932_replace(const std::string& s) {
    const std::wstring w = utf8_to_wide(s);
    if (w.empty()) return {};
    BOOL used_default = FALSE;
    const int len = WideCharToMultiByte(
        kCodePageCp932, 0, w.data(), static_cast<int>(w.size()),
        nullptr, 0, nullptr, &used_default);
    Bytes out(static_cast<std::size_t>(len));
    WideCharToMultiByte(
        kCodePageCp932, 0, w.data(), static_cast<int>(w.size()),
        reinterpret_cast<char*>(out.data()), len, nullptr, nullptr);
    return out;
}

char32_t utf8_next(const std::string& s, std::size_t& i) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
    const std::uint8_t b = p[i];
    if (b < kUtf8AsciiLimit) {
        ++i;
        return b;
    }
    const int extra =
        (b >= kUtf8FourByteLead) ? kUtf8ThreeExtra
        : (b >= kUtf8ThreeByteLead) ? kUtf8TwoExtra
        : (b >= kUtf8TwoByteLead) ? kUtf8OneExtra
        : 0;
    if (extra == 0 || i + static_cast<std::size_t>(extra) >= s.size()) {
        ++i;
        return b;
    }
    char32_t cp = b & (kUtf8LeadPayloadBaseMask >> extra);
    for (int k = 1; k <= extra; ++k) {
        cp = (cp << kUtf8ContShiftBits) |
             (p[i + static_cast<std::size_t>(k)] & kUtf8ContPayloadMask);
    }
    i += static_cast<std::size_t>(extra) + 1;
    return cp;
}

int jp_char_count(const std::string& utf8) {
    int n = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        if ((cp >= kMinHiragana && cp <= kMaxHiragana) ||
            (cp >= kMinKatakana && cp <= kMaxKatakana) ||
            (cp >= kMinCjkUnified && cp <= kMaxCjkUnified) ||
            (cp >= kMinCjkExtA && cp <= kMaxCjkExtA)) {
            ++n;
        }
    }
    return n;
}

bool is_clean_text(const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        if (cp < kMinPrintableControl && cp != U'\t' && cp != U'\r' &&
            cp != U'\n') {
            return false;
        }
    }
    return true;
}

void log_info(const std::string& msg) {
    std::fputs((timestamp() + " INFO " + msg + "\n").c_str(), stdout);
    std::fflush(stdout);
}

void print_line(const std::string& msg) {
    std::fputs((msg + "\n").c_str(), stdout);
    std::fflush(stdout);
}

void print_inline(const std::string& msg) {
    std::fputs(msg.c_str(), stdout);
    std::fflush(stdout);
}

void setup_console_utf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

std::string json_pretty(const boost::json::value& v, int indent) {
    std::string out;
    pretty_impl(v, out, indent, 0);
    return out;
}

boost::json::value json_parse_file(const std::string& path) {
    const Bytes raw = read_file(path);
    try {
        return boost::json::parse(boost::json::string_view(
            reinterpret_cast<const char*>(raw.data()), raw.size()));
    } catch (const std::exception& e) {
        // Boost names the offset and its own header, never the file.  A run
        // reads several JSON files, so the diagnosis says which one is bad.
        throw std::runtime_error("cannot parse " + path + ": " + e.what());
    }
}

}  // namespace snu
