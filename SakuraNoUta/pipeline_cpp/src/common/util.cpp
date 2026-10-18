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

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace snu {

// ---- file IO ---------------------------------------------------------------

Bytes read_file(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    f.seekg(0, std::ios::end);
    auto size = static_cast<std::size_t>(f.tellg());
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

void write_file_atomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    write_file(tmp, content);
    std::filesystem::rename(std::filesystem::u8path(tmp),
                            std::filesystem::u8path(path));
}

// ---- CP932 <-> UTF-8 -------------------------------------------------------

namespace {

// Validate CP932 byte structure before handing to MultiByteToWideChar,
// because MB_ERR_INVALID_CHARS accepts some invalid bytes (e.g. lone 0x80).
bool cp932_structure_valid(const std::uint8_t* p, std::size_t n) {
    std::size_t i = 0;
    while (i < n) {
        std::uint8_t b = p[i];
        if (b <= 0x7F || (b >= 0xA1 && b <= 0xDF)) {
            ++i;
        } else if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) {
            if (i + 1 >= n) return false;
            std::uint8_t t = p[i + 1];
            if (!((t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFC)))
                return false;
            i += 2;
        } else {
            return false;
        }
    }
    return true;
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                  nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

}  // namespace

std::optional<std::string> cp932_to_utf8_strict(const std::uint8_t* p, std::size_t n) {
    if (n == 0) return std::string();
    if (!cp932_structure_valid(p, n)) return std::nullopt;
    int len = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
                                  reinterpret_cast<const char*>(p),
                                  static_cast<int>(n), nullptr, 0);
    if (len <= 0) return std::nullopt;
    std::wstring w(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
                        reinterpret_cast<const char*>(p),
                        static_cast<int>(n), w.data(), len);
    return wide_to_utf8(w);
}

std::string cp932_to_utf8_replace(const std::uint8_t* p, std::size_t n) {
    // Emit U+FFFD per undecodable byte; slide one byte on failure to keep
    // offsets aligned.
    std::string out;
    std::size_t i = 0;
    while (i < n) {
        // Find the longest strictly-decodable prefix starting at i by trying
        // per-character steps (cheap: cp932 chars are 1 or 2 bytes).
        std::uint8_t b = p[i];
        std::size_t step = (b <= 0x7F || (b >= 0xA1 && b <= 0xDF)) ? 1 : 2;
        if (i + step <= n) {
            auto piece = cp932_to_utf8_strict(p + i, step);
            if (piece) {
                out += *piece;
                i += step;
                continue;
            }
        }
        out += "\xEF\xBF\xBD";  // U+FFFD
        ++i;
    }
    return out;
}

Bytes utf8_to_cp932_replace(const std::string& s) {
    std::wstring w = utf8_to_wide(s);
    if (w.empty()) return {};
    BOOL used_default = FALSE;
    int len = WideCharToMultiByte(932, 0, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, &used_default);
    Bytes out(static_cast<std::size_t>(len));
    WideCharToMultiByte(932, 0, w.data(), static_cast<int>(w.size()),
                        reinterpret_cast<char*>(out.data()), len, nullptr, nullptr);
    return out;
}

// ---- Unicode helpers -------------------------------------------------------

char32_t utf8_next(const std::string& s, std::size_t& i) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
    std::uint8_t b = p[i];
    if (b < 0x80) { ++i; return b; }
    int extra = (b >= 0xF0) ? 3 : (b >= 0xE0) ? 2 : (b >= 0xC0) ? 1 : 0;
    if (extra == 0 || i + extra >= s.size()) { ++i; return b; }
    char32_t cp = b & (0x3F >> extra);
    for (int k = 1; k <= extra; ++k)
        cp = (cp << 6) | (p[i + k] & 0x3F);
    i += static_cast<std::size_t>(extra) + 1;
    return cp;
}

int jp_char_count(const std::string& utf8) {
    int n = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        char32_t cp = utf8_next(utf8, i);
        if ((cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
            (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF))
            ++n;
    }
    return n;
}

bool is_clean_text(const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        char32_t cp = utf8_next(utf8, i);
        if (cp < 0x20 && cp != U'\t' && cp != U'\r' && cp != U'\n')
            return false;
    }
    return true;
}

// ---- logging ---------------------------------------------------------------

namespace {
std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
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

void print_inline(const std::string& msg) {
    std::fputs(msg.c_str(), stdout);
    std::fflush(stdout);
}

void setup_console_utf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

// ---- JSON ------------------------------------------------------------------

namespace {
void pretty_impl(const boost::json::value& v, std::string& out, int indent, int depth) {
    namespace bj = boost::json;
    const std::string pad(static_cast<std::size_t>(indent) * depth, ' ');
    const std::string pad1(static_cast<std::size_t>(indent) * (depth + 1), ' ');
    switch (v.kind()) {
    case bj::kind::object: {
        const auto& o = v.get_object();
        if (o.empty()) { out += "{}"; return; }
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
        if (a.empty()) { out += "[]"; return; }
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

std::string json_pretty(const boost::json::value& v, int indent) {
    std::string out;
    pretty_impl(v, out, indent, 0);
    return out;
}

boost::json::value json_parse_file(const std::string& path) {
    Bytes raw = read_file(path);
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
