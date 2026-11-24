// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-room unit tests for Sakura no Uta UTF-16 Win32 UI chrome lookup,
// longest-first window title matching, accelerator/mnemonic preservation,
// numeric readout normalization, and PIMPL copy/move semantics.

#include "chrome_text_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace chrome_text {
namespace {

constexpr std::size_t kEmptyDictionarySize = 0u;
constexpr std::wstring_view kFpsSuffix = L" [60.0 fps]";
constexpr std::wstring_view kAltFpsSuffix = L" [59.9 fps]";
constexpr std::wstring_view kVersionSuffix = L" [Ver 1.00]";

TEST(ChromeTextLogicUt,
     TranslatesWindowTitleLongestFirstPreservingTrailingSuffix) {
    const std::wstring base_english(kEnglishWindowTitle);
    const std::wstring english_with_fps = base_english + std::wstring(kFpsSuffix);
    const std::wstring english_with_alt_fps =
        base_english + std::wstring(kAltFpsSuffix);
    const std::wstring english_with_ver =
        base_english + std::wstring(kVersionSuffix);

    struct PositiveTitleCase {
        const char* label;
        std::wstring_view input;
        const std::wstring* expected;
    };

    const PositiveTitleCase kPositiveCases[] = {
        {"full_fullwidth_hyphen_bare", L"サクラノ詩 －櫻の森の上を舞う－",
         &base_english},
        {"full_fullwidth_hyphen_with_fps",
         L"サクラノ詩 －櫻の森の上を舞う－ [60.0 fps]", &english_with_fps},
        {"full_horizontal_bar_with_fps",
         L"サクラノ詩 ―櫻の森の上を舞う― [59.9 fps]", &english_with_alt_fps},
        {"full_em_dash_bare", L"サクラノ詩 —櫻の森の上を舞う—", &base_english},
        {"full_ascii_hyphen_with_ver",
         L"サクラノ詩 -櫻の森の上を舞う- [Ver 1.00]", &english_with_ver},
        {"kanji_stem_full_subtitle_with_fps",
         L"桜の詩 －櫻の森の上を舞う－ [60.0 fps]", &english_with_fps},
        {"katakana_stem_bare", L"サクラノ詩", &base_english},
        {"katakana_stem_with_fps", L"サクラノ詩 [60.0 fps]", &english_with_fps},
        {"kanji_stem_bare", L"桜の詩", &base_english},
    };

    const ChromeDictionary& dict = DefaultChromeDictionary();
    for (const auto& tc : kPositiveCases) {
        SCOPED_TRACE(tc.label);
        const std::optional<std::wstring> actual =
            dict.TranslateWindowTitle(tc.input);
        ASSERT_TRUE(actual.has_value());
        EXPECT_EQ(*actual, *tc.expected);
        EXPECT_EQ(TranslateWindowTitle(tc.input), actual);
    }

    constexpr wchar_t kEmbeddedNulTitle[] = {
        L'サ', L'ク', L'ラ', L'\0', L'ノ', L'詩',
    };
    const std::wstring already_english_fps =
        base_english + std::wstring(kFpsSuffix);

    struct RejectedTitleCase {
        const char* label;
        std::wstring_view input;
    };

    const RejectedTitleCase kRejectedCases[] = {
        {"empty", L""},
        {"already_english_bare", kEnglishWindowTitle},
        {"already_english_with_suffix", already_english_fps},
        {"unrelated_ascii", L"BGI Sound Player"},
        {"unrelated_japanese_control", L"環境設定"},
        {"non_prefix_occurrence", L"[Window] サクラノ詩 －櫻の森の上を舞う－"},
        {"leading_space_non_prefix", L" サクラノ詩"},
        {"embedded_nul_in_stem",
         std::wstring_view(kEmbeddedNulTitle,
                           sizeof(kEmbeddedNulTitle) / sizeof(wchar_t))},
    };

    for (const auto& tc : kRejectedCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(dict.TranslateWindowTitle(tc.input), std::nullopt);
        EXPECT_EQ(TranslateWindowTitle(tc.input), std::nullopt);
    }
}

TEST(ChromeTextLogicUt, RejectsEmptyPureAsciiAndEmbeddedNulControlText) {
    constexpr wchar_t kAsciiWithNul[] = {
        L'O', L'K', L'\0', L'C', L'a', L'n', L'c', L'e', L'l',
    };
    constexpr wchar_t kJapaneseInteriorNul[] = {
        L'環', L'境', L'\0', L'設', L'定',
    };
    constexpr wchar_t kJapaneseTrailingNul[] = {
        L'環', L'境', L'設', L'定', L'\0', L'X',
    };

    struct MissCase {
        const char* label;
        std::wstring_view input;
    };

    const MissCase kCases[] = {
        {"empty", L""},
        {"pure_ascii_word", L"Configuration"},
        {"pure_ascii_menu_mnemonic_accel", L"Close (&C)\tCtrl+W"},
        {"pure_ascii_seconds", L"0.5 s"},
        {"pure_ascii_embedded_nul",
         std::wstring_view(kAsciiWithNul,
                           sizeof(kAsciiWithNul) / sizeof(wchar_t))},
        {"unmapped_japanese", L"未登録のテスト文字列"},
        {"japanese_interior_nul",
         std::wstring_view(kJapaneseInteriorNul,
                           sizeof(kJapaneseInteriorNul) / sizeof(wchar_t))},
        {"japanese_trailing_nul_extra",
         std::wstring_view(kJapaneseTrailingNul,
                           sizeof(kJapaneseTrailingNul) / sizeof(wchar_t))},
    };

    const ChromeDictionary& dict = DefaultChromeDictionary();
    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(dict.TranslateControlOrMenuText(tc.input), std::nullopt);
        EXPECT_EQ(TranslateChromeWide(tc.input), std::nullopt);
    }
}

TEST(ChromeTextLogicUt,
     PreservesTabAcceleratorsAndValidMnemonicsWhileRejectingMalformedForms) {
    struct HitCase {
        const char* label;
        std::wstring_view input;
        std::wstring_view expected;
    };

    constexpr HitCase kHits[] = {
        {"kanji_dialog_caption", L"環境設定", L"Configuration"},
        {"halfwidth_katakana_button", L"ｷｬﾝｾﾙ", L"Cancel"},
        {"menu_tab_alt_f4", L"終了しますか？\tAlt+F4",
         L"Exit the game?\tAlt+F4"},
        {"menu_tab_ctrl_w", L"閉じる\tCtrl+W", L"Close\tCtrl+W"},
        {"tight_alpha_mnemonic", L"閉じる(&C)", L"Close(&C)"},
        {"spaced_alpha_mnemonic", L"環境設定 (&O)", L"Configuration (&O)"},
        {"digit_mnemonic", L"閉じる(&1)", L"Close(&1)"},
        {"combined_mnemonic_and_tab_accel", L"環境設定 (&O)\tCtrl+O",
         L"Configuration (&O)\tCtrl+O"},
    };

    const ChromeDictionary& dict = DefaultChromeDictionary();
    for (const auto& tc : kHits) {
        SCOPED_TRACE(tc.label);
        const std::optional<std::wstring> actual =
            dict.TranslateControlOrMenuText(tc.input);
        ASSERT_TRUE(actual.has_value());
        EXPECT_EQ(*actual, tc.expected);
        EXPECT_EQ(TranslateChromeWide(tc.input), actual);
    }

    struct MalformedMnemonicCase {
        const char* label;
        std::wstring_view input;
    };

    constexpr MalformedMnemonicCase kMalformed[] = {
        {"punctuation_mnemonic", L"閉じる(&!)"},
        {"bare_mnemonic_no_stem", L"(&C)"},
        {"multi_char_mnemonic", L"閉じる(&CC)"},
        {"non_ascii_mnemonic", L"閉じる(&あ)"},
    };

    for (const auto& tc : kMalformed) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(dict.TranslateControlOrMenuText(tc.input), std::nullopt);
    }
}

TEST(ChromeTextLogicUt,
     TranslatesFormattedDirectSoundErrorAndNormalizesSecondsReadouts) {
    struct FormattedHitCase {
        const char* label;
        std::wstring_view input;
        std::wstring_view expected;
    };

    constexpr FormattedHitCase kHits[] = {
        {"directsound_primary_buffer_result",
         L"プライマリバッファの生成に失敗しました。result[0x88780032]",
         L"Failed to create DirectSound primary buffer. result[0x88780032]"},
        {"ascii_decimal_seconds", L"0.5秒", L"0.5 s"},
        {"ascii_integer_seconds", L"0秒", L"0 s"},
        {"fullwidth_decimal_spaced_seconds", L"１２．５ 秒", L"12.5 s"},
    };

    const ChromeDictionary& dict = DefaultChromeDictionary();
    for (const auto& tc : kHits) {
        SCOPED_TRACE(tc.label);
        const std::optional<std::wstring> actual =
            dict.TranslateControlOrMenuText(tc.input);
        ASSERT_TRUE(actual.has_value());
        EXPECT_EQ(*actual, tc.expected);
    }

    struct InvalidReadoutCase {
        const char* label;
        std::wstring_view input;
    };

    constexpr InvalidReadoutCase kInvalidReadouts[] = {
        {"bare_unit_only", L"秒"},
        {"leading_dot_no_digits", L".秒"},
        {"double_decimal_point", L"1.2.3秒"},
        {"latin_letters_before_unit", L"abc秒"},
        {"fullwidth_double_decimal_point", L"１．２．３秒"},
    };

    for (const auto& tc : kInvalidReadouts) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(dict.TranslateControlOrMenuText(tc.input), std::nullopt);
    }
}

TEST(ChromeTextLogicUt, SupportsPimplCopyAndMoveSemantics) {
    ChromeDictionary original;
    const std::size_t expected_size = original.Size();
    ASSERT_GT(expected_size, kEmptyDictionarySize);

    ChromeDictionary copy_constructed(original);
    EXPECT_EQ(copy_constructed.Size(), expected_size);
    EXPECT_EQ(copy_constructed.TranslateControlOrMenuText(L"環境設定"),
              std::optional<std::wstring>(L"Configuration"));

    ChromeDictionary copy_assigned;
    copy_assigned = original;
    EXPECT_EQ(copy_assigned.Size(), expected_size);
    EXPECT_EQ(copy_assigned.TranslateControlOrMenuText(L"ｷｬﾝｾﾙ"),
              std::optional<std::wstring>(L"Cancel"));

    ChromeDictionary move_constructed(std::move(copy_constructed));
    EXPECT_EQ(move_constructed.Size(), expected_size);
    EXPECT_EQ(move_constructed.TranslateWindowTitle(L"サクラノ詩 [60.0 fps]"),
              std::optional<std::wstring>(std::wstring(kEnglishWindowTitle) +
                                          std::wstring(kFpsSuffix)));

    ChromeDictionary move_assigned;
    move_assigned = std::move(move_constructed);
    EXPECT_EQ(move_assigned.Size(), expected_size);
    EXPECT_EQ(move_assigned.TranslateControlOrMenuText(L"閉じる(&C)"),
              std::optional<std::wstring>(L"Close(&C)"));

    // Assigning into a moved-from dictionary must restore full PIMPL state.
    copy_constructed = original;
    EXPECT_EQ(copy_constructed.Size(), expected_size);
    EXPECT_EQ(copy_constructed.TranslateControlOrMenuText(L"閉じる"),
              std::optional<std::wstring>(L"Close"));
}

}  // namespace
}  // namespace chrome_text
