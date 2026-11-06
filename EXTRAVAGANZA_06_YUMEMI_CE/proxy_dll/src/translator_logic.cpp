// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

#include "translator_logic.h"

namespace translator_logic {

namespace {

constexpr std::size_t kUtf8BomByteLen = 3;
constexpr unsigned char kUtf8BomByte0 = 0xEF;
constexpr unsigned char kUtf8BomByte1 = 0xBB;
constexpr unsigned char kUtf8BomByte2 = 0xBF;

}  // namespace

std::size_t Utf8BomLen(const char* buf, std::size_t len) {
    if (buf && len >= kUtf8BomByteLen &&
        static_cast<unsigned char>(buf[0]) == kUtf8BomByte0 &&
        static_cast<unsigned char>(buf[1]) == kUtf8BomByte1 &&
        static_cast<unsigned char>(buf[2]) == kUtf8BomByte2) {
        return kUtf8BomByteLen;
    }
    return 0;
}

void UnescapeInPlace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        switch (s[i + 1]) {
        case 'r':
            out += '\r';
            ++i;
            break;
        case 'n':
            out += '\n';
            ++i;
            break;
        case 't':
            out += '\t';
            ++i;
            break;
        case '\\':
            out += '\\';
            ++i;
            break;
        default:
            out += s[i];
            break;
        }
    }
    s.swap(out);
}

int ParseTsvBuffer(const char* buf, std::size_t len, TranslationMap& out) {
    if (!buf || len == 0) return 0;

    int count = 0;
    std::size_t lineStart = 0;

    for (std::size_t i = 0; i <= len; ++i) {
        if (i != len && buf[i] != '\n') continue;

        std::size_t lineEnd = i;
        if (lineEnd > lineStart && buf[lineEnd - 1] == '\r') {
            --lineEnd;
        }

        const std::size_t curStart = lineStart;
        lineStart = i + 1;
        if (lineEnd <= curStart) continue;

        std::size_t tabPos = std::string::npos;
        for (std::size_t j = curStart; j < lineEnd; ++j) {
            if (buf[j] == '\t') {
                tabPos = j;
                break;
            }
        }
        if (tabPos == std::string::npos || tabPos <= curStart || tabPos + 1 >= lineEnd) {
            continue;
        }

        std::string jp(buf + curStart, tabPos - curStart);
        std::string en(buf + tabPos + 1, lineEnd - (tabPos + 1));

        UnescapeInPlace(jp);
        UnescapeInPlace(en);

        out[std::move(jp)] = std::move(en);
        ++count;
    }

    return count;
}

namespace {

constexpr unsigned char kCp932Lead1Min = 0x81;
constexpr unsigned char kCp932Lead1Max = 0x9F;
constexpr unsigned char kCp932Lead2Min = 0xE0;
constexpr unsigned char kCp932Lead2Max = 0xFC;
constexpr std::size_t kColorPrefixByteLen = 3;
constexpr std::size_t kMinAsciiLettersForWords = 2;
constexpr unsigned char kFxfEncryptedHighBit = 0x80;
constexpr std::uint8_t kFxfXorMask = 0xFF;
constexpr char kWindowTitleKey[] = "windowTitle";
constexpr std::size_t kWindowTitleKeyLen = sizeof(kWindowTitleKey) - 1;

bool IsCp932LeadByte(unsigned char c) noexcept {
    return (c >= kCp932Lead1Min && c <= kCp932Lead1Max) ||
           (c >= kCp932Lead2Min && c <= kCp932Lead2Max);
}

bool HasCp932ColorPrefix(const char* s) noexcept {
    if (!s || s[0] != '#') {
        return false;
    }
    const auto lead = static_cast<unsigned char>(s[1]);
    const auto trail = static_cast<unsigned char>(s[2]);
    return IsCp932LeadByte(lead) && trail != 0;
}

const char* SkipControlPrefixes(const char* s) noexcept {
    if (!s) {
        return nullptr;
    }
    if (s[0] == '%') {
        ++s;
    }
    if (HasCp932ColorPrefix(s)) {
        s += kColorPrefixByteLen;
    }
    return s;
}

bool IsAsciiAlpha(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

}  // namespace

void FlattenNewlinesInPlace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            continue;
        }
        if (s[i] == '\n') {
            if (!out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
            continue;
        }
        out.push_back(s[i]);
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    s.swap(out);
}

bool LookupTranslation(const TranslationMap& map,
                       const char* originalText,
                       bool singleLineMode,
                       std::string& outTranslated) {
    if (!originalText || !originalText[0] || map.empty()) {
        return false;
    }

    const bool origStartsWithPercent = (originalText[0] == '%');
    const char* afterPercent = origStartsWithPercent ? (originalText + 1) : originalText;
    const bool hasColorPrefix = HasCp932ColorPrefix(afterPercent);
    const std::string colorPrefix =
        hasColorPrefix ? std::string(afterPercent, kColorPrefixByteLen) : std::string();

    auto it = map.find(originalText);
    if (it == map.end() && !origStartsWithPercent) {
        std::string withPercent = "%";
        withPercent.append(originalText);
        it = map.find(withPercent);
    }
    if (it == map.end() && hasColorPrefix) {
        const char* strippedColor = afterPercent + kColorPrefixByteLen;
        if (strippedColor[0] != '\0') {
            it = map.find(strippedColor);
            if (it == map.end()) {
                std::string withPercentStripped = "%";
                withPercentStripped.append(strippedColor);
                it = map.find(withPercentStripped);
            }
        }
    }

    if (it == map.end()) {
        return false;
    }

    std::string result = it->second;
    if (!origStartsWithPercent && !result.empty() && result[0] == '%') {
        result.erase(0, 1);
    }
    if (hasColorPrefix) {
        const std::size_t insertPos = (!result.empty() && result[0] == '%') ? 1u : 0u;
        if (result.size() <= insertPos || result[insertPos] != '#') {
            result.insert(insertPos, colorPrefix);
        }
    }
    if (singleLineMode) {
        FlattenNewlinesInPlace(result);
    }

    outTranslated = std::move(result);
    return true;
}

bool HasAsciiWords(const char* text) {
    const char* p = SkipControlPrefixes(text);
    if (!p || !p[0]) {
        return false;
    }
    std::size_t alphaCount = 0;
    for (std::size_t i = 0; p[i] != '\0'; ++i) {
        const auto c = static_cast<unsigned char>(p[i]);
        if (IsCp932LeadByte(c)) {
            return false;
        }
        if (IsAsciiAlpha(c)) {
            ++alphaCount;
        }
    }
    return alphaCount >= kMinAsciiLettersForWords;
}

std::size_t VisibleAsciiLen(const char* text) {
    const char* p = SkipControlPrefixes(text);
    if (!p) {
        return 0;
    }
    std::size_t len = 0;
    while (p[len] != '\0') {
        ++len;
    }
    return len;
}

UiFontAdjustment ComputeUiFontAdjustment(const char* text,
                                         bool wasTranslated,
                                         int curFontSize,
                                         int curSpace,
                                         int curMaxChars) {
    UiFontAdjustment adj{};
    adj.newFontSize = curFontSize;
    adj.newSpace = curSpace;

    if (curSpace <= 0 || curFontSize < kMinUiUnpatchedFontSize) {
        return adj;
    }
    if (!wasTranslated && !HasAsciiWords(text)) {
        return adj;
    }

    const std::size_t visLen = VisibleAsciiLen(text);
    adj.shouldAdjust = true;
    adj.newFontSize =
        (visLen > kMediumChoiceCharLimit) ? kCompactChoiceFontSize : kStandardChoiceFontSize;
    adj.newSpace = kPatchedChoiceSpace;
    adj.yOffset = (curFontSize - adj.newFontSize) / 2;
    adj.expandMaxChars = (curMaxChars > 0 && curMaxChars <= curFontSize);
    return adj;
}

std::string ExtractFxfWindowTitle(const std::uint8_t* fxfBytes, std::size_t len) {
    if (!fxfBytes || len == 0) {
        return {};
    }
    const bool isXorEncoded = (fxfBytes[0] >= kFxfEncryptedHighBit);
    std::string decoded(len, '\0');
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint8_t b = isXorEncoded ? static_cast<std::uint8_t>(fxfBytes[i] ^ kFxfXorMask)
                                            : fxfBytes[i];
        decoded[i] = static_cast<char>(b);
    }

    std::size_t lineStart = 0;
    for (std::size_t i = 0; i <= len; ++i) {
        if (i != len && decoded[i] != '\n') {
            continue;
        }
        std::size_t lineEnd = i;
        if (lineEnd > lineStart && decoded[lineEnd - 1] == '\r') {
            --lineEnd;
        }
        std::size_t cur = lineStart;
        lineStart = i + 1;

        while (cur < lineEnd && (decoded[cur] == ' ' || decoded[cur] == '\t')) {
            ++cur;
        }
        if (lineEnd - cur <= kWindowTitleKeyLen) {
            continue;
        }
        if (decoded.compare(cur, kWindowTitleKeyLen, kWindowTitleKey) != 0) {
            continue;
        }
        const char sep = decoded[cur + kWindowTitleKeyLen];
        if (sep != ' ' && sep != '\t') {
            continue;
        }
        std::size_t valStart = cur + kWindowTitleKeyLen + 1;
        while (valStart < lineEnd && (decoded[valStart] == ' ' || decoded[valStart] == '\t')) {
            ++valStart;
        }
        while (lineEnd > valStart &&
               (decoded[lineEnd - 1] == ' ' || decoded[lineEnd - 1] == '\t')) {
            --lineEnd;
        }
        if (lineEnd > valStart) {
            return decoded.substr(valStart, lineEnd - valStart);
        }
    }
    return {};
}

}  // namespace translator_logic
