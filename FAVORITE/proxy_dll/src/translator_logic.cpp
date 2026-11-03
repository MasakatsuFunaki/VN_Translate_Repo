// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace translator_logic {

namespace {

constexpr unsigned char kUtf8Bom0 = 0xEFu;
constexpr unsigned char kUtf8Bom1 = 0xBBu;
constexpr unsigned char kUtf8Bom2 = 0xBFu;
constexpr size_t kUtf8BomByteCount = 3u;

constexpr unsigned char kUtf8AsciiMask = 0x80u;
constexpr unsigned char kUtf8TwoByteMask = 0xE0u;
constexpr unsigned char kUtf8TwoBytePrefix = 0xC0u;
constexpr unsigned char kUtf8ThreeByteMask = 0xF0u;
constexpr unsigned char kUtf8ThreeBytePrefix = 0xE0u;
constexpr unsigned char kUtf8FourByteMask = 0xF8u;
constexpr unsigned char kUtf8FourBytePrefix = 0xF0u;

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

constexpr size_t kWordWrapExtraReserve = 64u;
constexpr size_t kControlNewlineLength = 2u;
constexpr size_t kCp932SingleByteLength = 1u;
constexpr size_t kCp932DoubleByteLength = 2u;

constexpr bool IsCp932LeadByte(unsigned char b) noexcept {
    return (b >= kCp932LeadLowMin && b <= kCp932LeadLowMax) ||
           (b >= kCp932LeadHighMin && b <= kCp932LeadHighMax);
}

constexpr bool IsAsciiWhitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

}  // namespace

void UnescapeInPlace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char c = s[i + 1];
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
        s.replace(0, kControlNewlineLength, " ");
        return true;
    }
    return false;
}

size_t WordWrapForEngine(std::string& s, size_t line_chars, size_t start_col) {
    std::string out;
    out.reserve(s.size() + kWordWrapExtraReserve);
    size_t col = start_col;
    size_t i = 0;

    while (i < s.size()) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            out += '\\';
            out += 'n';
            i += kControlNewlineLength;
            col = 0;
            continue;
        }

        if (s[i] == ' ') {
            size_t sp = 0;
            while (i + sp < s.size() && s[i + sp] == ' ') {
                ++sp;
            }

            const size_t wstart = i + sp;
            size_t wend = wstart;
            while (wend < s.size() && s[wend] != ' ' &&
                   !(s[wend] == '\\' && wend + 1 < s.size() && s[wend + 1] == 'n')) {
                ++wend;
            }
            const size_t wlen = wend - wstart;

            if (wlen > 0 && col > 0 && col + sp + wlen > line_chars) {
                out += '\\';
                out += 'n';
                col = 0;
                i += sp;
                continue;
            }
            out.append(sp, ' ');
            col += sp;
            i += sp;
            continue;
        }

        if (col > line_chars && col == start_col && i == 0) {
            out += '\\';
            out += 'n';
            col = 0;
        }

        const size_t wstart = i;
        while (i < s.size() && s[i] != ' ' &&
               !(s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n')) {
            ++i;
        }
        out.append(s, wstart, i - wstart);
        col += i - wstart;
    }

    s = std::move(out);
    return col;
}

size_t Utf8SafeTruncate(const char* s, size_t max_bytes) {
    if (!s) {
        return 0;
    }
    size_t i = 0;
    while (i < max_bytes && s[i]) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t cl = 1;
        if ((c & kUtf8AsciiMask) == 0) {
            cl = 1;
        } else if ((c & kUtf8TwoByteMask) == kUtf8TwoBytePrefix) {
            cl = 2;
        } else if ((c & kUtf8ThreeByteMask) == kUtf8ThreeBytePrefix) {
            cl = 3;
        } else if ((c & kUtf8FourByteMask) == kUtf8FourBytePrefix) {
            cl = 4;
        }
        if (i + cl > max_bytes) {
            break;
        }
        i += cl;
    }
    return i;
}

int ParseTsvBuffer(const char* data, size_t len, TranslationMap& out) {
    if (!data || len == 0) {
        return 0;
    }

    size_t start = 0;
    if (len >= kUtf8BomByteCount &&
        static_cast<unsigned char>(data[0]) == kUtf8Bom0 &&
        static_cast<unsigned char>(data[1]) == kUtf8Bom1 &&
        static_cast<unsigned char>(data[2]) == kUtf8Bom2) {
        start = kUtf8BomByteCount;
    }

    int count = 0;
    size_t i = start;
    while (i < len) {
        const size_t line_start = i;
        while (i < len && data[i] != '\n' && data[i] != '\r') {
            ++i;
        }
        const size_t line_end = i;
        while (i < len && (data[i] == '\r' || data[i] == '\n')) {
            ++i;
        }

        size_t tab = line_start;
        while (tab < line_end && data[tab] != '\t') {
            ++tab;
        }
        if (tab == line_start || tab >= line_end) {
            continue;
        }

        std::string jp(data + line_start, tab - line_start);
        std::string en(data + tab + 1, line_end - (tab + 1));
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
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(f);
        return false;
    }
    std::vector<char> buf(static_cast<size_t>(sz));
    const size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (n == 0) {
        return false;
    }
    const int added = ParseTsvBuffer(buf.data(), n, out);
    return added > 0;
}

const std::string* FindTranslation(const TranslationMap& map,
                                   const std::string& src) {
    const auto it = map.find(src);
    return (it != map.end()) ? &it->second : nullptr;
}

std::string NormalizeCp932LookupKey(const char* src, size_t len) {
    if (!src || len == 0) {
        return {};
    }

    struct Span {
        size_t offset;
        size_t length;
    };

    std::vector<Span> spans;
    spans.reserve(len);
    size_t i = 0;
    while (i < len) {
        const unsigned char b = static_cast<unsigned char>(src[i]);
        if (IsCp932LeadByte(b) && i + 1 < len) {
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
            const unsigned char b1 = static_cast<unsigned char>(src[sp.offset + 1]);
            return b0 == kCp932PunctLeadByte && b1 == kCp932FullwidthSpaceTrail;
        }
        return false;
    };

    auto is_pair = [&](const Span& sp, unsigned char b0, unsigned char b1) -> bool {
        return sp.length == kCp932DoubleByteLength &&
               static_cast<unsigned char>(src[sp.offset]) == b0 &&
               static_cast<unsigned char>(src[sp.offset + 1]) == b1;
    };

    size_t lo = 0;
    size_t hi = spans.size();
    auto trim_spaces = [&]() {
        while (lo < hi && is_space(spans[lo])) {
            ++lo;
        }
        while (lo < hi && is_space(spans[hi - 1])) {
            --hi;
        }
    };

    trim_spaces();

    if (hi - lo >= kCp932DoubleByteLength) {
        const bool kagi =
            is_pair(spans[lo], kCp932PunctLeadByte, kCp932CornerOpenTrail) &&
            is_pair(spans[hi - 1], kCp932PunctLeadByte, kCp932CornerCloseTrail);
        const bool nijuu =
            is_pair(spans[lo], kCp932PunctLeadByte, kCp932DoubleCornerOpenTrail) &&
            is_pair(spans[hi - 1], kCp932PunctLeadByte, kCp932DoubleCornerCloseTrail);
        if (kagi || nijuu) {
            ++lo;
            --hi;
            trim_spaces();
        }
    }

    if (lo >= hi) {
        return {};
    }
    const size_t byte_start = spans[lo].offset;
    const size_t byte_end = spans[hi - 1].offset + spans[hi - 1].length;
    return std::string(src + byte_start, byte_end - byte_start);
}

std::string WrapAsciiToColumnGrid(const std::string& text, size_t max_cols) {
    if (text.empty() || max_cols == 0) {
        return text;
    }

    std::string out;
    out.reserve(text.size() + kWordWrapExtraReserve);
    size_t col = 0;
    size_t i = 0;
    const size_t n = text.size();

    while (i < n) {
        if (IsAsciiWhitespace(text[i])) {
            bool has_newline = false;
            size_t sp = 0;
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
                for (size_t k = 0; k < sp; ++k) {
                    out += ' ';
                    col = (col >= max_cols) ? 1 : (col + 1);
                }
                break;
            }

            size_t wend = i;
            while (wend < n && !IsAsciiWhitespace(text[wend])) {
                ++wend;
            }
            const size_t wlen = wend - i;

            if (has_newline) {
                if (col > 0 && col < max_cols) {
                    out.append(max_cols - col, ' ');
                }
                col = 0;
                continue;
            }
            if (col > 0) {
                const size_t sep = (sp > 0) ? sp : 1;
                if (wlen <= max_cols && col + sep + wlen > max_cols) {
                    if (col < max_cols) {
                        out.append(max_cols - col, ' ');
                    }
                    col = 0;
                    continue;
                }
                for (size_t k = 0; k < sep; ++k) {
                    out += ' ';
                    col = (col >= max_cols) ? 1 : (col + 1);
                }
                continue;
            }
            for (size_t k = 0; k < sp; ++k) {
                out += ' ';
                col = (col >= max_cols) ? 1 : (col + 1);
            }
            continue;
        }

        size_t wend = i;
        while (wend < n && !IsAsciiWhitespace(text[wend])) {
            ++wend;
        }
        const size_t wlen = wend - i;
        out.append(text, i, wlen);
        const size_t total = col + wlen;
        if (total == 0) {
            col = 0;
        } else {
            const size_t rem = total % max_cols;
            col = (rem == 0) ? max_cols : rem;
        }
        i = wend;
    }

    return out;
}

}  // namespace translator_logic
