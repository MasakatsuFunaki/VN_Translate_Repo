// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "common/util.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace spot_checker {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kTimestampBufferSize = 16;
constexpr std::size_t kFormatSampleBytes = 4096;
constexpr int kMinDetectedIndent = 1;
constexpr int kMaxDetectedIndent = 8;
constexpr std::size_t kCrlfReserveDivisor = 16;

constexpr std::uint8_t kUtf8AsciiLimit = 0x80U;
constexpr std::uint8_t kUtf8TwoByteMin = 0xC0U;
constexpr std::uint8_t kUtf8ThreeByteMin = 0xE0U;
constexpr std::uint8_t kUtf8FourByteMin = 0xF0U;
constexpr std::uint8_t kUtf8PayloadBaseMask = 0x3FU;
constexpr int kUtf8ContinuationShiftBits = 6;
constexpr int kOneExtraByte = 1;
constexpr int kTwoExtraBytes = 2;
constexpr int kThreeExtraBytes = 3;

constexpr char32_t kCornerBracketFirst = 0x300CU;
constexpr char32_t kCornerBracketLast = 0x300FU;
constexpr char32_t kHiraganaFirst = 0x3040U;
constexpr char32_t kHiraganaLast = 0x309FU;
constexpr char32_t kKatakanaFirst = 0x30A0U;
constexpr char32_t kKatakanaLast = 0x30FFU;
constexpr char32_t kCjkExtAFirst = 0x3400U;
constexpr char32_t kCjkExtALast = 0x4DBFU;
constexpr char32_t kCjkUnifiedFirst = 0x4E00U;
constexpr char32_t kCjkUnifiedLast = 0x9FFFU;

struct LocalFreeGuard {
    explicit LocalFreeGuard(wchar_t** p) noexcept : ptr(p) {}
    ~LocalFreeGuard() {
        if (ptr != nullptr) {
            ::LocalFree(ptr);
        }
    }
    LocalFreeGuard(const LocalFreeGuard&) = delete;
    LocalFreeGuard& operator=(const LocalFreeGuard&) = delete;

    wchar_t** ptr = nullptr;
};

bool is_ascii_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return s;
    }
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(from, pos);
        if (hit == std::string::npos) {
            out.append(s, pos, std::string::npos);
            return out;
        }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[kTimestampBufferSize];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void pretty_impl(const bj::value& v, std::string& out, int indent, int depth) {
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
            if (!first) {
                out += ",\n";
            }
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
            if (!first) {
                out += ",\n";
            }
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
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot open " + path);
    }
    f.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    Bytes out(size);
    if (size > 0) {
        f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    }
    if (!f) {
        throw std::runtime_error("read failed: " + path);
    }
    return out;
}

std::string read_file_text(const std::string& path) {
    const Bytes b = read_file(path);
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

void write_file(const std::string& path, const void* data, std::size_t size) {
    const fs::path p = fs::u8path(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
    }
    std::ofstream f(p, std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot create " + path);
    }
    if (size > 0) {
        f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    if (!f) {
        throw std::runtime_error("write failed: " + path);
    }
}

void write_file_atomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    write_file(tmp, content);
    std::error_code ec;
    fs::rename(fs::u8path(tmp), fs::u8path(path), ec);
    if (ec) {
        fs::remove(fs::u8path(path), ec);
        fs::rename(fs::u8path(tmp), fs::u8path(path));
    }
}

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    if (wide.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("UTF-16 string exceeds maximum supported length");
    }
    const int src_len = static_cast<int>(wide.size());
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), src_len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("Invalid UTF-16 sequence in wide_to_utf8");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                              src_len, out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    }
    return out;
}

std::vector<std::string> process_utf8_args() {
    const wchar_t* cmd = ::GetCommandLineW();
    if (cmd == nullptr || *cmd == L'\0') {
        return {};
    }
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(cmd, &argc);
    const LocalFreeGuard guard(argv);
    if (argv == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed");
    }
    std::vector<std::string> args;
    if (argc > 1) {
        args.reserve(static_cast<std::size_t>(argc - 1));
        for (int i = 1; i < argc; ++i) {
            args.push_back(wide_to_utf8(argv[i]));
        }
    }
    return args;
}

char32_t utf8_next(const std::string& s, std::size_t& i) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
    const std::uint8_t b = p[i];
    if (b < kUtf8AsciiLimit) {
        ++i;
        return b;
    }
    const int extra = (b >= kUtf8FourByteMin)    ? kThreeExtraBytes
                      : (b >= kUtf8ThreeByteMin) ? kTwoExtraBytes
                      : (b >= kUtf8TwoByteMin)   ? kOneExtraByte
                                                 : 0;
    if (extra == 0 || i + static_cast<std::size_t>(extra) >= s.size()) {
        ++i;
        return b;
    }
    char32_t cp = b & (kUtf8PayloadBaseMask >> extra);
    for (int k = 1; k <= extra; ++k) {
        cp = (cp << kUtf8ContinuationShiftBits) |
             (p[i + static_cast<std::size_t>(k)] & kUtf8PayloadBaseMask);
    }
    i += static_cast<std::size_t>(extra) + 1;
    return cp;
}

std::size_t utf8_char_len(const std::string& s) {
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        utf8_next(s, i);
        ++n;
    }
    return n;
}

bool has_real_japanese(const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        if ((cp >= kHiraganaFirst && cp <= kHiraganaLast) ||
            (cp >= kKatakanaFirst && cp <= kKatakanaLast) ||
            (cp >= kCjkUnifiedFirst && cp <= kCjkUnifiedLast) ||
            (cp >= kCjkExtAFirst && cp <= kCjkExtALast)) {
            return true;
        }
    }
    return false;
}

bool has_japanese(const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        if ((cp >= kHiraganaFirst && cp <= kHiraganaLast) ||
            (cp >= kKatakanaFirst && cp <= kKatakanaLast) ||
            (cp >= kCjkUnifiedFirst && cp <= kCjkUnifiedLast) ||
            (cp >= kCjkExtAFirst && cp <= kCjkExtALast) ||
            (cp >= kCornerBracketFirst && cp <= kCornerBracketLast)) {
            return true;
        }
    }
    return false;
}

std::string trim_left(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size() && is_ascii_ws(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    return s.substr(b);
}

std::string trim_right(const std::string& s) {
    std::size_t e = s.size();
    while (e > 0 && is_ascii_ws(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(0, e);
}

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && is_ascii_ws(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && is_ascii_ws(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

std::string sanitize_ascii(const std::string& text) {
    std::string s = text;
    s = replace_all(s, "—", "--");
    s = replace_all(s, "–", "--");
    s = replace_all(s, "…", "...");
    s = replace_all(s, "‘", "'");
    s = replace_all(s, "’", "'");
    s = replace_all(s, "“", "\"");
    s = replace_all(s, "”", "\"");
    s = replace_all(s, "　", " ");
    return s;
}

void setup_console_utf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

void log_info(const std::string& msg) {
    std::fputs((timestamp() + " INFO " + msg + "\n").c_str(), stdout);
    std::fflush(stdout);
}

void print_line(const std::string& msg) {
    std::fputs((msg + "\n").c_str(), stdout);
    std::fflush(stdout);
}

std::string json_pretty(const bj::value& v, int indent) {
    std::string out;
    pretty_impl(v, out, indent, 0);
    return out;
}

JsonFormatStyle detect_json_format_style(const std::string& path,
                                         int default_indent,
                                         bool default_crlf) {
    JsonFormatStyle style;
    style.indent = default_indent;
    style.use_crlf = default_crlf;
    style.trailing_newline = false;

    std::error_code ec;
    const fs::path p = fs::u8path(path);
    if (!fs::exists(p, ec)) {
        return style;
    }

    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return style;
    }

    char head[kFormatSampleBytes];
    f.read(head, sizeof(head));
    const std::size_t n = static_cast<std::size_t>(f.gcount());
    if (n == 0) {
        return style;
    }

    const std::string sample(head, n);
    style.use_crlf = (sample.find("\r\n") != std::string::npos);

    const std::size_t nl = sample.find('\n');
    if (nl != std::string::npos) {
        int spaces = 0;
        std::size_t idx = nl + 1;
        while (idx < sample.size() && sample[idx] == ' ') {
            ++spaces;
            ++idx;
        }
        if (idx < sample.size() && sample[idx] == '"' &&
            spaces >= kMinDetectedIndent && spaces <= kMaxDetectedIndent) {
            style.indent = spaces;
        }
    }

    f.clear();
    f.seekg(0, std::ios::end);
    const auto total_sz = static_cast<std::size_t>(f.tellg());
    if (total_sz > 0) {
        f.seekg(static_cast<std::streamoff>(total_sz - 1));
        char last_ch = '\0';
        if (f.read(&last_ch, 1)) {
            style.trailing_newline = (last_ch == '\n');
        }
    }
    return style;
}

void write_json_preserving_style(const std::string& path,
                                 const bj::value& v,
                                 int default_indent,
                                 bool default_crlf) {
    const JsonFormatStyle style =
        detect_json_format_style(path, default_indent, default_crlf);
    std::string text = json_pretty(v, style.indent);
    if (style.trailing_newline) {
        text += '\n';
    }
    if (style.use_crlf) {
        std::string crlf_text;
        crlf_text.reserve(text.size() + text.size() / kCrlfReserveDivisor);
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
                crlf_text += '\r';
            }
            crlf_text += text[i];
        }
        text = std::move(crlf_text);
    }
    write_file_atomic(path, text);
}

bj::value json_parse_file(const std::string& path) {
    const Bytes raw = read_file(path);
    try {
        return bj::parse(
            bj::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()));
    } catch (const std::exception& e) {
        throw std::runtime_error("cannot parse " + path + ": " + e.what());
    }
}

}  // namespace spot_checker
