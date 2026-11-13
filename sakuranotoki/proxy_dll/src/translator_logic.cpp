// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace tr {

namespace {

constexpr unsigned char kBomByte0 = 0xEFU;
constexpr unsigned char kBomByte1 = 0xBBU;
constexpr unsigned char kBomByte2 = 0xBFU;
constexpr std::size_t kUtf8BomLength = 3;
constexpr std::size_t kSecondByteOffset = 1;
constexpr std::size_t kThirdByteOffset = 2;
constexpr std::size_t kFourthByteOffset = 3;
constexpr std::size_t kOneByteLen = 1;
constexpr std::size_t kTwoByteLen = 2;
constexpr std::size_t kThreeByteLen = 3;
constexpr std::size_t kFourByteLen = 4;
constexpr unsigned char kAsciiLimit = 0x80U;
constexpr unsigned char kTwoByteMask = 0xE0U;
constexpr unsigned char kTwoByteTag = 0xC0U;
constexpr unsigned char kTwoBytePayloadMask = 0x1FU;
constexpr unsigned char kThreeByteMask = 0xF0U;
constexpr unsigned char kThreeByteTag = 0xE0U;
constexpr unsigned char kThreeBytePayloadMask = 0x0FU;
constexpr unsigned char kFourByteMask = 0xF8U;
constexpr unsigned char kFourByteTag = 0xF0U;
constexpr unsigned char kFourBytePayloadMask = 0x07U;
constexpr unsigned char kContMask = 0xC0U;
constexpr unsigned char kContTag = 0x80U;
constexpr unsigned char kContPayloadMask = 0x3FU;
constexpr unsigned kShift6 = 6U;
constexpr unsigned kShift12 = 12U;
constexpr unsigned kShift18 = 18U;
constexpr unsigned kHiraganaMin = 0x3040U;
constexpr unsigned kHiraganaMax = 0x309FU;
constexpr unsigned kKatakanaMin = 0x30A0U;
constexpr unsigned kKatakanaMax = 0x30FFU;
constexpr unsigned kCjkExtAMin = 0x3400U;
constexpr unsigned kCjkExtAMax = 0x4DBFU;
constexpr unsigned kCjkUnifiedMin = 0x4E00U;
constexpr unsigned kCjkUnifiedMax = 0x9FFFU;

bool starts_with_utf8_bom(std::string_view s) noexcept {
    return s.size() >= kUtf8BomLength &&
           static_cast<unsigned char>(s[0]) == kBomByte0 &&
           static_cast<unsigned char>(s[kSecondByteOffset]) == kBomByte1 &&
           static_cast<unsigned char>(s[kThirdByteOffset]) == kBomByte2;
}

bool is_continuation_byte(unsigned char b) noexcept {
    return (b & kContMask) == kContTag;
}

void unescape(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            const char next = s[++i];
            switch (next) {
            case 't':  out.push_back('\t'); break;
            case 'r':  out.push_back('\r'); break;
            case 'n':  out.push_back('\n'); break;
            case '\\': out.push_back('\\'); break;
            default:
                out.push_back('\\');
                out.push_back(next);
                break;
            }
            continue;
        }
        out.push_back(c);
    }
    s.swap(out);
}

}  // namespace

std::size_t load_tsv(std::string_view tsv,
                     std::unordered_map<std::string, std::string>& out) {
    if (starts_with_utf8_bom(tsv)) {
        tsv.remove_prefix(kUtf8BomLength);
    }

    std::size_t inserted = 0;
    std::size_t i = 0;
    while (i < tsv.size()) {
        std::size_t end = tsv.find('\n', i);
        if (end == std::string_view::npos) {
            end = tsv.size();
        }
        std::string_view line = tsv.substr(i, end - i);
        i = end + 1;

        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos) {
            continue;
        }

        std::string jp(line.substr(0, tab));
        std::string en(line.substr(tab + 1));
        unescape(jp);
        unescape(en);

        if (jp.empty() || en.empty()) {
            continue;
        }
        if (out.emplace(std::move(jp), std::move(en)).second) {
            ++inserted;
        }
    }
    return inserted;
}

std::size_t load_tsv_file(const std::string& path,
                          std::unordered_map<std::string, std::string>& out) {
    if (path.empty()) {
        return SIZE_MAX;
    }
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) {
        return SIZE_MAX;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string body = ss.str();
    return load_tsv(body, out);
}

bool lookup(const std::unordered_map<std::string, std::string>& table,
            std::string_view jp, std::string& out) {
    const auto it = table.find(std::string(jp));
    if (it == table.end()) {
        return false;
    }
    out = it->second;
    return true;
}

bool has_japanese_utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        unsigned cp = 0;
        std::size_t len = kOneByteLen;
        if (b < kAsciiLimit) {
            cp = b;
            len = kOneByteLen;
        } else if ((b & kTwoByteMask) == kTwoByteTag &&
                   i + kSecondByteOffset < s.size()) {
            const auto b1 = static_cast<unsigned char>(s[i + kSecondByteOffset]);
            if (!is_continuation_byte(b1)) {
                return false;
            }
            cp = (static_cast<unsigned>(b & kTwoBytePayloadMask) << kShift6) |
                 static_cast<unsigned>(b1 & kContPayloadMask);
            len = kTwoByteLen;
        } else if ((b & kThreeByteMask) == kThreeByteTag &&
                   i + kThirdByteOffset < s.size()) {
            const auto b1 = static_cast<unsigned char>(s[i + kSecondByteOffset]);
            const auto b2 = static_cast<unsigned char>(s[i + kThirdByteOffset]);
            if (!is_continuation_byte(b1) || !is_continuation_byte(b2)) {
                return false;
            }
            cp = (static_cast<unsigned>(b & kThreeBytePayloadMask) << kShift12) |
                 (static_cast<unsigned>(b1 & kContPayloadMask) << kShift6) |
                 static_cast<unsigned>(b2 & kContPayloadMask);
            len = kThreeByteLen;
        } else if ((b & kFourByteMask) == kFourByteTag &&
                   i + kFourthByteOffset < s.size()) {
            const auto b1 = static_cast<unsigned char>(s[i + kSecondByteOffset]);
            const auto b2 = static_cast<unsigned char>(s[i + kThirdByteOffset]);
            const auto b3 = static_cast<unsigned char>(s[i + kFourthByteOffset]);
            if (!is_continuation_byte(b1) || !is_continuation_byte(b2) ||
                !is_continuation_byte(b3)) {
                return false;
            }
            cp = (static_cast<unsigned>(b & kFourBytePayloadMask) << kShift18) |
                 (static_cast<unsigned>(b1 & kContPayloadMask) << kShift12) |
                 (static_cast<unsigned>(b2 & kContPayloadMask) << kShift6) |
                 static_cast<unsigned>(b3 & kContPayloadMask);
            len = kFourByteLen;
        } else {
            return false;
        }
        if ((cp >= kHiraganaMin && cp <= kHiraganaMax) ||
            (cp >= kKatakanaMin && cp <= kKatakanaMax) ||
            (cp >= kCjkUnifiedMin && cp <= kCjkUnifiedMax) ||
            (cp >= kCjkExtAMin && cp <= kCjkExtAMax)) {
            return true;
        }
        i += len;
    }
    return false;
}

}  // namespace tr
