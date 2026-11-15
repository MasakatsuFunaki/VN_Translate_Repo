// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "common/utf8_text.h"

#include <cstdint>

namespace spot_checker {

namespace {

constexpr std::uint8_t kAsciiMaxByte = 0x7FU;
constexpr std::uint8_t kFirstNonAsciiByte = 0x80U;
constexpr std::uint8_t kContinuationMin = 0x80U;
constexpr std::uint8_t kContinuationMax = 0xBFU;
constexpr std::uint8_t kContinuationMask = 0x3FU;
constexpr std::uint8_t kContinuationHeaderBits = 0x80U;

constexpr std::uint8_t kTwoByteLeadMin = 0xC2U;
constexpr std::uint8_t kTwoByteLeadMax = 0xDFU;
constexpr std::uint8_t kTwoBytePayloadMask = 0x1FU;
constexpr std::uint8_t kTwoByteHeaderBits = 0xC0U;

constexpr std::uint8_t kThreeByteLeadMin = 0xE0U;
constexpr std::uint8_t kThreeByteLeadMax = 0xEFU;
constexpr std::uint8_t kThreeByteOverlongLead = 0xE0U;
constexpr std::uint8_t kThreeByteOverlongMinSecond = 0xA0U;
constexpr std::uint8_t kThreeByteSurrogateLead = 0xEDU;
constexpr std::uint8_t kThreeByteSurrogateMaxSecond = 0x9FU;
constexpr std::uint8_t kThreeBytePayloadMask = 0x0FU;
constexpr std::uint8_t kThreeByteHeaderBits = 0xE0U;

constexpr std::uint8_t kFourByteLeadMin = 0xF0U;
constexpr std::uint8_t kFourByteLeadMax = 0xF4U;
constexpr std::uint8_t kFourByteOverlongLead = 0xF0U;
constexpr std::uint8_t kFourByteOverlongMinSecond = 0x90U;
constexpr std::uint8_t kFourByteMaxCodepointLead = 0xF4U;
constexpr std::uint8_t kFourByteMaxCodepointMaxSecond = 0x8FU;
constexpr std::uint8_t kFourBytePayloadMask = 0x07U;
constexpr std::uint8_t kFourByteHeaderBits = 0xF0U;

constexpr int kUtf8Shift6 = 6;
constexpr int kUtf8Shift12 = 12;
constexpr int kUtf8Shift18 = 18;

constexpr char32_t kTwoByteMaxCp = 0x7FFU;
constexpr char32_t kThreeByteMaxCp = 0xFFFFU;
constexpr char32_t kSurrogateHighFirst = 0xD800U;
constexpr char32_t kSurrogateHighLast = 0xDBFFU;
constexpr char32_t kSurrogateLowFirst = 0xDC00U;
constexpr char32_t kSurrogateLowLast = 0xDFFFU;
constexpr char32_t kAstralPlaneBase = 0x10000U;
constexpr int kUtf16SurrogateShift = 10;
constexpr char32_t kUtf16LowTenBitsMask = 0x3FFU;

constexpr char32_t kHiraganaFirst = 0x3040U;
constexpr char32_t kHiraganaLast = 0x309FU;
constexpr char32_t kKatakanaFirst = 0x30A0U;
constexpr char32_t kKatakanaLast = 0x30FFU;
constexpr char32_t kKatakanaPhoneticExtFirst = 0x31F0U;
constexpr char32_t kKatakanaPhoneticExtLast = 0x31FFU;
constexpr char32_t kHalfwidthKatakanaFirst = 0xFF66U;
constexpr char32_t kHalfwidthKatakanaLast = 0xFF9FU;

constexpr char32_t kCjkSymbolsPunctFirst = 0x3000U;
constexpr char32_t kCjkSymbolsPunctLast = 0x303FU;
constexpr char32_t kCjkExtAFirst = 0x3400U;
constexpr char32_t kCjkExtALast = 0x4DBFU;
constexpr char32_t kCjkUnifiedFirst = 0x4E00U;
constexpr char32_t kCjkUnifiedLast = 0x9FFFU;
constexpr char32_t kCjkCompatIdeographsFirst = 0xF900U;
constexpr char32_t kCjkCompatIdeographsLast = 0xFAFFU;
constexpr char32_t kCjkExtBFirst = 0x20000U;
constexpr char32_t kCjkExtBLast = 0x2A6DFU;

constexpr char32_t kFullwidthFormsFirst = 0xFF01U;
constexpr char32_t kFullwidthPunctAndHalfwidthFirst = 0xFF01U;
constexpr char32_t kHalfwidthCjkPunctLast = 0xFF65U;

constexpr char32_t kIdeographicSpace = 0x3000U;
constexpr char32_t kByteOrderMark = 0xFEFFU;
constexpr char32_t kEnDash = 0x2013U;
constexpr char32_t kEmDash = 0x2014U;
constexpr char32_t kHorizontalBar = 0x2015U;
constexpr char32_t kLeftSingleQuote = 0x2018U;
constexpr char32_t kRightSingleQuote = 0x2019U;
constexpr char32_t kLeftDoubleQuote = 0x201CU;
constexpr char32_t kRightDoubleQuote = 0x201DU;
constexpr char32_t kTwoDotLeader = 0x2025U;
constexpr char32_t kHorizontalEllipsis = 0x2026U;
constexpr char32_t kBoxDrawingsLightHorizontal = 0x2500U;
constexpr char32_t kWaveDash = 0x301CU;
constexpr char32_t kFullwidthTilde = 0xFF5EU;

bool is_continuation_byte(std::uint8_t b) noexcept {
    return b >= kContinuationMin && b <= kContinuationMax;
}

bool is_ascii_whitespace(unsigned char b) noexcept {
    return b == ' ' || b == '\t' || b == '\r' || b == '\n';
}

bool is_unicode_trim_cp(char32_t cp) noexcept {
    return cp == U' ' || cp == U'\t' || cp == U'\r' || cp == U'\n' ||
           cp == kIdeographicSpace || cp == kByteOrderMark;
}

unsigned char to_lower_ascii_byte(unsigned char b) noexcept {
    if (b >= 'A' && b <= 'Z') {
        return static_cast<unsigned char>(b - 'A' + 'a');
    }
    return b;
}

bool is_valid_three_byte_second(std::uint8_t b0, std::uint8_t b1) noexcept {
    if (b0 == kThreeByteOverlongLead) {
        return b1 >= kThreeByteOverlongMinSecond && b1 <= kContinuationMax;
    }
    if (b0 == kThreeByteSurrogateLead) {
        return b1 >= kContinuationMin && b1 <= kThreeByteSurrogateMaxSecond;
    }
    return is_continuation_byte(b1);
}

bool is_valid_four_byte_second(std::uint8_t b0, std::uint8_t b1) noexcept {
    if (b0 == kFourByteOverlongLead) {
        return b1 >= kFourByteOverlongMinSecond && b1 <= kContinuationMax;
    }
    if (b0 == kFourByteMaxCodepointLead) {
        return b1 >= kContinuationMin && b1 <= kFourByteMaxCodepointMaxSecond;
    }
    return is_continuation_byte(b1);
}

}  // namespace

char32_t utf8_next(std::string_view text, std::size_t& index) noexcept {
    if (index >= text.size()) {
        return U'\0';
    }

    const auto b0 = static_cast<std::uint8_t>(text[index]);
    if (b0 <= kAsciiMaxByte) {
        ++index;
        return static_cast<char32_t>(b0);
    }

    if (b0 >= kTwoByteLeadMin && b0 <= kTwoByteLeadMax) {
        if (index + 1U >= text.size()) {
            ++index;
            return kUnicodeReplacementChar;
        }
        const auto b1 = static_cast<std::uint8_t>(text[index + 1U]);
        if (!is_continuation_byte(b1)) {
            ++index;
            return kUnicodeReplacementChar;
        }
        index += 2U;
        return (static_cast<char32_t>(b0 & kTwoBytePayloadMask) << kUtf8Shift6) |
               static_cast<char32_t>(b1 & kContinuationMask);
    }

    if (b0 >= kThreeByteLeadMin && b0 <= kThreeByteLeadMax) {
        if (index + 1U >= text.size()) {
            ++index;
            return kUnicodeReplacementChar;
        }
        const auto b1 = static_cast<std::uint8_t>(text[index + 1U]);
        if (!is_valid_three_byte_second(b0, b1)) {
            ++index;
            return kUnicodeReplacementChar;
        }
        if (index + 2U >= text.size()) {
            index += 2U;
            return kUnicodeReplacementChar;
        }
        const auto b2 = static_cast<std::uint8_t>(text[index + 2U]);
        if (!is_continuation_byte(b2)) {
            index += 2U;
            return kUnicodeReplacementChar;
        }
        index += 3U;
        return (static_cast<char32_t>(b0 & kThreeBytePayloadMask) << kUtf8Shift12) |
               (static_cast<char32_t>(b1 & kContinuationMask) << kUtf8Shift6) |
               static_cast<char32_t>(b2 & kContinuationMask);
    }

    if (b0 >= kFourByteLeadMin && b0 <= kFourByteLeadMax) {
        if (index + 1U >= text.size()) {
            ++index;
            return kUnicodeReplacementChar;
        }
        const auto b1 = static_cast<std::uint8_t>(text[index + 1U]);
        if (!is_valid_four_byte_second(b0, b1)) {
            ++index;
            return kUnicodeReplacementChar;
        }
        if (index + 2U >= text.size()) {
            index += 2U;
            return kUnicodeReplacementChar;
        }
        const auto b2 = static_cast<std::uint8_t>(text[index + 2U]);
        if (!is_continuation_byte(b2)) {
            index += 2U;
            return kUnicodeReplacementChar;
        }
        if (index + 3U >= text.size()) {
            index += 3U;
            return kUnicodeReplacementChar;
        }
        const auto b3 = static_cast<std::uint8_t>(text[index + 3U]);
        if (!is_continuation_byte(b3)) {
            index += 3U;
            return kUnicodeReplacementChar;
        }
        index += 4U;
        return (static_cast<char32_t>(b0 & kFourBytePayloadMask) << kUtf8Shift18) |
               (static_cast<char32_t>(b1 & kContinuationMask) << kUtf8Shift12) |
               (static_cast<char32_t>(b2 & kContinuationMask) << kUtf8Shift6) |
               static_cast<char32_t>(b3 & kContinuationMask);
    }

    ++index;
    return kUnicodeReplacementChar;
}

std::size_t utf8_codepoint_count(std::string_view text) noexcept {
    std::size_t count = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        utf8_next(text, index);
        ++count;
    }
    return count;
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp > kMaxUnicodeCodepoint ||
        (cp >= kSurrogateHighFirst && cp <= kSurrogateLowLast)) {
        cp = kUnicodeReplacementChar;
    }

    if (cp <= kAsciiMaxByte) {
        out.push_back(static_cast<char>(cp));
        return;
    }
    if (cp <= kTwoByteMaxCp) {
        out.push_back(static_cast<char>(kTwoByteHeaderBits |
                                        static_cast<std::uint8_t>(cp >> kUtf8Shift6)));
        out.push_back(static_cast<char>(
            kContinuationHeaderBits | static_cast<std::uint8_t>(cp & kContinuationMask)));
        return;
    }
    if (cp <= kThreeByteMaxCp) {
        out.push_back(static_cast<char>(
            kThreeByteHeaderBits | static_cast<std::uint8_t>(cp >> kUtf8Shift12)));
        out.push_back(static_cast<char>(
            kContinuationHeaderBits |
            static_cast<std::uint8_t>((cp >> kUtf8Shift6) & kContinuationMask)));
        out.push_back(static_cast<char>(
            kContinuationHeaderBits | static_cast<std::uint8_t>(cp & kContinuationMask)));
        return;
    }

    out.push_back(static_cast<char>(kFourByteHeaderBits |
                                    static_cast<std::uint8_t>(cp >> kUtf8Shift18)));
    out.push_back(static_cast<char>(
        kContinuationHeaderBits |
        static_cast<std::uint8_t>((cp >> kUtf8Shift12) & kContinuationMask)));
    out.push_back(static_cast<char>(
        kContinuationHeaderBits |
        static_cast<std::uint8_t>((cp >> kUtf8Shift6) & kContinuationMask)));
    out.push_back(static_cast<char>(
        kContinuationHeaderBits | static_cast<std::uint8_t>(cp & kContinuationMask)));
}

std::string wide_to_utf8(std::wstring_view wide) {
    std::string out;
    out.reserve(wide.size());
    std::size_t i = 0;
    while (i < wide.size()) {
        const auto unit = static_cast<char32_t>(static_cast<std::uint16_t>(wide[i]));
        if (unit >= kSurrogateHighFirst && unit <= kSurrogateHighLast) {
            if (i + 1U < wide.size()) {
                const auto low =
                    static_cast<char32_t>(static_cast<std::uint16_t>(wide[i + 1U]));
                if (low >= kSurrogateLowFirst && low <= kSurrogateLowLast) {
                    const char32_t scalar =
                        kAstralPlaneBase +
                        (((unit - kSurrogateHighFirst) << kUtf16SurrogateShift) |
                         (low - kSurrogateLowFirst));
                    append_utf8(out, scalar);
                    i += 2U;
                    continue;
                }
            }
            append_utf8(out, kUnicodeReplacementChar);
            ++i;
            continue;
        }
        if (unit >= kSurrogateLowFirst && unit <= kSurrogateLowLast) {
            append_utf8(out, kUnicodeReplacementChar);
            ++i;
            continue;
        }
        append_utf8(out, unit);
        ++i;
    }
    return out;
}

std::wstring utf8_to_wide(std::string_view utf8) {
    std::wstring out;
    out.reserve(utf8.size());
    std::size_t index = 0;
    while (index < utf8.size()) {
        const char32_t cp = utf8_next(utf8, index);
        if (cp <= kThreeByteMaxCp) {
            out.push_back(static_cast<wchar_t>(cp));
            continue;
        }
        const char32_t offset = cp - kAstralPlaneBase;
        out.push_back(
            static_cast<wchar_t>(kSurrogateHighFirst + (offset >> kUtf16SurrogateShift)));
        out.push_back(
            static_cast<wchar_t>(kSurrogateLowFirst + (offset & kUtf16LowTenBitsMask)));
    }
    return out;
}

bool is_hiragana(char32_t cp) noexcept {
    return cp >= kHiraganaFirst && cp <= kHiraganaLast;
}

bool is_katakana(char32_t cp) noexcept {
    return (cp >= kKatakanaFirst && cp <= kKatakanaLast) ||
           (cp >= kKatakanaPhoneticExtFirst && cp <= kKatakanaPhoneticExtLast) ||
           (cp >= kHalfwidthKatakanaFirst && cp <= kHalfwidthKatakanaLast);
}

bool is_lexical_katakana(char32_t cp) noexcept {
    if (!is_katakana(cp)) {
        return false;
    }
    return cp != U'゠' && cp != U'・' && cp != U'ー' && cp != U'ｰ';
}

bool is_kanji(char32_t cp) noexcept {
    return cp == U'々' ||
           (cp >= kCjkUnifiedFirst && cp <= kCjkUnifiedLast) ||
           (cp >= kCjkExtAFirst && cp <= kCjkExtALast) ||
           (cp >= kCjkExtBFirst && cp <= kCjkExtBLast) ||
           (cp >= kCjkCompatIdeographsFirst && cp <= kCjkCompatIdeographsLast);
}

bool has_japanese(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const char32_t cp = utf8_next(text, index);
        if (is_hiragana(cp) || is_katakana(cp) || is_kanji(cp)) {
            return true;
        }
        if ((cp >= kCjkSymbolsPunctFirst && cp <= kCjkSymbolsPunctLast) ||
            (cp >= kFullwidthFormsFirst && cp <= kHalfwidthKatakanaLast)) {
            return true;
        }
    }
    return false;
}

bool has_real_japanese(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const char32_t cp = utf8_next(text, index);
        if (is_hiragana(cp) || is_lexical_katakana(cp) || is_kanji(cp)) {
            return true;
        }
    }
    return false;
}

bool is_pure_silence_or_punctuation(std::string_view text) noexcept {
    if (has_real_japanese(text)) {
        return false;
    }
    for (char c : text) {
        const auto b = static_cast<unsigned char>(c);
        if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9')) {
            return false;
        }
    }
    return true;
}

bool has_cjk_or_fullwidth_punctuation(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const char32_t cp = utf8_next(text, index);
        if (cp >= kCjkSymbolsPunctFirst && cp <= kCjkSymbolsPunctLast && cp != U'々') {
            return true;
        }
        if (cp >= kFullwidthPunctAndHalfwidthFirst && cp <= kHalfwidthCjkPunctLast) {
            return true;
        }
        if (cp == kEnDash || cp == kEmDash || cp == kHorizontalBar ||
            cp == kLeftSingleQuote || cp == kRightSingleQuote ||
            cp == kLeftDoubleQuote || cp == kRightDoubleQuote ||
            cp == kTwoDotLeader || cp == kHorizontalEllipsis ||
            cp == kBoxDrawingsLightHorizontal || cp == U'・' || cp == U'゠') {
            return true;
        }
    }
    return false;
}

std::string_view trim_ascii_view(std::string_view text) noexcept {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_ascii_whitespace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin &&
           is_ascii_whitespace(static_cast<unsigned char>(text[end - 1U]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string trim_ascii(std::string_view text) {
    return std::string(trim_ascii_view(text));
}

std::string trim_left_ascii(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           is_ascii_whitespace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    return std::string(text.substr(begin));
}

std::string trim_right_ascii(std::string_view text) {
    std::size_t end = text.size();
    while (end > 0 && is_ascii_whitespace(static_cast<unsigned char>(text[end - 1U]))) {
        --end;
    }
    return std::string(text.substr(0, end));
}

std::string trim_unicode(std::string_view text) {
    std::size_t index = 0;
    std::size_t first_non_ws = 0;
    std::size_t after_last_non_ws = 0;
    bool found_non_ws = false;

    while (index < text.size()) {
        const std::size_t cp_start = index;
        const char32_t cp = utf8_next(text, index);
        if (!is_unicode_trim_cp(cp)) {
            if (!found_non_ws) {
                first_non_ws = cp_start;
                found_non_ws = true;
            }
            after_last_non_ws = index;
        }
    }
    if (!found_non_ws) {
        return {};
    }
    return std::string(text.substr(first_non_ws, after_last_non_ws - first_non_ws));
}

std::string to_lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(to_lower_ascii_byte(static_cast<unsigned char>(c)));
    }
    return out;
}

bool is_word_byte(unsigned char b) noexcept {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
           (b >= '0' && b <= '9') || b >= kFirstNonAsciiByte;
}

bool contains_word_ascii_ci(std::string_view haystack,
                            std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > haystack.size()) {
        return false;
    }
    const std::size_t limit = haystack.size() - needle.size();
    for (std::size_t pos = 0; pos <= limit; ++pos) {
        bool matched = true;
        for (std::size_t k = 0; k < needle.size(); ++k) {
            const auto hb = to_lower_ascii_byte(static_cast<unsigned char>(haystack[pos + k]));
            const auto nb = to_lower_ascii_byte(static_cast<unsigned char>(needle[k]));
            if (hb != nb) {
                matched = false;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        const bool left_ok =
            (pos == 0) || !is_word_byte(static_cast<unsigned char>(haystack[pos - 1U]));
        const std::size_t end = pos + needle.size();
        const bool right_ok =
            (end == haystack.size()) ||
            !is_word_byte(static_cast<unsigned char>(haystack[end]));
        if (left_ok && right_ok) {
            return true;
        }
    }
    return false;
}

std::string sanitize_ascii_punctuation(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const std::size_t cp_start = index;
        const char32_t cp = utf8_next(text, index);
        if (cp == kEmDash || cp == kEnDash || cp == kHorizontalBar ||
            cp == kBoxDrawingsLightHorizontal) {
            out += "--";
            continue;
        }
        if (cp == kHorizontalEllipsis || cp == kTwoDotLeader) {
            out += "...";
            continue;
        }
        if (cp == kLeftSingleQuote || cp == kRightSingleQuote) {
            out.push_back('\'');
            continue;
        }
        if (cp == kLeftDoubleQuote || cp == kRightDoubleQuote) {
            out.push_back('"');
            continue;
        }
        if (cp == kWaveDash || cp == kFullwidthTilde) {
            out.push_back('~');
            continue;
        }
        if (cp == kIdeographicSpace) {
            out.push_back(' ');
            continue;
        }
        out.append(text.substr(cp_start, index - cp_start));
    }
    return out;
}

}  // namespace spot_checker
