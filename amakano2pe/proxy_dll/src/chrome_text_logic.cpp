// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "chrome_text_logic.h"

#include <array>

namespace chrome_text {
namespace {

// "アマカノ2～Perfect Edition～" with U+FF5E FULLWIDTH TILDE.
constexpr std::wstring_view kJapaneseTitleFullwidthTilde =
    L"アマカノ2\uFF5EPerfect Edition\uFF5E";

// "アマカノ2〜Perfect Edition〜" with U+301C WAVE DASH.
constexpr std::wstring_view kJapaneseTitleWaveDash =
    L"アマカノ2\u301CPerfect Edition\u301C";

// "アマカノ２～Perfect Edition～" with U+FF12 FULLWIDTH DIGIT TWO + U+FF5E.
constexpr std::wstring_view kJapaneseTitleFullwidthDigitTilde =
    L"アマカノ\uFF12\uFF5EPerfect Edition\uFF5E";

// "アマカノ２〜Perfect Edition〜" with U+FF12 FULLWIDTH DIGIT TWO + U+301C.
constexpr std::wstring_view kJapaneseTitleFullwidthDigitWaveDash =
    L"アマカノ\uFF12\u301CPerfect Edition\u301C";

constexpr std::array kJapaneseTitleSpellings{
    kJapaneseTitleFullwidthTilde,
    kJapaneseTitleWaveDash,
    kJapaneseTitleFullwidthDigitTilde,
    kJapaneseTitleFullwidthDigitWaveDash,
};

// UTF-8 bytes of kJapaneseTitleFullwidthTilde.
constexpr std::string_view kJapaneseTitleFullwidthTildeUtf8 =
    "アマカノ2\uFF5EPerfect Edition\uFF5E";

// UTF-8 bytes of kJapaneseTitleWaveDash.
constexpr std::string_view kJapaneseTitleWaveDashUtf8 =
    "アマカノ2\u301CPerfect Edition\u301C";

// UTF-8 bytes of kJapaneseTitleFullwidthDigitTilde.
constexpr std::string_view kJapaneseTitleFullwidthDigitTildeUtf8 =
    "アマカノ\uFF12\uFF5EPerfect Edition\uFF5E";

// UTF-8 bytes of kJapaneseTitleFullwidthDigitWaveDash.
constexpr std::string_view kJapaneseTitleFullwidthDigitWaveDashUtf8 =
    "アマカノ\uFF12\u301CPerfect Edition\u301C";

constexpr std::array kJapaneseTitleSpellingsUtf8{
    kJapaneseTitleFullwidthTildeUtf8,
    kJapaneseTitleWaveDashUtf8,
    kJapaneseTitleFullwidthDigitTildeUtf8,
    kJapaneseTitleFullwidthDigitWaveDashUtf8,
};

constexpr std::string_view kEnglishWindowTitleUtf8 =
    "Amakano 2 ~Perfect Edition~";

// cs2confx.dll formats its PropertySheet caption as "%s システム環境設定"
// ($0500-0001$).
constexpr std::wstring_view kSystemSettingsSuffixJp =
    L" システム環境設定";
constexpr std::wstring_view kSystemSettingsSuffixEn = L" System Settings";

constexpr std::string_view kSystemSettingsSuffixJpUtf8 =
    " システム環境設定";
constexpr std::string_view kSystemSettingsSuffixEnUtf8 = " System Settings";

template <class CharT>
bool StartsWith(std::basic_string_view<CharT> text,
                std::basic_string_view<CharT> prefix) noexcept {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

template <class CharT, std::size_t SpellingCount>
std::optional<std::basic_string<CharT>> ReplaceTitlePrefix(
    std::basic_string_view<CharT> title,
    const std::array<std::basic_string_view<CharT>, SpellingCount>& japanese_spellings,
    std::basic_string_view<CharT> english_title,
    std::basic_string_view<CharT> settings_suffix_jp,
    std::basic_string_view<CharT> settings_suffix_en) {
    for (const std::basic_string_view<CharT> spelling : japanese_spellings) {
        if (!StartsWith(title, spelling)) {
            continue;
        }
        std::basic_string<CharT> translated(english_title);
        const std::basic_string_view<CharT> remainder =
            title.substr(spelling.size());
        if (remainder == settings_suffix_jp) {
            translated.append(settings_suffix_en);
        } else {
            translated.append(remainder);
        }
        return translated;
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::wstring> TranslateWindowTitle(std::wstring_view title) {
    return ReplaceTitlePrefix(title, kJapaneseTitleSpellings,
                              kEnglishWindowTitle, kSystemSettingsSuffixJp,
                              kSystemSettingsSuffixEn);
}

std::optional<std::string> TranslateWindowTitleUtf8(std::string_view utf8_title) {
    return ReplaceTitlePrefix(
        utf8_title, kJapaneseTitleSpellingsUtf8, kEnglishWindowTitleUtf8,
        kSystemSettingsSuffixJpUtf8, kSystemSettingsSuffixEnUtf8);
}

}  // namespace chrome_text
