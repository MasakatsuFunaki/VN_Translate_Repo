// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

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

namespace exm {

namespace {

constexpr UINT kCp932CodePage = 932;
constexpr std::size_t kCrlfReserveDivisor = 16;

constexpr std::uint8_t kAsciiMax = 0x7F;
constexpr std::uint8_t kHalfwidthKatakanaMin = 0xA1;
constexpr std::uint8_t kHalfwidthKatakanaMax = 0xDF;
constexpr std::uint8_t kLeadRange1Min = 0x81;
constexpr std::uint8_t kLeadRange1Max = 0x9F;
constexpr std::uint8_t kLeadRange2Min = 0xE0;
constexpr std::uint8_t kLeadRange2Max = 0xFC;
constexpr std::uint8_t kTrailRange1Min = 0x40;
constexpr std::uint8_t kTrailRange1Max = 0x7E;
constexpr std::uint8_t kTrailRange2Min = 0x80;
constexpr std::uint8_t kTrailRange2Max = 0xFC;

constexpr std::uint8_t kSpecialByte80 = 0x80;
constexpr std::uint8_t kSpecialByteA0 = 0xA0;
constexpr std::uint8_t kSpecialByteFD = 0xFD;
constexpr std::uint8_t kSpecialByteFE = 0xFE;
constexpr std::uint8_t kSpecialByteFF = 0xFF;

constexpr char32_t kSpecialCp80 = 0x0080;
constexpr char32_t kSpecialCpA0 = 0xF8F0;
constexpr char32_t kSpecialCpFD = 0xF8F1;
constexpr char32_t kSpecialCpFE = 0xF8F2;
constexpr char32_t kSpecialCpFF = 0xF8F3;

constexpr char32_t kUtf8OneByteLimit = 0x80;
constexpr char32_t kUtf8TwoByteLimit = 0x800;
constexpr std::uint8_t kUtf8TwoBytePrefix = 0xC0;
constexpr std::uint8_t kUtf8ThreeBytePrefix = 0xE0;
constexpr std::uint8_t kUtf8FourBytePrefix = 0xF0;
constexpr std::uint8_t kUtf8ContPrefix = 0x80;
constexpr std::uint8_t kUtf8ContMask = 0x3F;
constexpr unsigned int kUtf8Shift6 = 6;
constexpr unsigned int kUtf8Shift12 = 12;

constexpr char32_t kHiraganaMin = 0x3040;
constexpr char32_t kHiraganaMax = 0x309F;
constexpr char32_t kKatakanaMin = 0x30A0;
constexpr char32_t kKatakanaMax = 0x30FF;
constexpr char32_t kCjkUnifiedMin = 0x4E00;
constexpr char32_t kCjkUnifiedMax = 0x9FFF;
constexpr char32_t kCjkExtAMin = 0x3400;
constexpr char32_t kCjkExtAMax = 0x4DBF;

constexpr char32_t kUnicodeEnQuad = 0x2000;
constexpr char32_t kUnicodeHairSpace = 0x200A;
constexpr char32_t kUnicodeOghamSpace = 0x1680;
constexpr char32_t kUnicodeLineSep = 0x2028;
constexpr char32_t kUnicodeParaSep = 0x2029;
constexpr char32_t kUnicodeNarrowNbsp = 0x202F;
constexpr char32_t kUnicodeMathSpace = 0x205F;
constexpr char32_t kUnicodeIdeographicSpace = 0x3000;
constexpr char32_t kUnicodeNextLine = 0x0085;
constexpr char32_t kUnicodeNoBreakSpace = 0x00A0;
constexpr char32_t kAsciiFileSeparator = 0x1C;
constexpr char32_t kAsciiGroupSeparator = 0x1D;
constexpr char32_t kAsciiRecordSeparator = 0x1E;
constexpr char32_t kAsciiUnitSeparator = 0x1F;

constexpr std::size_t kTimestampBufSize = 16;
constexpr int kThousandsGroupSize = 3;

std::string to_crlf(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / kCrlfReserveDivisor);
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) out += '\r';
        out += s[i];
    }
    return out;
}

bool cp932_special_single(std::uint8_t b, char32_t* cp) {
    switch (b) {
    case kSpecialByte80: *cp = kSpecialCp80; return true;
    case kSpecialByteA0: *cp = kSpecialCpA0; return true;
    case kSpecialByteFD: *cp = kSpecialCpFD; return true;
    case kSpecialByteFE: *cp = kSpecialCpFE; return true;
    case kSpecialByteFF: *cp = kSpecialCpFF; return true;
    default: return false;
    }
}

bool cp932_is_single(std::uint8_t b) {
    char32_t ignored = 0;
    return b <= kAsciiMax ||
           (b >= kHalfwidthKatakanaMin && b <= kHalfwidthKatakanaMax) ||
           cp932_special_single(b, &ignored);
}

std::size_t cp932_char_len(const std::uint8_t* p, std::size_t n, std::size_t i) {
    const std::uint8_t b = p[i];
    if (cp932_is_single(b)) return 1;
    const bool is_lead = (b >= kLeadRange1Min && b <= kLeadRange1Max) ||
                         (b >= kLeadRange2Min && b <= kLeadRange2Max);
    if (!is_lead || i + 1 >= n) return 0;
    const std::uint8_t t = p[i + 1];
    const bool is_trail = (t >= kTrailRange1Min && t <= kTrailRange1Max) ||
                          (t >= kTrailRange2Min && t <= kTrailRange2Max);
    return is_trail ? 2 : 0;
}

void append_utf8_cp(std::string& out, char32_t cp) {
    if (cp < kUtf8OneByteLimit) {
        out += static_cast<char>(cp);
        return;
    }
    if (cp < kUtf8TwoByteLimit) {
        out += static_cast<char>(kUtf8TwoBytePrefix | (cp >> kUtf8Shift6));
        out += static_cast<char>(kUtf8ContPrefix | (cp & kUtf8ContMask));
        return;
    }
    out += static_cast<char>(kUtf8ThreeBytePrefix | (cp >> kUtf8Shift12));
    out += static_cast<char>(kUtf8ContPrefix | ((cp >> kUtf8Shift6) & kUtf8ContMask));
    out += static_cast<char>(kUtf8ContPrefix | (cp & kUtf8ContMask));
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

void trim_range(const std::string& utf8, bool left, bool right,
                std::size_t& begin, std::size_t& end) {
    std::vector<std::size_t> starts;
    std::vector<char32_t> cps;
    std::size_t i = 0;
    while (i < utf8.size()) {
        starts.push_back(i);
        cps.push_back(utf8_next(utf8, i));
    }
    std::size_t b = 0;
    std::size_t e = cps.size();
    if (left) {
        while (b < e && is_unicode_space(cps[b])) ++b;
    }
    if (right) {
        while (e > b && is_unicode_space(cps[e - 1])) --e;
    }
    begin = (b == cps.size()) ? utf8.size() : starts[b];
    end = (e == cps.size()) ? utf8.size() : starts[e];
    if (b == e) end = begin;
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[kTimestampBufSize];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void pretty_impl(const boost::json::value& v, std::string& out, int indent, int depth) {
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

void dump_impl(const boost::json::value& v, std::string& out) {
    namespace bj = boost::json;
    switch (v.kind()) {
    case bj::kind::object: {
        out += '{';
        bool first = true;
        for (const auto& kv : v.get_object()) {
            if (!first) out += ", ";
            first = false;
            out += bj::serialize(bj::value(kv.key()));
            out += ": ";
            dump_impl(kv.value(), out);
        }
        out += '}';
        return;
    }
    case bj::kind::array: {
        out += '[';
        bool first = true;
        for (const auto& e : v.get_array()) {
            if (!first) out += ", ";
            first = false;
            dump_impl(e, out);
        }
        out += ']';
        return;
    }
    default:
        out += bj::serialize(v);
    }
}

}  // namespace

ScopedFile::ScopedFile(std::FILE* fp) noexcept : fp_(fp) {}

ScopedFile::ScopedFile(const std::string& path, const char* mode)
    : fp_(_wfopen(utf8_to_wide(path).c_str(),
                  utf8_to_wide(mode ? mode : "").c_str())) {}

ScopedFile::~ScopedFile() {
    reset();
}

ScopedFile::ScopedFile(ScopedFile&& other) noexcept : fp_(other.release()) {}

ScopedFile& ScopedFile::operator=(ScopedFile&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

std::FILE* ScopedFile::release() noexcept {
    std::FILE* old = fp_;
    fp_ = nullptr;
    return old;
}

void ScopedFile::reset(std::FILE* fp) noexcept {
    if (fp_ != nullptr) {
        std::fclose(fp_);
    }
    fp_ = fp;
}

Bytes read_file(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    f.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    Bytes out(size);
    if (size) f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (!f) throw std::runtime_error("read failed: " + path);
    return out;
}

void write_file(const std::string& path, const void* data, std::size_t size) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot create " + path);
    if (size) f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!f) throw std::runtime_error("write failed: " + path);
}

void write_file_atomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    write_file(tmp, content);
    std::filesystem::rename(std::filesystem::u8path(tmp),
                            std::filesystem::u8path(path));
}

void write_file_text(const std::string& path, const std::string& content) {
    write_file(path, to_crlf(content));
}

void write_file_atomic_text(const std::string& path, const std::string& content) {
    write_file_atomic(path, to_crlf(content));
}

std::optional<std::string> cp932_to_utf8_strict(const std::uint8_t* p, std::size_t n) {
    if (n == 0) return std::string();
    std::string out;
    std::size_t i = 0;
    while (i < n) {
        char32_t special = 0;
        if (cp932_special_single(p[i], &special)) {
            append_utf8_cp(out, special);
            ++i;
            continue;
        }
        const std::size_t run_start = i;
        while (i < n) {
            char32_t ignored = 0;
            if (cp932_special_single(p[i], &ignored)) break;
            const std::size_t clen = cp932_char_len(p, n, i);
            if (clen == 0) return std::nullopt;
            i += clen;
        }
        const int bytes = static_cast<int>(i - run_start);
        if (bytes <= 0) return std::nullopt;
        const int len = MultiByteToWideChar(kCp932CodePage, MB_ERR_INVALID_CHARS,
                                            reinterpret_cast<const char*>(p + run_start),
                                            bytes, nullptr, 0);
        if (len <= 0) return std::nullopt;
        std::wstring w(static_cast<std::size_t>(len), L'\0');
        MultiByteToWideChar(kCp932CodePage, MB_ERR_INVALID_CHARS,
                            reinterpret_cast<const char*>(p + run_start), bytes,
                            w.data(), len);
        out += wide_to_utf8(w);
    }
    return out;
}

std::string cp932_to_utf8_replace(const std::uint8_t* p, std::size_t n) {
    std::string out;
    std::size_t i = 0;
    while (i < n) {
        const std::uint8_t b = p[i];
        const std::size_t step = cp932_is_single(b) ? 1 : 2;
        if (i + step <= n) {
            auto piece = cp932_to_utf8_strict(p + i, step);
            if (piece) {
                out += *piece;
                i += step;
                continue;
            }
        }
        out += "";
        ++i;
    }
    return out;
}

Bytes utf8_to_cp932_replace(const std::string& s) {
    const std::wstring w = utf8_to_wide(s);
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(kCp932CodePage, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    Bytes out(static_cast<std::size_t>(len));
    WideCharToMultiByte(kCp932CodePage, 0, w.data(), static_cast<int>(w.size()),
                        reinterpret_cast<char*>(out.data()), len, nullptr, nullptr);
    return out;
}

std::optional<Bytes> utf8_to_cp932_strict(const std::string& s) {
    const std::wstring w = utf8_to_wide(s);
    if (w.empty()) return Bytes{};
    BOOL used_default = FALSE;
    const int len = WideCharToMultiByte(kCp932CodePage, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, &used_default);
    if (len <= 0) return std::nullopt;
    Bytes out(static_cast<std::size_t>(len));
    WideCharToMultiByte(kCp932CodePage, 0, w.data(), static_cast<int>(w.size()),
                        reinterpret_cast<char*>(out.data()), len, nullptr, &used_default);
    if (used_default) return std::nullopt;
    return out;
}

char32_t utf8_next(const std::string& s, std::size_t& i) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
    const std::uint8_t b = p[i];
    if (b < kUtf8OneByteLimit) {
        ++i;
        return b;
    }
    const int extra = (b >= kUtf8FourBytePrefix) ? 3
                    : (b >= kUtf8ThreeBytePrefix) ? 2
                    : (b >= kUtf8TwoBytePrefix) ? 1
                    : 0;
    if (extra == 0 || i + static_cast<std::size_t>(extra) >= s.size()) {
        ++i;
        return b;
    }
    char32_t cp = b & (kUtf8ContMask >> extra);
    for (int k = 1; k <= extra; ++k) {
        cp = (cp << kUtf8Shift6) | (p[i + static_cast<std::size_t>(k)] & kUtf8ContMask);
    }
    i += static_cast<std::size_t>(extra) + 1;
    return cp;
}

int jp_char_count(const std::string& utf8) {
    int n = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        if ((cp >= kHiraganaMin && cp <= kHiraganaMax) ||
            (cp >= kKatakanaMin && cp <= kKatakanaMax) ||
            (cp >= kCjkUnifiedMin && cp <= kCjkUnifiedMax) ||
            (cp >= kCjkExtAMin && cp <= kCjkExtAMax)) {
            ++n;
        }
    }
    return n;
}

bool is_unicode_space(char32_t cp) {
    switch (cp) {
    case '\t':
    case '\n':
    case '\v':
    case '\f':
    case '\r':
    case kAsciiFileSeparator:
    case kAsciiGroupSeparator:
    case kAsciiRecordSeparator:
    case kAsciiUnitSeparator:
    case ' ':
    case kUnicodeNextLine:
    case kUnicodeNoBreakSpace:
    case kUnicodeOghamSpace:
    case kUnicodeLineSep:
    case kUnicodeParaSep:
    case kUnicodeNarrowNbsp:
    case kUnicodeMathSpace:
    case kUnicodeIdeographicSpace:
        return true;
    default:
        return cp >= kUnicodeEnQuad && cp <= kUnicodeHairSpace;
    }
}

std::string trim(const std::string& utf8) {
    std::size_t b = 0;
    std::size_t e = 0;
    trim_range(utf8, true, true, b, e);
    return utf8.substr(b, e - b);
}

std::string trim_left(const std::string& utf8) {
    std::size_t b = 0;
    std::size_t e = 0;
    trim_range(utf8, true, false, b, e);
    return utf8.substr(b, e - b);
}

std::string trim_right(const std::string& utf8) {
    std::size_t b = 0;
    std::size_t e = 0;
    trim_range(utf8, false, true, b, e);
    return utf8.substr(b, e - b);
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

std::string comma(long long n) {
    const std::string digits = std::to_string(n < 0 ? -n : n);
    std::string out;
    int count = 0;
    for (std::size_t i = digits.size(); i-- > 0;) {
        out += digits[i];
        if (++count % kThousandsGroupSize == 0 && i != 0) out += ',';
    }
    if (n < 0) out += '-';
    return std::string(out.rbegin(), out.rend());
}

std::string json_pretty(const boost::json::value& v, int indent) {
    std::string out;
    pretty_impl(v, out, indent, 0);
    return out;
}

std::string json_dump(const boost::json::value& v) {
    std::string out;
    dump_impl(v, out);
    return out;
}

boost::json::value json_parse_file(const std::string& path) {
    const Bytes raw = read_file(path);
    try {
        return boost::json::parse(boost::json::string_view(
            reinterpret_cast<const char*>(raw.data()), raw.size()));
    } catch (const std::exception& e) {
        throw std::runtime_error("cannot parse " + path + ": " + e.what());
    }
}

}  // namespace exm
