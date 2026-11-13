// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "spt/spt_script.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mgi::spt {

namespace {

constexpr char32_t kAsciiMaxCodepoint = 0x7F;
constexpr std::size_t kMaxBytecodeNoiseLen = 3;
constexpr std::uint8_t kSptXorMask = 0xFF;
constexpr std::size_t kMinValidSptSize = 0x44;
constexpr std::size_t kMagicOffset = 0x30;
constexpr std::string_view kSptHeaderMagic = "SPTHEADER0";
constexpr std::size_t kStringCountOffset = 0x18;
constexpr std::size_t kStringCountEndOffset = 0x1C;
constexpr unsigned kShiftByte1 = 8;
constexpr unsigned kShiftByte2 = 16;
constexpr unsigned kShiftByte3 = 24;

constexpr char32_t kCjkSymbolsFirst = 0x3000;
constexpr char32_t kCjkSymbolsLast = 0x303F;
constexpr char32_t kHiraganaFirst = 0x3040;
constexpr char32_t kHiraganaLast = 0x309F;
constexpr char32_t kKatakanaFirst = 0x30A0;
constexpr char32_t kKatakanaLast = 0x30FF;
constexpr char32_t kCjkExtAFirst = 0x3400;
constexpr char32_t kCjkExtALast = 0x4DBF;
constexpr char32_t kCjkUnifiedFirst = 0x4E00;
constexpr char32_t kCjkUnifiedLast = 0x9FFF;
constexpr char32_t kFullwidthFormsFirst = 0xFF00;
constexpr char32_t kFullwidthFormsLast = 0xFFEF;

constexpr std::size_t kMaxSpeakerPlateChars = 10;
constexpr std::size_t kMaxNameEntryChars = 6;
constexpr std::size_t kMinNonJpStringBytes = 4;
constexpr std::size_t kSptStringAlignment = 4;

// Codepoint count -- every length test in classify() counts CHARACTERS, not
// bytes; a CP932 line is 2-3 bytes per character and byte lengths would move
// every threshold.
std::size_t char_len(const std::string& utf8) {
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        utf8_next(utf8, i);
        ++n;
    }
    return n;
}

bool all_ascii(const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        if (utf8_next(utf8, i) > kAsciiMaxCodepoint) {
            return false;
        }
    }
    return true;
}

bool starts_with(const std::string& s, std::string_view prefix) {
    return s.rfind(prefix, 0) == 0;
}

// Three-byte-or-shorter strings that are really opcode operands, not text.
bool is_bytecode(std::size_t byte_len, const std::string& text) {
    if (byte_len > kMaxBytecodeNoiseLen) {
        return false;
    }
    static constexpr std::string_view kNoiseTokens[] = {
        "ff", "UU", "VU", "33", "DD", "22", "f", "U", "V", "D"};
    for (const std::string_view token : kNoiseTokens) {
        if (text == token) {
            return true;
        }
    }
    return false;
}

}  // namespace

Bytes decrypt(const Bytes& data) {
    Bytes out(data.size());
    for (std::size_t i = 0; i < data.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(data[i] ^ kSptXorMask);
    }
    return out;
}

bool is_valid(const Bytes& dec) {
    return dec.size() >= kMinValidSptSize &&
           std::memcmp(dec.data() + kMagicOffset, kSptHeaderMagic.data(),
                       kSptHeaderMagic.size()) == 0;
}

std::uint32_t header_string_count(const Bytes& dec) {
    if (dec.size() < kStringCountEndOffset) {
        return 0;
    }
    return static_cast<std::uint32_t>(dec[kStringCountOffset]) |
           (static_cast<std::uint32_t>(dec[kStringCountOffset + 1]) << kShiftByte1) |
           (static_cast<std::uint32_t>(dec[kStringCountOffset + 2]) << kShiftByte2) |
           (static_cast<std::uint32_t>(dec[kStringCountOffset + 3]) << kShiftByte3);
}

bool has_japanese(const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        if ((cp >= kHiraganaFirst && cp <= kHiraganaLast) ||
            (cp >= kKatakanaFirst && cp <= kKatakanaLast) ||
            (cp >= kCjkUnifiedFirst && cp <= kCjkUnifiedLast) ||
            (cp >= kCjkExtAFirst && cp <= kCjkExtALast) ||
            (cp >= kFullwidthFormsFirst && cp <= kFullwidthFormsLast) ||
            (cp >= kCjkSymbolsFirst && cp <= kCjkSymbolsLast)) {
            return true;
        }
    }
    return false;
}

std::string classify(const std::string& raw) {
    const std::string text = trim(raw);
    if (text.empty()) {
        return "empty";
    }

    const std::size_t crlf = text.find("\r\n");
    if (crlf != std::string::npos) {
        // First segment short and unquoted -> a speaker plate, so dialogue.
        const std::string first = text.substr(0, crlf);
        if (char_len(first) <= kMaxSpeakerPlateChars &&
            first.find("「") == std::string::npos) {
            return "dialogue";
        }
        return "narrative";
    }
    if (text.find("「") != std::string::npos && text.find("」") != std::string::npos) {
        return "dialogue";
    }
    if (starts_with(text, "★") || starts_with(text, "■") || starts_with(text, "●")) {
        return "menu";
    }
    if (all_ascii(text)) {
        return "label";
    }
    if (char_len(text) <= kMaxNameEntryChars && has_japanese(text)) {
        return "name";
    }
    if (has_japanese(text)) {
        return "narrative";
    }
    return "other";
}

std::vector<SptString> extract_strings(const Bytes& dec, std::size_t skip_offset) {
    std::vector<SptString> out;
    const std::size_t fsize = dec.size();
    std::size_t pos = skip_offset;

    while (pos < fsize) {
        const auto* nul = static_cast<const std::uint8_t*>(
            std::memchr(dec.data() + pos, 0, fsize - pos));
        if (nul == nullptr) {
            break;
        }
        const std::size_t end = static_cast<std::size_t>(nul - dec.data());
        if (end == pos) {
            ++pos;
            continue;
        }

        const std::size_t byte_len = end - pos;
        // Strict decode only: an undecodable run is opcode data, so skip it.
        if (const auto text = cp932_to_utf8_strict(dec.data() + pos, byte_len)) {
            const bool jp = has_japanese(*text);
            if (!is_bytecode(byte_len, *text) && (jp || byte_len >= kMinNonJpStringBytes)) {
                SptString s;
                s.offset = static_cast<std::uint32_t>(pos);
                s.byte_len = static_cast<std::uint32_t>(byte_len);
                s.text = *text;
                s.has_jp = jp;
                out.push_back(std::move(s));
            }
        }

        // SPT text blocks are 4-byte aligned; skip the padding so it doesn't
        // get absorbed as prefix garbage on the next string.
        pos = ((end + kSptStringAlignment) / kSptStringAlignment) * kSptStringAlignment;
    }
    return out;
}

}  // namespace mgi::spt
