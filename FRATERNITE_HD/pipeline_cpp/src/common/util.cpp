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

namespace frat {

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

namespace {
std::string to_crlf(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 16);
    for (std::size_t i = 0; i < s.size(); ++i) {
        // Never double up an existing CRLF.
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) out += '\r';
        out += s[i];
    }
    return out;
}
}  // namespace

void write_file_text(const std::string& path, const std::string& content) {
    write_file(path, to_crlf(content));
}

void write_file_atomic_text(const std::string& path, const std::string& content) {
    write_file_atomic(path, to_crlf(content));
}

// ---- CP932 <-> UTF-8 -------------------------------------------------------

namespace {

// CP932 byte structure: single bytes 0x00-0x7F and 0xA1-0xDF (halfwidth
// katakana); lead bytes 0x81-0x9F and 0xE0-0xFC with trail 0x40-0x7E /
// 0x80-0xFC.
// CP932 ALSO decodes these five single bytes, which a naive "structurally
// valid Shift-JIS" test rejects:
//   0x80 -> U+0080, 0xA0 -> U+F8F0, 0xFD -> U+F8F1, 0xFE -> U+F8F2, 0xFF -> U+F8F3
// MultiByteToWideChar(932) does NOT agree on all of them, so they are mapped
// here rather than handed to the OS codec.
bool cp932_special_single(std::uint8_t b, char32_t* cp) {
    switch (b) {
    case 0x80: *cp = 0x0080; return true;
    case 0xA0: *cp = 0xF8F0; return true;
    case 0xFD: *cp = 0xF8F1; return true;
    case 0xFE: *cp = 0xF8F2; return true;
    case 0xFF: *cp = 0xF8F3; return true;
    default: return false;
    }
}

// True when `b` starts a single-byte cp932 character.
bool cp932_is_single(std::uint8_t b) {
    char32_t ignored;
    return b <= 0x7F || (b >= 0xA1 && b <= 0xDF) || cp932_special_single(b, &ignored);
}

// Byte length of the character at p[i], or 0 when the sequence is invalid.
// Lead bytes 0x81-0x9F / 0xE0-0xFC take a trail of 0x40-0x7E or 0x80-0xFC.
std::size_t cp932_char_len(const std::uint8_t* p, std::size_t n, std::size_t i) {
    const std::uint8_t b = p[i];
    if (cp932_is_single(b)) return 1;
    if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) {
        if (i + 1 >= n) return 0;
        const std::uint8_t t = p[i + 1];
        if ((t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFC)) return 2;
    }
    return 0;
}

void append_utf8_cp(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
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
    // The buffer is split into maximal runs the OS codec gets right, separated
    // by the five special singles above, which are emitted directly.
    // Converting a whole run per call keeps this cheap on multi-MB scripts.
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
        if (bytes <= 0) return std::nullopt;  // no progress: refuse to spin
        int len = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
                                      reinterpret_cast<const char*>(p + run_start),
                                      bytes, nullptr, 0);
        if (len <= 0) return std::nullopt;
        std::wstring w(static_cast<std::size_t>(len), L'\0');
        MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
                            reinterpret_cast<const char*>(p + run_start), bytes,
                            w.data(), len);
        out += wide_to_utf8(w);
    }
    return out;
}

std::string cp932_to_utf8_replace(const std::uint8_t* p, std::size_t n) {
    // Walk the byte stream emitting one U+FFFD per undecodable byte: decode
    // valid runs strictly, slide a single byte on failure.
    std::string out;
    std::size_t i = 0;
    while (i < n) {
        std::uint8_t b = p[i];
        std::size_t step = cp932_is_single(b) ? 1 : 2;
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

namespace {

// UTF-16 code unit -> CP932 sequence, built by INVERTING the strict decoder in
// ascending byte order (first byte sequence to claim a codepoint wins), which
// is how the canonical CP932 encoding table resolves its duplicate glyphs.
// 0xFFFF marks an unmapped codepoint; values below 0x100 are one-byte
// encodings.
const std::vector<std::uint16_t>& cp932_encode_table() {
    static const std::vector<std::uint16_t> table = [] {
        std::vector<std::uint16_t> t(0x10000, 0xFFFF);
        auto claim = [&t](const std::optional<std::string>& utf8, std::uint16_t v) {
            if (!utf8 || utf8->empty()) return;
            std::size_t i = 0;
            const char32_t cp = utf8_next(*utf8, i);
            if (i != utf8->size() || cp > 0xFFFF) return;  // must be one BMP char
            if (t[cp] == 0xFFFF) t[cp] = v;
        };
        for (int b = 0x00; b <= 0xFF; ++b) {
            const auto by = static_cast<std::uint8_t>(b);
            claim(cp932_to_utf8_strict(&by, 1), static_cast<std::uint16_t>(b));
        }
        auto lead_range = [](int k) { return k < 0x1F ? 0x81 + k : 0xE0 + (k - 0x1F); };
        auto trail_range = [](int k) { return k < 0x3F ? 0x40 + k : 0x80 + (k - 0x3F); };
        for (int li = 0; li < 0x1F + 0x1D; ++li) {
            for (int ti = 0; ti < 0x3F + 0x7D; ++ti) {
                const std::uint8_t pair[2] = {static_cast<std::uint8_t>(lead_range(li)),
                                              static_cast<std::uint8_t>(trail_range(ti))};
                claim(cp932_to_utf8_strict(pair, 2),
                      static_cast<std::uint16_t>((pair[0] << 8) | pair[1]));
            }
        }
        // Codepoints the inversion cannot reach because a lower byte sequence
        // already claimed the same glyph; CP932 encodes them as the higher
        // sequence listed here.
        // The four PUA entries are already produced by the single-byte pass;
        // they are repeated so the whole fixup set reads as one list.
        const std::pair<char32_t, std::uint16_t> fixups[] = {
            {0x00A2, 0x8191}, {0x00A3, 0x8192}, {0x00AC, 0x81CA}, {0x2016, 0x8161},
            {0x2212, 0x817C}, {0x301C, 0x8160}, {0xF8F0, 0x00A0}, {0xF8F1, 0x00FD},
            {0xF8F2, 0x00FE}, {0xF8F3, 0x00FF},
        };
        for (const auto& [cp, v] : fixups) t[cp] = v;
        return t;
    }();
    return table;
}

}  // namespace

bool cp932_encodable(char32_t cp) {
    return cp <= 0xFFFF && cp932_encode_table()[cp] != 0xFFFF;
}

Bytes utf8_to_cp932_replace(const std::string& s) {
    const auto& table = cp932_encode_table();
    Bytes out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const char32_t cp = utf8_next(s, i);
        const std::uint16_t v = cp <= 0xFFFF ? table[cp] : 0xFFFF;
        if (v == 0xFFFF) {
            out.push_back('?');
        } else if (v < 0x100) {
            out.push_back(static_cast<std::uint8_t>(v));
        } else {
            out.push_back(static_cast<std::uint8_t>(v >> 8));
            out.push_back(static_cast<std::uint8_t>(v & 0xFF));
        }
    }
    return out;
}

std::optional<Bytes> utf8_to_cp932_strict(const std::string& s) {
    const auto& table = cp932_encode_table();
    Bytes out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const char32_t cp = utf8_next(s, i);
        const std::uint16_t v = cp <= 0xFFFF ? table[cp] : 0xFFFF;
        if (v == 0xFFFF) return std::nullopt;
        if (v < 0x100) {
            out.push_back(static_cast<std::uint8_t>(v));
        } else {
            out.push_back(static_cast<std::uint8_t>(v >> 8));
            out.push_back(static_cast<std::uint8_t>(v & 0xFF));
        }
    }
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

std::vector<char32_t> utf8_decode(const std::string& s) {
    std::vector<char32_t> out;
    std::size_t i = 0;
    while (i < s.size()) out.push_back(utf8_next(s, i));
    return out;
}

std::string utf8_encode_cp(char32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

std::string utf8_encode(const std::vector<char32_t>& cps) {
    std::string out;
    out.reserve(cps.size());
    for (char32_t cp : cps) out += utf8_encode_cp(cp);
    return out;
}

std::size_t char_len(const std::string& utf8) {
    std::size_t n = 0, i = 0;
    while (i < utf8.size()) {
        utf8_next(utf8, i);
        ++n;
    }
    return n;
}

std::string cp_substr(const std::string& utf8, std::size_t cp_begin, std::size_t cp_end) {
    std::string out;
    std::size_t i = 0, n = 0;
    while (i < utf8.size() && n < cp_end) {
        const std::size_t start = i;
        utf8_next(utf8, i);
        if (n >= cp_begin) out.append(utf8, start, i - start);
        ++n;
    }
    return out;
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

bool is_unicode_space(char32_t cp) {
    // Unicode category Zs plus the bidi WS/B/S controls.  U+200B ZERO WIDTH
    // SPACE is deliberately absent: it has no width to trim and stripping it
    // would change a TSV key the engine really does emit.
    switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:
    case 0x20: case 0x85: case 0xA0:
    case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
        return true;
    default:
        return cp >= 0x2000 && cp <= 0x200A;
    }
}

namespace {

// Byte range [begin, end) of `utf8` with whitespace trimmed off the requested
// sides.  Codepoint start offsets are collected up front so the tail scan can
// walk backwards without re-synchronising on UTF-8 continuation bytes.
void trim_range(const std::string& utf8, bool left, bool right,
                   std::size_t& begin, std::size_t& end) {
    std::vector<std::size_t> starts;
    std::vector<char32_t> cps;
    std::size_t i = 0;
    while (i < utf8.size()) {
        starts.push_back(i);
        cps.push_back(utf8_next(utf8, i));
    }
    std::size_t b = 0, e = cps.size();
    if (left)
        while (b < e && is_unicode_space(cps[b])) ++b;
    if (right)
        while (e > b && is_unicode_space(cps[e - 1])) --e;
    begin = (b == cps.size()) ? utf8.size() : starts[b];
    end = (e == cps.size()) ? utf8.size() : starts[e];
    if (b == e) end = begin;
}

}  // namespace

std::string trim(const std::string& utf8) {
    std::size_t b, e;
    trim_range(utf8, true, true, b, e);
    return utf8.substr(b, e - b);
}

std::string trim_left(const std::string& utf8) {
    std::size_t b, e;
    trim_range(utf8, true, false, b, e);
    return utf8.substr(b, e - b);
}

std::string trim_right(const std::string& utf8) {
    std::size_t b, e;
    trim_range(utf8, false, true, b, e);
    return utf8.substr(b, e - b);
}

std::string quote_repr(const std::string& utf8) {
    const std::vector<char32_t> cps = utf8_decode(utf8);
    // Single quotes by default; switch to double quotes only when the string
    // contains an apostrophe and no double quote, so "Yuka's Mother" reads
    // without an escape.
    bool has_sq = false, has_dq = false;
    for (char32_t cp : cps) {
        if (cp == '\'') has_sq = true;
        if (cp == '"') has_dq = true;
    }
    const char quote = (has_sq && !has_dq) ? '"' : '\'';
    std::string out(1, quote);
    for (char32_t cp : cps) {
        if (cp == static_cast<char32_t>(quote) || cp == '\\') {
            out += '\\';
            out += static_cast<char>(cp);
        } else if (cp == '\n') {
            out += "\\n";
        } else if (cp == '\r') {
            out += "\\r";
        } else if (cp == '\t') {
            out += "\\t";
        } else if (cp < 0x20 || cp == 0x7F || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
                   (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
                   cp == 0x202F || cp == 0x205F || cp == 0x3000) {
            // Non-printable: the ASCII controls and every separator other than
            // plain ' ' -- escaped so they are visible in the gate's report.
            char buf[16];
            if (cp < 0x100) std::snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned>(cp));
            else std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(cp));
            out += buf;
        } else {
            out += utf8_encode_cp(cp);
        }
    }
    out += quote;
    return out;
}

std::string pad_left_cp(const std::string& utf8, std::size_t width) {
    const std::size_t n = char_len(utf8);
    return n >= width ? utf8 : std::string(width - n, ' ') + utf8;
}

std::string pad_right_cp(const std::string& utf8, std::size_t width) {
    const std::size_t n = char_len(utf8);
    return n >= width ? utf8 : utf8 + std::string(width - n, ' ');
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

void log_warning(const std::string& msg) {
    std::fputs((timestamp() + " WARNING " + msg + "\n").c_str(), stdout);
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
    std::string digits = std::to_string(n < 0 ? -n : n);
    std::string out;
    int count = 0;
    for (std::size_t i = digits.size(); i-- > 0;) {
        out += digits[i];
        if (++count % 3 == 0 && i != 0) out += ',';
    }
    if (n < 0) out += '-';
    return std::string(out.rbegin(), out.rend());
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

namespace {
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

std::string json_dump(const boost::json::value& v) {
    std::string out;
    dump_impl(v, out);
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

}  // namespace frat
