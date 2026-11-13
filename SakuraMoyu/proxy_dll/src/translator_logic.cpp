// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <filesystem>
#include <fstream>
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <utility>
#include <vector>

namespace translator_logic {

namespace {

constexpr unsigned char kUtf8Bom0 = 0xEFu;
constexpr unsigned char kUtf8Bom1 = 0xBBu;
constexpr unsigned char kUtf8Bom2 = 0xBFu;
constexpr std::size_t kUtf8BomByteCount = 3u;

constexpr unsigned char kUtf8AsciiMask = 0x80u;
constexpr unsigned char kUtf8TwoByteMask = 0xE0u;
constexpr unsigned char kUtf8TwoBytePrefix = 0xC0u;
constexpr unsigned char kUtf8ThreeByteMask = 0xF0u;
constexpr unsigned char kUtf8ThreeBytePrefix = 0xE0u;
constexpr unsigned char kUtf8FourByteMask = 0xF8u;
constexpr unsigned char kUtf8FourBytePrefix = 0xF0u;
constexpr std::size_t kUtf8OneByteLength = 1u;
constexpr std::size_t kUtf8TwoByteLength = 2u;
constexpr std::size_t kUtf8ThreeByteLength = 3u;
constexpr std::size_t kUtf8FourByteLength = 4u;

constexpr unsigned char kCp932LeadLowMin = 0x81u;
constexpr unsigned char kCp932LeadLowMax = 0x9Fu;
constexpr unsigned char kCp932LeadHighMin = 0xE0u;
constexpr unsigned char kCp932LeadHighMax = 0xFCu;

constexpr unsigned char kCp932PunctLeadByte = 0x81u;
constexpr unsigned char kCp932FullwidthSpaceTrail = 0x40u;
constexpr unsigned char kCp932CornerOpenTrail = 0x75u;
constexpr unsigned char kCp932CornerCloseTrail = 0x76u;
constexpr unsigned char kCp932DoubleCornerOpenTrail = 0x77u;
constexpr unsigned char kCp932DoubleCornerCloseTrail = 0x78u;

constexpr std::size_t kWordWrapExtraReserve = 64u;
constexpr std::size_t kControlNewlineLength = 2u;
constexpr std::size_t kCp932SingleByteLength = 1u;
constexpr std::size_t kCp932DoubleByteLength = 2u;
constexpr std::size_t kSingleByteElementSize = 1u;
constexpr int kMinimumPositiveParsedCount = 0;
constexpr long kFileStartOffset = 0L;
constexpr long kEmptyOrFailedTellSize = 0L;

constexpr bool IsCp932LeadByte(unsigned char b) noexcept {
    return (b >= kCp932LeadLowMin && b <= kCp932LeadLowMax) ||
           (b >= kCp932LeadHighMin && b <= kCp932LeadHighMax);
}

constexpr bool IsAsciiWhitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

constexpr std::size_t Utf8LeadSequenceLength(unsigned char c) noexcept {
    if ((c & kUtf8AsciiMask) == 0u) {
        return kUtf8OneByteLength;
    }
    if ((c & kUtf8TwoByteMask) == kUtf8TwoBytePrefix) {
        return kUtf8TwoByteLength;
    }
    if ((c & kUtf8ThreeByteMask) == kUtf8ThreeBytePrefix) {
        return kUtf8ThreeByteLength;
    }
    if ((c & kUtf8FourByteMask) == kUtf8FourBytePrefix) {
        return kUtf8FourByteLength;
    }
    return kUtf8OneByteLength;
}

}  // namespace

void UnescapeInPlace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0u; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1u < s.size()) {
            const char c = s[i + 1u];
            if (c == 'r') {
                out += '\r';
                ++i;
                continue;
            }
            if (c == 'n') {
                out += '\n';
                ++i;
                continue;
            }
            if (c == 't') {
                out += '\t';
                ++i;
                continue;
            }
            if (c == '\\') {
                out += '\\';
                ++i;
                continue;
            }
        }
        out += s[i];
    }
    s = std::move(out);
}

bool StripLeadingControlNewline(std::string& s) {
    if (s.size() >= kControlNewlineLength && s[0] == '\\' && s[1] == 'n') {
        s.replace(0u, kControlNewlineLength, " ");
        return true;
    }
    return false;
}

std::size_t WordWrapForEngine(std::string& s, std::size_t line_chars, std::size_t start_col) {
    std::string out;
    out.reserve(s.size() + kWordWrapExtraReserve);
    std::size_t col = start_col;
    std::size_t i = 0u;

    while (i < s.size()) {
        if (s[i] == '\\' && i + 1u < s.size() && s[i + 1u] == 'n') {
            out += '\\';
            out += 'n';
            i += kControlNewlineLength;
            col = 0u;
            continue;
        }

        if (s[i] == ' ') {
            std::size_t sp = 0u;
            while (i + sp < s.size() && s[i + sp] == ' ') {
                ++sp;
            }

            const std::size_t wstart = i + sp;
            std::size_t wend = wstart;
            while (wend < s.size() && s[wend] != ' ' &&
                   !(s[wend] == '\\' && wend + 1u < s.size() && s[wend + 1u] == 'n')) {
                ++wend;
            }
            const std::size_t wlen = wend - wstart;

            if (wlen > 0u && col > 0u && col + sp + wlen > line_chars) {
                out += '\\';
                out += 'n';
                col = 0u;
                i += sp;
                continue;
            }
            out.append(sp, ' ');
            col += sp;
            i += sp;
            continue;
        }

        if (col > line_chars && col == start_col && i == 0u) {
            out += '\\';
            out += 'n';
            col = 0u;
        }

        const std::size_t wstart = i;
        while (i < s.size() && s[i] != ' ' &&
               !(s[i] == '\\' && i + 1u < s.size() && s[i + 1u] == 'n')) {
            ++i;
        }
        out.append(s, wstart, i - wstart);
        col += i - wstart;
    }

    s = std::move(out);
    return col;
}

std::size_t Utf8SafeTruncate(const char* s, std::size_t max_bytes) {
    if (s == nullptr) {
        return 0u;
    }
    std::size_t i = 0u;
    while (i < max_bytes && s[i] != '\0') {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const std::size_t cl = Utf8LeadSequenceLength(c);
        if (i + cl > max_bytes) {
            break;
        }
        i += cl;
    }
    return i;
}

int ParseTsvBuffer(const char* data, std::size_t len, TranslationMap& out) {
    if (data == nullptr || len == 0u) {
        return 0;
    }

    std::size_t start = 0u;
    if (len >= kUtf8BomByteCount &&
        static_cast<unsigned char>(data[0]) == kUtf8Bom0 &&
        static_cast<unsigned char>(data[1]) == kUtf8Bom1 &&
        static_cast<unsigned char>(data[2]) == kUtf8Bom2) {
        start = kUtf8BomByteCount;
    }

    int count = 0;
    std::size_t i = start;
    while (i < len) {
        const std::size_t line_start = i;
        while (i < len && data[i] != '\n' && data[i] != '\r') {
            ++i;
        }
        const std::size_t line_end = i;
        while (i < len && (data[i] == '\r' || data[i] == '\n')) {
            ++i;
        }

        std::size_t tab = line_start;
        while (tab < line_end && data[tab] != '\t') {
            ++tab;
        }
        if (tab == line_start || tab >= line_end) {
            continue;
        }

        std::string jp(data + line_start, tab - line_start);
        std::string en(data + tab + 1u, line_end - (tab + 1u));
        UnescapeInPlace(jp);
        UnescapeInPlace(en);
        if (jp.empty() || en.empty()) {
            continue;
        }
        out[jp] = en;
        ++count;
    }
    return count;
}

bool LoadTsvFile(const std::string& path, TranslationMap& out) {
    if (path.empty()) {
        return false;
    }
#ifdef _WIN32
    const int wide_len = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        path.data(),
        static_cast<int>(path.size()),
        nullptr,
        0);
    if (wide_len <= 0) {
        return false;
    }
    std::wstring wpath(static_cast<std::size_t>(wide_len), L'\0');
    const int converted = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        path.data(),
        static_cast<int>(path.size()),
        wpath.data(),
        wide_len);
    if (converted != wide_len) {
        return false;
    }

    using ScopedFile = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
    ScopedFile file(_wfopen(wpath.c_str(), L"rb"), &std::fclose);
    if (!file) {
        return false;
    }
    if (std::fseek(file.get(), kFileStartOffset, SEEK_END) != 0) {
        return false;
    }
    const long sz = std::ftell(file.get());
    if (sz <= kEmptyOrFailedTellSize) {
        return false;
    }
    if (std::fseek(file.get(), kFileStartOffset, SEEK_SET) != 0) {
        return false;
    }
    std::vector<char> buf(static_cast<std::size_t>(sz));
    const std::size_t bytes_read =
        std::fread(buf.data(), kSingleByteElementSize, buf.size(), file.get());
    if (bytes_read == 0u) {
        return false;
    }
    return ParseTsvBuffer(buf.data(), bytes_read, out) > kMinimumPositiveParsedCount;
#else
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const std::streamsize sz = file.tellg();
    if (sz <= 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::vector<char> buf(static_cast<std::size_t>(sz));
    if (!file.read(buf.data(), sz)) {
        return false;
    }
    return ParseTsvBuffer(buf.data(), buf.size(), out) > kMinimumPositiveParsedCount;
#endif
}

const std::string* FindTranslation(const TranslationMap& map,
                                   const std::string& src) {
    const auto it = map.find(src);
    return (it != map.end()) ? &it->second : nullptr;
}

std::string NormalizeCp932LookupKey(const char* src, std::size_t len) {
    if (src == nullptr || len == 0u) {
        return {};
    }

    struct Span {
        std::size_t offset = 0u;
        std::size_t length = 0u;
    };

    std::vector<Span> spans;
    spans.reserve(len);
    std::size_t i = 0u;
    while (i < len) {
        const unsigned char b = static_cast<unsigned char>(src[i]);
        if (IsCp932LeadByte(b) && i + 1u < len) {
            spans.push_back({i, kCp932DoubleByteLength});
            i += kCp932DoubleByteLength;
            continue;
        }
        spans.push_back({i, kCp932SingleByteLength});
        i += kCp932SingleByteLength;
    }

    auto is_space = [&](const Span& sp) -> bool {
        if (sp.length == kCp932SingleByteLength) {
            return IsAsciiWhitespace(src[sp.offset]);
        }
        if (sp.length == kCp932DoubleByteLength) {
            const unsigned char b0 = static_cast<unsigned char>(src[sp.offset]);
            const unsigned char b1 = static_cast<unsigned char>(src[sp.offset + 1u]);
            return b0 == kCp932PunctLeadByte && b1 == kCp932FullwidthSpaceTrail;
        }
        return false;
    };

    auto is_pair = [&](const Span& sp, unsigned char b0, unsigned char b1) -> bool {
        return sp.length == kCp932DoubleByteLength &&
               static_cast<unsigned char>(src[sp.offset]) == b0 &&
               static_cast<unsigned char>(src[sp.offset + 1u]) == b1;
    };

    std::size_t lo = 0u;
    std::size_t hi = spans.size();
    auto trim_spaces = [&]() {
        while (lo < hi && is_space(spans[lo])) {
            ++lo;
        }
        while (lo < hi && is_space(spans[hi - 1u])) {
            --hi;
        }
    };

    trim_spaces();

    if (hi - lo >= kCp932DoubleByteLength) {
        const bool kagi =
            is_pair(spans[lo], kCp932PunctLeadByte, kCp932CornerOpenTrail) &&
            is_pair(spans[hi - 1u], kCp932PunctLeadByte, kCp932CornerCloseTrail);
        const bool nijuu =
            is_pair(spans[lo], kCp932PunctLeadByte, kCp932DoubleCornerOpenTrail) &&
            is_pair(spans[hi - 1u], kCp932PunctLeadByte, kCp932DoubleCornerCloseTrail);
        if (kagi || nijuu) {
            ++lo;
            --hi;
            trim_spaces();
        }
    }

    if (lo >= hi) {
        return {};
    }
    const std::size_t byte_start = spans[lo].offset;
    const std::size_t byte_end = spans[hi - 1u].offset + spans[hi - 1u].length;
    return std::string(src + byte_start, byte_end - byte_start);
}

std::string WrapAsciiToColumnGrid(const std::string& text, std::size_t max_cols) {
    if (text.empty() || max_cols == 0u) {
        return text;
    }

    std::string out;
    out.reserve(text.size() + kWordWrapExtraReserve);
    std::size_t col = 0u;
    std::size_t i = 0u;
    const std::size_t n = text.size();

    while (i < n) {
        if (IsAsciiWhitespace(text[i])) {
            bool has_newline = false;
            std::size_t sp = 0u;
            while (i < n && IsAsciiWhitespace(text[i])) {
                if (text[i] == '\n') {
                    has_newline = true;
                }
                if (text[i] == ' ') {
                    ++sp;
                }
                ++i;
            }
            if (i >= n) {
                for (std::size_t k = 0u; k < sp; ++k) {
                    out += ' ';
                    col = (col >= max_cols) ? 1u : (col + 1u);
                }
                break;
            }

            std::size_t wend = i;
            while (wend < n && !IsAsciiWhitespace(text[wend])) {
                ++wend;
            }
            const std::size_t wlen = wend - i;

            if (has_newline) {
                if (col > 0u && col < max_cols) {
                    out.append(max_cols - col, ' ');
                }
                col = 0u;
                continue;
            }
            if (col > 0u) {
                const std::size_t sep = (sp > 0u) ? sp : 1u;
                if (wlen <= max_cols && col + sep + wlen > max_cols) {
                    if (col < max_cols) {
                        out.append(max_cols - col, ' ');
                    }
                    col = 0u;
                    continue;
                }
                for (std::size_t k = 0u; k < sep; ++k) {
                    out += ' ';
                    col = (col >= max_cols) ? 1u : (col + 1u);
                }
                continue;
            }
            for (std::size_t k = 0u; k < sp; ++k) {
                out += ' ';
                col = (col >= max_cols) ? 1u : (col + 1u);
            }
            continue;
        }

        std::size_t wend = i;
        while (wend < n && !IsAsciiWhitespace(text[wend])) {
            ++wend;
        }
        const std::size_t wlen = wend - i;
        out.append(text, i, wlen);
        const std::size_t total = col + wlen;
        if (total == 0u) {
            col = 0u;
        } else {
            const std::size_t rem = total % max_cols;
            col = (rem == 0u) ? max_cols : rem;
        }
        i = wend;
    }

    return out;
}

}  // namespace translator_logic
