// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — Contracts for chrome_text_logic (Win32 chrome localization, dual-encoded
// CP932/UTF-8 UI string routing, "%N" tooltip word-wrapping, and 1280x720 modal
// dialog centering & button hit-rect layout).

#include "chrome_text_logic.h"
#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

TEST(ChromeTextLogicWindowTitleTest, PreservesMainWindowTitleForSingleInstanceMutex) {
    EXPECT_FALSE(chrome_text_logic::TranslateWindowTitle(L"").has_value());
    EXPECT_FALSE(
        chrome_text_logic::TranslateWindowTitle(L"CROSS†CHANNEL").has_value());
    EXPECT_FALSE(
        chrome_text_logic::TranslateWindowTitle(L"CROSS†CHANNEL Ver.1.00")
            .has_value());
    EXPECT_FALSE(
        chrome_text_logic::TranslateWindowTitle(L"Unrelated Window").has_value());
}

TEST(ChromeTextLogicWin32ChromeTest, TranslatesKnownCaptionsBodiesAndDialogControls) {
    const auto confirm = chrome_text_logic::TranslateMessageBoxCaption(L"確認");
    ASSERT_TRUE(confirm.has_value());
    EXPECT_EQ(*confirm, L"Confirm");

    const auto error = chrome_text_logic::TranslateMessageBoxCaption(L"エラー");
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(*error, L"Error");

    EXPECT_FALSE(chrome_text_logic::TranslateMessageBoxCaption(L"").has_value());
    EXPECT_FALSE(
        chrome_text_logic::TranslateMessageBoxCaption(L"Confirm").has_value());

    const auto exit_body =
        chrome_text_logic::TranslateMessageBoxBody(L"終了しますか？");
    ASSERT_TRUE(exit_body.has_value());
    EXPECT_EQ(*exit_body, L"Exit the game?");

    const auto ps2_body = chrome_text_logic::TranslateMessageBoxBody(
        L"ピクセルシェーダ2.0に対応していないため、起動できません");
    ASSERT_TRUE(ps2_body.has_value());
    EXPECT_EQ(*ps2_body,
              L"Cannot start because Pixel Shader 2.0 is not supported.");

    EXPECT_FALSE(chrome_text_logic::TranslateMessageBoxBody(L"").has_value());
    EXPECT_FALSE(
        chrome_text_logic::TranslateMessageBoxBody(L"Exit the game?").has_value());

    const auto about_title =
        chrome_text_logic::TranslateDialogText(L"バージョン情報 ph");
    ASSERT_TRUE(about_title.has_value());
    EXPECT_EQ(*about_title, L"About CROSS†CHANNEL");

    const auto about_ver =
        chrome_text_logic::TranslateDialogText(L"cc, Version 1.0");
    ASSERT_TRUE(about_ver.has_value());
    EXPECT_EQ(*about_ver, L"CROSS†CHANNEL Version 1.00");

    EXPECT_FALSE(chrome_text_logic::TranslateDialogText(L"").has_value());
    EXPECT_FALSE(chrome_text_logic::TranslateDialogText(L"OK").has_value());
}

TEST(ChromeTextLogicEngineUiTest, AcceptsBothCp932AndUtf8KeysAndRoutesTooltips) {
    const std::string yes_utf8 = "はい";
    const std::string yes_cp932 = translator_logic::Utf8ToCp932(yes_utf8);
    const auto from_cp932 = chrome_text_logic::TranslateEngineUiCp932(yes_cp932);
    const auto from_utf8 = chrome_text_logic::TranslateEngineUiCp932(yes_utf8);
    ASSERT_TRUE(from_cp932.has_value());
    ASSERT_TRUE(from_utf8.has_value());
    EXPECT_EQ(*from_cp932, "Yes");
    EXPECT_EQ(*from_utf8, "Yes");

    // Format specifiers (%s, %d) must survive in exact positional order.
    const std::string ach_cp932 =
        translator_logic::Utf8ToCp932("実績解除【%s : %dG】");
    const auto ach_en = chrome_text_logic::TranslateEngineUiCp932(ach_cp932);
    ASSERT_TRUE(ach_en.has_value());
    EXPECT_EQ(*ach_en, "Unlocked [%s: %dG]");

    // TranslateConfigTooltipCp932 rejects non-tooltip UI keys, whereas
    // TranslateEngineUiCp932 routes tooltip keys through default 28-col wrapping.
    EXPECT_FALSE(
        chrome_text_logic::TranslateConfigTooltipCp932(yes_cp932).has_value());
    EXPECT_FALSE(
        chrome_text_logic::TranslateConfigTooltipCp932("").has_value());

    const std::string bgm_tip_cp932 = translator_logic::Utf8ToCp932(
        "ゲーム中に流れるＢＧＭのボリュームを設定します。%N%N"
        "『ＭＩＮ』の方に設定するほど、小さな音で再生されます。%N%N"
        "『ＭＡＸ』の方に設定するほど、大きな音で再生されます。");
    const auto via_ui = chrome_text_logic::TranslateEngineUiCp932(bgm_tip_cp932);
    const auto via_tip_default =
        chrome_text_logic::TranslateConfigTooltipCp932(bgm_tip_cp932);
    ASSERT_TRUE(via_ui.has_value());
    ASSERT_TRUE(via_tip_default.has_value());
    EXPECT_EQ(*via_ui, *via_tip_default);

    // Empty and already-English scene/UI strings return std::nullopt.
    EXPECT_FALSE(chrome_text_logic::TranslateEngineUiCp932("").has_value());
    EXPECT_FALSE(chrome_text_logic::TranslateEngineUiCp932("Yes").has_value());
    EXPECT_FALSE(
        chrome_text_logic::TranslateEngineUiCp932("CROSS POINT").has_value());
}

TEST(ChromeTextLogicTooltipWrapTest, EnforcesColumnBoundaryParagraphsAndHardSplits) {
    // Exact boundary seam around max_cols = 10: 9, 10, and 11 columns.
    EXPECT_EQ(chrome_text_logic::WrapEngineTooltipCp932("abcd efgh", 10u),
              "abcd efgh");
    EXPECT_EQ(chrome_text_logic::WrapEngineTooltipCp932("abcd efghi", 10u),
              "abcd efghi");
    EXPECT_EQ(chrome_text_logic::WrapEngineTooltipCp932("abcd efghij", 10u),
              "abcd%Nefghij");

    // Collapses leading, repeated, and trailing ASCII spaces inside a paragraph.
    EXPECT_EQ(chrome_text_logic::WrapEngineTooltipCp932("   one    two   ", 10u),
              "one two");

    // Preserves explicit "%N%N" blank-line paragraph breaks while wrapping each paragraph.
    EXPECT_EQ(chrome_text_logic::WrapEngineTooltipCp932(
                  "Alpha beta.%N%NGamma delta.", 8u),
              "Alpha%Nbeta.%N%NGamma%Ndelta.");

    // Degenerate empty and zero-column inputs pass through unchanged.
    EXPECT_EQ(chrome_text_logic::WrapEngineTooltipCp932("", 10u), "");
    EXPECT_EQ(chrome_text_logic::WrapEngineTooltipCp932("abcd efghij", 0u),
              "abcd efghij");

    // Single token exceeding max_cols hard-splits into exact max_cols chunks.
    EXPECT_EQ(chrome_text_logic::WrapEngineTooltipCp932(
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", 10u),
              "ABCDEFGHIJ%NKLMNOPQRST%NUVWXYZ0123%N45");
}

TEST(ChromeTextLogicModalLayoutTest, MeasuresCp932UnitsAndComputesSymmetricHitRectCoords) {
    EXPECT_EQ(chrome_text_logic::MeasureEngineDisplayUnits(""), 0u);
    EXPECT_EQ(chrome_text_logic::MeasureEngineDisplayUnits("Yes"), 3u);

    // Both CP932 lead-byte ranges (0x81..0x9F hiragana and 0xE0..0xFC kanji)
    // count as 2 display units per 2-byte glyph; measurement stops at "%N" or '\0'.
    const std::string hira_with_nl =
        translator_logic::Utf8ToCp932("はい") + "%N" +
        translator_logic::Utf8ToCp932("いいえ");
    EXPECT_EQ(chrome_text_logic::MeasureEngineDisplayUnits(hira_with_nl), 4u);

    const std::string high_range_kanji = translator_logic::Utf8ToCp932("崩壊");
    EXPECT_EQ(chrome_text_logic::MeasureEngineDisplayUnits(high_range_kanji), 4u);

    // Literal '%' not followed by 'N' counts as 1 ASCII unit, embedded NUL stops,
    // and a truncated trailing CP932 lead byte (0x81 or 0xE0) does not read out-of-bounds.
    EXPECT_EQ(chrome_text_logic::MeasureEngineDisplayUnits("50% off"), 7u);
    EXPECT_EQ(chrome_text_logic::MeasureEngineDisplayUnits(
                  std::string_view("AB\0CD", 5u)),
              2u);
    EXPECT_EQ(chrome_text_logic::MeasureEngineDisplayUnits(
                  std::string_view("\x81", 1u)),
              1u);
    EXPECT_EQ(chrome_text_logic::MeasureEngineDisplayUnits(
                  std::string_view("\xE0", 1u)),
              1u);

    // 34-char ASCII line at step_x = 24.0f (12 px/unit => 408 px wide):
    // centered on 640.0f => x = 640 - 204 = 436.0f; with +20 offset => 456.0f.
    const std::string prompt = "Any unsaved progress will be lost.";
    ASSERT_EQ(prompt.size(), 34u);
    EXPECT_FLOAT_EQ(chrome_text_logic::ComputeModalLineX(prompt, 0, 24.0f),
                    436.0f);
    EXPECT_FLOAT_EQ(chrome_text_logic::ComputeModalLineX(prompt, 20, 24.0f),
                    456.0f);

    // Native vertical centering parity:
    //   1 line w/ buttons:  0x55 - ((1*0x26 - 2)/2) = 85 - 18 = 67
    //   3 lines w/ buttons: 0x55 - ((3*0x26 - 2)/2) = 85 - 56 = 29
    //   1 line no buttons:  0x68 - ((1*0x26 - 2)/2) = 104 - 18 = 86
    EXPECT_EQ(chrome_text_logic::ComputeModalFirstLineOffsetY(1, true), 67);
    EXPECT_EQ(chrome_text_logic::ComputeModalFirstLineOffsetY(3, true), 29);
    EXPECT_EQ(chrome_text_logic::ComputeModalFirstLineOffsetY(1, false), 86);

    // Button layout at modal origin (0, 260), step_x = 24.0f (12 px/unit):
    //   Yes ("Yes", 36 px) centered in [535, 607] => x = 553.0f, right = 589.0f
    //   "/" (12 px) centered in gap [607, 643]    => x = 619.0f, right = 631.0f
    //   No ("No", 24 px) symmetric across "/"     => x = 661.0f, right = 685.0f (inside [643, 751])
    //   y = 260 + 0x8E (142) = 402.0f
    const auto btn = chrome_text_logic::ComputeModalButtonLayout(0, 260, 24.0f);
    EXPECT_FLOAT_EQ(btn.yes_x, 553.0f);
    EXPECT_FLOAT_EQ(btn.slash_x, 619.0f);
    EXPECT_FLOAT_EQ(btn.no_x, 661.0f);
    EXPECT_FLOAT_EQ(btn.y, 402.0f);
}
