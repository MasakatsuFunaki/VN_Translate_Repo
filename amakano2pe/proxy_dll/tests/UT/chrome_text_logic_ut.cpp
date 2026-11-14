// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "chrome_text_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace {

// "アマカノ2～Perfect Edition～" (U+FF5E FULLWIDTH TILDE), as config\startup.xml spells it.
constexpr std::wstring_view kJapaneseTitle =
    L"アマカノ2\uFF5EPerfect Edition\uFF5E";

// Same title with U+301C WAVE DASH.
constexpr std::wstring_view kJapaneseTitleWaveDash =
    L"アマカノ2\u301CPerfect Edition\u301C";

// Same title with U+FF12 FULLWIDTH DIGIT TWO + U+FF5E FULLWIDTH TILDE.
constexpr std::wstring_view kJapaneseTitleFullwidthDigit =
    L"アマカノ\uFF12\uFF5EPerfect Edition\uFF5E";

// UTF-8 bytes of kJapaneseTitle and kJapaneseTitleWaveDash.
constexpr std::string_view kJapaneseTitleUtf8 =
    "アマカノ2\uFF5EPerfect Edition\uFF5E";
constexpr std::string_view kJapaneseTitleWaveDashUtf8 =
    "アマカノ2\u301CPerfect Edition\u301C";
constexpr std::string_view kJapaneseTitleFullwidthDigitUtf8 =
    "アマカノ\uFF12\uFF5EPerfect Edition\uFF5E";

constexpr std::wstring_view kEnglishTitle = L"Amakano 2 ~Perfect Edition~";
constexpr std::string_view kEnglishTitleUtf8 = "Amakano 2 ~Perfect Edition~";

constexpr std::wstring_view kTitleSuffix = L" - Debug Build";
constexpr std::string_view kTitleSuffixUtf8 = " - Debug Build";
constexpr std::size_t kClosingTildeChars = 1;

TEST(ChromeTextLogicTest,
     ReplacesJapaneseTitlePrefixesAndPreservesTrailingSuffixAcrossEncodings) {
    EXPECT_EQ(chrome_text::TranslateWindowTitle(kJapaneseTitle),
              std::optional<std::wstring>(kEnglishTitle));
    EXPECT_EQ(chrome_text::TranslateWindowTitle(kJapaneseTitleWaveDash),
              std::optional<std::wstring>(kEnglishTitle));
    EXPECT_EQ(chrome_text::TranslateWindowTitle(kJapaneseTitleFullwidthDigit),
              std::optional<std::wstring>(kEnglishTitle));

    const std::wstring wide_with_suffix =
        std::wstring(kJapaneseTitle) + std::wstring(kTitleSuffix);
    const std::wstring expected_wide =
        std::wstring(kEnglishTitle) + std::wstring(kTitleSuffix);
    EXPECT_EQ(chrome_text::TranslateWindowTitle(wide_with_suffix),
              std::optional<std::wstring>(expected_wide));

    EXPECT_EQ(chrome_text::TranslateWindowTitleUtf8(kJapaneseTitleUtf8),
              std::optional<std::string>(kEnglishTitleUtf8));
    EXPECT_EQ(chrome_text::TranslateWindowTitleUtf8(kJapaneseTitleWaveDashUtf8),
              std::optional<std::string>(kEnglishTitleUtf8));
    EXPECT_EQ(
        chrome_text::TranslateWindowTitleUtf8(kJapaneseTitleFullwidthDigitUtf8),
        std::optional<std::string>(kEnglishTitleUtf8));

    const std::string utf8_with_suffix =
        std::string(kJapaneseTitleUtf8) + std::string(kTitleSuffixUtf8);
    const std::string expected_utf8 =
        std::string(kEnglishTitleUtf8) + std::string(kTitleSuffixUtf8);
    EXPECT_EQ(chrome_text::TranslateWindowTitleUtf8(utf8_with_suffix),
              std::optional<std::string>(expected_utf8));

    const std::wstring settings_caption_jp =
        std::wstring(kJapaneseTitle) +
        L" システム環境設定";
    EXPECT_EQ(chrome_text::TranslateWindowTitle(settings_caption_jp),
              std::optional<std::wstring>(
                  std::wstring(kEnglishTitle) + L" System Settings"));
}

TEST(ChromeTextLogicTest,
     PassesThroughEmptyUnrelatedPartialAndAlreadyEnglishTitles) {
    EXPECT_FALSE(chrome_text::TranslateWindowTitle(L"").has_value());
    EXPECT_FALSE(chrome_text::TranslateWindowTitle(L"Notepad").has_value());
    EXPECT_FALSE(chrome_text::TranslateWindowTitle(kEnglishTitle).has_value());

    const std::wstring_view without_closing_tilde =
        kJapaneseTitle.substr(0, kJapaneseTitle.size() - kClosingTildeChars);
    EXPECT_FALSE(
        chrome_text::TranslateWindowTitle(without_closing_tilde).has_value());
    EXPECT_FALSE(chrome_text::TranslateWindowTitle(
                     L"[" + std::wstring(kJapaneseTitle))
                     .has_value());

    EXPECT_FALSE(chrome_text::TranslateWindowTitleUtf8("").has_value());
    EXPECT_FALSE(chrome_text::TranslateWindowTitleUtf8("Notepad").has_value());
    EXPECT_FALSE(
        chrome_text::TranslateWindowTitleUtf8(kEnglishTitleUtf8).has_value());
}

}  // namespace
