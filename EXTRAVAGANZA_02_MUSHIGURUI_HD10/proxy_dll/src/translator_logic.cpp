// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#include <windows.h>

#include <array>
#include <cstring>
#include <utility>

namespace translator_logic {

namespace {

constexpr UINT kShiftJisCodePage = 932u;

constexpr std::size_t kUtf8BomByteLength = 3;
constexpr std::array<unsigned char, kUtf8BomByteLength> kUtf8BomBytes = {
    0xEF, 0xBB, 0xBF,
};

constexpr unsigned char kCp932Lead1Min = 0x81;
constexpr unsigned char kCp932Lead1Max = 0x9F;
constexpr unsigned char kCp932Lead2Min = 0xE0;
constexpr unsigned char kCp932Lead2Max = 0xFC;
constexpr unsigned char kCp932Trail1Min = 0x40;
constexpr unsigned char kCp932Trail1Max = 0x7E;
constexpr unsigned char kCp932Trail2Min = 0x80;
constexpr unsigned char kCp932Trail2Max = 0xFC;

constexpr std::size_t kCp932DoubleByteLength = 2;
constexpr std::size_t kEscapeSequenceLength = 2;
constexpr std::size_t kCp932ColorTagByteLength = 3;
constexpr std::size_t kCp932ColorTagLeadOffset = 1;
constexpr std::size_t kCp932ColorTagTrailOffset = 2;
constexpr std::size_t kMinAsciiLettersForWords = 2;

constexpr unsigned char kNonAsciiMinByte = 0x80;
constexpr std::uint8_t kFxfXorMask = 0xFF;
constexpr char kWindowTitleKey[] = "windowTitle";
constexpr std::size_t kWindowTitleKeyLen = sizeof(kWindowTitleKey) - 1;
constexpr std::string_view kHd10EditionToken = "HD10";
constexpr const char* kDefaultEnglishTitleBase = "EXTRAVAGANZA ~Mushigurui~";
constexpr const char* kDefaultEnglishTitleHd10 = "EXTRAVAGANZA ~Mushigurui~ HD10";

// 18 Music Gallery runtime strings (17 tracks in nya\listenbgmlist.xtx +
// cannotMusicName in init2\listenBGM.fxf) and 4 Config Page 2 live preview
// strings (init2\config.fxf). Note that entry 14 uses ideographic space U+3000
// and fullwidth tilde U+FF5E, and entry 17 uses single-byte ASCII 'p' matching
// nya\listenbgmlist.xtx verbatim.
constexpr std::array<RuntimeUiFallbackEntry, kRuntimeUiFallbackCount>
    kRuntimeUiFallbacks = {{
        {L"青き蟲独", "Aoki Mushidoku"},
        {L"真鈴ヶ崎", "Masuzugasaki"},
        {L"少女達の昼下がり", "Girls' Afternoon"},
        {L"カフェ・ＡｎｚＵ", "Cafe AnzU"},
        {L"家族団欒", "Family Gathering"},
        {L"蟲使いの森", "Forest of the Bug User"},
        {L"薄暗い倉庫", "Dimly Lit Warehouse"},
        {L"Ｄｒ．Ｗｅｓｔの研究室", "Dr. West's Laboratory"},
        {L"我が子との絆", "Bond with My Child"},
        {L"穢れた欲望", "Defiled Desire"},
        {L"牙と爪の乱舞", "Wild Dance of Fang and Claw"},
        {L"別れのとき", "Time to Part"},
        {L"人と蟲の先に在る生物", "Beyond Human and Bug"},
        {L"Ｃｏｃｙｔｕｓ　～Ｏｒｇｅｌ～", "Cocytus ~Orgel~"},
        {L"その十五年に決着を", "Settling Fifteen Years"},
        {L"美しい夢", "Beautiful Dream"},
        {L"ｍｙpｒｅｃｉｏｕｓ", "my precious"},
        {L"？？？", "???"},
        {L"ウィンドウの透明度を変更できます。", "You can adjust window opacity."},
        {L"このメッセージウィンドウをクリックすることで、", "Click this message window"},
        {L"カラーを変更できます。", "to change its color."},
        {L"キャラ名表示エリア", "Character Name"},
    }};

bool IsCp932LeadByte(unsigned char c) noexcept {
    return (c >= kCp932Lead1Min && c <= kCp932Lead1Max) ||
           (c >= kCp932Lead2Min && c <= kCp932Lead2Max);
}

bool IsCp932TrailByte(unsigned char c) noexcept {
    return (c >= kCp932Trail1Min && c <= kCp932Trail1Max) ||
           (c >= kCp932Trail2Min && c <= kCp932Trail2Max);
}

bool IsAsciiAlpha(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool ContainsNonAsciiByte(std::string_view s) noexcept {
    for (char ch : s) {
        if (static_cast<unsigned char>(ch) >= kNonAsciiMinByte) {
            return true;
        }
    }
    return false;
}

std::size_t LeadingMarkdownHashPrefixLen(const std::string& s,
                                         std::size_t pos) noexcept {
    std::size_t cur = pos;
    while (cur < s.size() && s[cur] == '#') {
        if (cur + kCp932ColorTagByteLength <= s.size() &&
            HasCp932ColorPrefix(s.c_str() + cur)) {
            return 0;
        }
        ++cur;
    }
    if (cur > pos && cur < s.size() && s[cur] == ' ') {
        return (cur - pos) + 1;
    }
    return 0;
}

const char* SkipControlPrefixes(const char* s) noexcept {
    if (s == nullptr) {
        return nullptr;
    }
    if (s[0] == '%') {
        ++s;
    }
    if (HasCp932ColorPrefix(s)) {
        s += kCp932ColorTagByteLength;
    }
    return s;
}

template <typename FindFn>
bool LookupWithFinder(FindFn find_candidate,
                      const char* originalText,
                      bool singleLineMode,
                      std::string& outTranslated) {
    if (originalText == nullptr || originalText[0] == '\0') {
        return false;
    }

    const bool origStartsWithPercent = (originalText[0] == '%');
    const char* afterPercent =
        origStartsWithPercent ? (originalText + 1) : originalText;
    const bool hasColorPrefix = HasCp932ColorPrefix(afterPercent);
    const std::string colorPrefix =
        hasColorPrefix ? std::string(afterPercent, kCp932ColorTagByteLength)
                       : std::string();

    const std::string* matched = find_candidate(std::string_view(originalText));
    if (matched == nullptr && !origStartsWithPercent) {
        std::string withPercent = "%";
        withPercent.append(originalText);
        matched = find_candidate(std::string_view(withPercent));
    }
    if (matched == nullptr && hasColorPrefix) {
        const char* strippedColor = afterPercent + kCp932ColorTagByteLength;
        if (strippedColor[0] != '\0') {
            matched = find_candidate(std::string_view(strippedColor));
            if (matched == nullptr) {
                std::string withPercentStripped = "%";
                withPercentStripped.append(strippedColor);
                matched = find_candidate(std::string_view(withPercentStripped));
            }
        }
    }

    if (matched == nullptr) {
        return false;
    }

    std::string result = *matched;
    if (!origStartsWithPercent && !result.empty() && result[0] == '%') {
        result.erase(0, 1);
    }

    if (hasColorPrefix) {
        const std::size_t insertPos =
            (!result.empty() && result[0] == '%') ? 1u : 0u;
        if (!HasCp932ColorPrefix(result.c_str() + insertPos)) {
            result.insert(insertPos, colorPrefix);
        }
    }

    SanitizeAsciiHashInPlace(result);

    if (singleLineMode) {
        FlattenNewlinesInPlace(result);
    }

    outTranslated = std::move(result);
    return true;
}

template <typename OnRowFn>
int ParseTsvRows(const char* data, std::size_t len, OnRowFn on_row) {
    if (data == nullptr || len == 0) {
        return 0;
    }

    int count = 0;
    std::size_t i = 0;
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
        std::string en(data + tab + 1, line_end - (tab + 1));
        UnescapeInPlace(jp);
        UnescapeInPlace(en);
        if (jp.empty() || en.empty()) {
            continue;
        }
        on_row(std::move(jp), std::move(en));
        ++count;
    }
    return count;
}

}  // namespace

const std::array<RuntimeUiFallbackEntry, kRuntimeUiFallbackCount>&
GetRuntimeUiFallbackEntries() noexcept {
    return kRuntimeUiFallbacks;
}

bool EncodeWideToCp932(std::wstring_view wide_text, std::string& out_cp932) {
    out_cp932.clear();
    if (wide_text.empty()) {
        return true;
    }

    BOOL used_default_char = FALSE;
    const int needed = WideCharToMultiByte(
        kShiftJisCodePage,
        WC_NO_BEST_FIT_CHARS,
        wide_text.data(),
        static_cast<int>(wide_text.size()),
        nullptr,
        0,
        nullptr,
        &used_default_char);
    if (needed <= 0 || used_default_char != FALSE) {
        return false;
    }

    std::string encoded(static_cast<std::size_t>(needed), '\0');
    used_default_char = FALSE;
    const int written = WideCharToMultiByte(
        kShiftJisCodePage,
        WC_NO_BEST_FIT_CHARS,
        wide_text.data(),
        static_cast<int>(wide_text.size()),
        encoded.data(),
        needed,
        nullptr,
        &used_default_char);
    if (written != needed || used_default_char != FALSE) {
        return false;
    }

    out_cp932 = std::move(encoded);
    return true;
}

void UnescapeInPlace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto ch = static_cast<unsigned char>(s[i]);
        if (IsCp932LeadByte(ch) &&
            i + kCp932DoubleByteLength <= s.size() &&
            IsCp932TrailByte(static_cast<unsigned char>(s[i + 1]))) {
            out.append(s, i, kCp932DoubleByteLength);
            i += kCp932DoubleByteLength;
            continue;
        }
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out.push_back(s[i]);
            ++i;
            continue;
        }
        switch (s[i + 1]) {
        case 'r':
            out.push_back('\r');
            i += kEscapeSequenceLength;
            break;
        case 'n':
            out.push_back('\n');
            i += kEscapeSequenceLength;
            break;
        case 't':
            out.push_back('\t');
            i += kEscapeSequenceLength;
            break;
        case '\\':
            out.push_back('\\');
            i += kEscapeSequenceLength;
            break;
        default:
            out.push_back(s[i]);
            ++i;
            break;
        }
    }
    s = std::move(out);
}

std::size_t Utf8BomLen(const char* data, std::size_t len) {
    if (data != nullptr && len >= kUtf8BomBytes.size() &&
        std::memcmp(data, kUtf8BomBytes.data(), kUtf8BomBytes.size()) == 0) {
        return kUtf8BomBytes.size();
    }
    return 0;
}

int ParseTsvBuffer(const char* data, std::size_t len, TranslationMap& out) {
    return ParseTsvRows(data, len, [&out](std::string jp, std::string en) {
        out[std::move(jp)] = std::move(en);
    });
}

std::string StripTrailingCrLf(std::string_view s) {
    std::size_t end = s.size();
    while (end > 0 && (s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return std::string(s.substr(0, end));
}

void FlattenNewlinesInPlace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (!out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
            continue;
        }
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    s.swap(out);
}

bool HasCp932ColorPrefix(const char* s) noexcept {
    if (s == nullptr || s[0] != '#') {
        return false;
    }
    const auto lead = static_cast<unsigned char>(s[kCp932ColorTagLeadOffset]);
    if (!IsCp932LeadByte(lead)) {
        return false;
    }
    const auto trail = static_cast<unsigned char>(s[kCp932ColorTagTrailOffset]);
    return IsCp932TrailByte(trail);
}

void SanitizeAsciiHashInPlace(std::string& s) {
    if (s.find('#') == std::string::npos) {
        return;
    }

    std::string fullwidth_hash;
    if (!EncodeWideToCp932(L"＃", fullwidth_hash) || fullwidth_hash.empty()) {
        return;
    }

    std::string out;
    out.reserve(s.size() + fullwidth_hash.size());

    bool at_line_prefix = true;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto ch = static_cast<unsigned char>(s[i]);

        if (IsCp932LeadByte(ch) &&
            i + kCp932DoubleByteLength <= s.size() &&
            IsCp932TrailByte(static_cast<unsigned char>(s[i + 1]))) {
            out.append(s, i, kCp932DoubleByteLength);
            i += kCp932DoubleByteLength;
            at_line_prefix = false;
            continue;
        }

        if (i == 0 && s[i] == '%') {
            out.push_back('%');
            ++i;
            continue;
        }

        if (s[i] == '\r' || s[i] == '\n') {
            out.push_back(s[i]);
            at_line_prefix = true;
            ++i;
            continue;
        }

        if (s[i] == '#') {
            if (i + kCp932ColorTagByteLength <= s.size() &&
                HasCp932ColorPrefix(s.c_str() + i)) {
                out.append(s, i, kCp932ColorTagByteLength);
                i += kCp932ColorTagByteLength;
                continue;
            }
            if (at_line_prefix) {
                const std::size_t md_prefix_len =
                    LeadingMarkdownHashPrefixLen(s, i);
                if (md_prefix_len > 0) {
                    i += md_prefix_len;
                    continue;
                }
            }
            out.append(fullwidth_hash);
            ++i;
            at_line_prefix = false;
            continue;
        }

        out.push_back(s[i]);
        ++i;
        at_line_prefix = false;
    }

    s.swap(out);
}

bool LookupTranslation(const TranslationMap& map,
                       const char* originalText,
                       bool singleLineMode,
                       std::string& outTranslated) {
    if (map.empty()) {
        return false;
    }
    return LookupWithFinder(
        [&map](std::string_view key) -> const std::string* {
            const auto it = map.find(std::string(key));
            if (it == map.end()) {
                return nullptr;
            }
            return &it->second;
        },
        originalText,
        singleLineMode,
        outTranslated);
}

bool HasAsciiWords(const char* text) noexcept {
    const char* p = SkipControlPrefixes(text);
    if (p == nullptr || p[0] == '\0') {
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

std::size_t VisibleAsciiLen(const char* text) noexcept {
    const char* p = SkipControlPrefixes(text);
    if (p == nullptr) {
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
                                         int curMaxChars) noexcept {
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
    adj.newFontSize = (visLen > kMediumChoiceCharLimit)
                          ? kCompactChoiceFontSize
                          : kStandardChoiceFontSize;
    adj.newSpace = kPatchedChoiceSpace;
    adj.yOffset = (curFontSize - adj.newFontSize) / kVerticalCenteringDivisor;
    adj.expandMaxChars = (curMaxChars > 0 && curMaxChars <= curFontSize);
    return adj;
}

std::string ExtractFxfWindowTitle(const std::uint8_t* fxfBytes, std::size_t len) {
    if (fxfBytes == nullptr || len == 0) {
        return {};
    }
    const bool isXorEncoded = (fxfBytes[0] >= kNonAsciiMinByte);
    std::string decoded(len, '\0');
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint8_t b = isXorEncoded
                                   ? static_cast<std::uint8_t>(fxfBytes[i] ^ kFxfXorMask)
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
        while (valStart < lineEnd &&
               (decoded[valStart] == ' ' || decoded[valStart] == '\t')) {
            ++valStart;
        }
        while (lineEnd > valStart &&
               (decoded[lineEnd - 1] == ' ' || decoded[lineEnd - 1] == '\t')) {
            --lineEnd;
        }
        if (lineEnd > valStart) {
            const std::string_view raw_title(
                decoded.data() + valStart, lineEnd - valStart);
            if (ContainsNonAsciiByte(raw_title)) {
                if (raw_title.find(kHd10EditionToken) != std::string_view::npos) {
                    return std::string(kDefaultEnglishTitleHd10);
                }
                return std::string(kDefaultEnglishTitleBase);
            }
            return std::string(raw_title);
        }
    }
    return {};
}

void TranslationTable::Clear() noexcept {
    exact_.clear();
    stripped_.clear();
}

void TranslationTable::InsertOrAssign(std::string jp, std::string en) {
    if (jp.empty() || en.empty()) {
        return;
    }
    std::string stripped_key = StripTrailingCrLf(jp);
    if (!stripped_key.empty()) {
        stripped_[std::move(stripped_key)] = en;
    }
    exact_[std::move(jp)] = std::move(en);
}

int TranslationTable::LoadFromTsvBuffer(const char* data, std::size_t len) {
    if (data == nullptr || len == 0) {
        return 0;
    }
    const std::size_t bom_len = Utf8BomLen(data, len);
    return ParseTsvRows(
        data + bom_len,
        len - bom_len,
        [this](std::string jp, std::string en) {
            InsertOrAssign(std::move(jp), std::move(en));
        });
}

std::size_t TranslationTable::SeedRuntimeUiFallbacks() {
    std::size_t inserted = 0;
    for (const RuntimeUiFallbackEntry& entry : kRuntimeUiFallbacks) {
        std::string jp;
        if (!EncodeWideToCp932(entry.jp_text, jp) || jp.empty()) {
            continue;
        }
        std::string en(entry.en_text);
        std::string stripped_key = StripTrailingCrLf(jp);
        if (!stripped_key.empty()) {
            stripped_.try_emplace(std::move(stripped_key), en);
        }
        const auto [it, did_insert] =
            exact_.try_emplace(std::move(jp), std::move(en));
        (void)it;
        if (did_insert) {
            ++inserted;
        }
    }
    return inserted;
}

const std::string* TranslationTable::FindCandidate(std::string_view key) const {
    if (key.empty()) {
        return nullptr;
    }
    const std::string exact_key(key);
    const auto exact_it = exact_.find(exact_key);
    if (exact_it != exact_.end()) {
        return &exact_it->second;
    }
    const std::string stripped_key = StripTrailingCrLf(key);
    if (stripped_key.empty()) {
        return nullptr;
    }
    const auto stripped_it = stripped_.find(stripped_key);
    if (stripped_it != stripped_.end()) {
        return &stripped_it->second;
    }
    return nullptr;
}

bool TranslationTable::Lookup(const char* originalText,
                              bool singleLineMode,
                              std::string& outTranslated) const {
    if (Empty()) {
        return false;
    }
    return LookupWithFinder(
        [this](std::string_view key) { return FindCandidate(key); },
        originalText,
        singleLineMode,
        outTranslated);
}

}  // namespace translator_logic
