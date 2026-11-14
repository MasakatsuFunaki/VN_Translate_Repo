// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for OS-independent SiglusEngine Win32 window title, confirmation
// prompt, right-click popup menu, and Config / Save-Load (#146) dialog lookup.

#include "chrome_text_logic.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace {

using chrome_text::ChromeDictionary;
using chrome_text::kEnglishWindowTitle;
using chrome_text::TranslateChromeWide;
using chrome_text::TranslateWindowTitle;

}  // namespace

TEST(ChromeTextLogic, TranslatesWindowTitleVariantsAndPreservesRuntimeSuffixes) {
    // Exact #GAMENAME and full-title fullwidth-tilde (U+FF5E) / wave-dash (U+301C) forms.
    EXPECT_EQ(TranslateWindowTitle(L"さよならを教えて"),
              std::optional<std::wstring>(std::wstring(kEnglishWindowTitle)));
    EXPECT_EQ(
        TranslateWindowTitle(L"さよならを教えて \uFF5Ecomment te dire adieu\uFF5E"),
        std::optional<std::wstring>(std::wstring(kEnglishWindowTitle)));
    EXPECT_EQ(
        TranslateWindowTitle(L"さよならを教えて \u301Ccomment te dire adieu\u301C"),
        std::optional<std::wstring>(std::wstring(kEnglishWindowTitle)));
    EXPECT_EQ(
        TranslateWindowTitle(L"さよならを教えて\uFF5Ecomment te dire adieu\uFF5E"),
        std::optional<std::wstring>(std::wstring(kEnglishWindowTitle)));

    // Trailing engine FPS / chapter suffixes must survive intact.
    EXPECT_EQ(TranslateWindowTitle(L"さよならを教えて [60.0 fps]"),
              std::optional<std::wstring>(
                  L"Sayonara o Oshiete ~comment te dire adieu~ [60.0 fps]"));
    EXPECT_EQ(
        TranslateWindowTitle(
            L"さよならを教えて \uFF5Ecomment te dire adieu\uFF5E - １日目"),
        std::optional<std::wstring>(
            L"Sayonara o Oshiete ~comment te dire adieu~ - １日目"));

    // Empty, already-English, or unrelated window titles return nullopt.
    EXPECT_EQ(TranslateWindowTitle(L""), std::nullopt);
    EXPECT_EQ(TranslateWindowTitle(kEnglishWindowTitle), std::nullopt);
    EXPECT_EQ(TranslateWindowTitle(L"Other Application"), std::nullopt);
}

TEST(ChromeTextLogic, TranslatesAllGameexeConfirmationPrompts) {
    struct PromptCase {
        std::wstring_view jp;
        std::wstring_view expected_en;
    };

    constexpr PromptCase kPrompts[] = {
        {L"最初から始めてもよろしいですか？", L"Start from the beginning?"},
        {L"タイトルに戻ってもよろしいですか？", L"Return to the title screen?"},
        {L"前の選択肢に戻ってもよろしいですか？",
         L"Return to the previous choice?"},
        {L"終了してもよろしいですか？", L"Are you sure you want to quit?"},
        {L"セーブしてもよろしいですか？", L"Save the game?"},
        {L"クイックセーブしてもよろしいですか？", L"Quick save the game?"},
        {L"ロードしてもよろしいですか？", L"Load this save data?"},
        {L"クイックロードしてもよろしいですか？", L"Quick load save data?"},
    };

    for (const auto& tc : kPrompts) {
        const auto actual = TranslateChromeWide(tc.jp);
        ASSERT_TRUE(actual.has_value())
            << "missing prompt for: " << std::wstring(tc.expected_en);
        EXPECT_EQ(*actual, tc.expected_en);
    }
}

TEST(ChromeTextLogic, TranslatesRightClickSystemMenuAndPreservesAccelerators) {
    struct MenuCase {
        std::wstring_view jp;
        std::wstring_view expected_en;
    };

    constexpr MenuCase kMenuItems[] = {
        {L"既読文章を早送り", L"Skip Read Text"},
        {L"文章を早送り", L"Fast Forward"},
        {L"自動早送り", L"Auto Fast Forward"},
        {L"オートモード", L"Auto Mode"},
        {L"ウィンドウを消す", L"Hide Window"},
        {L"セーブ", L"Save"},
        {L"ロード", L"Load"},
        {L"前の選択肢に戻る", L"Previous Choice"},
        {L"環境設定", L"Configuration"},
        {L"タイトルに戻る", L"Return to Title"},
        {L"ゲームを終了する", L"Quit Game"},
        {L"キャンセル", L"Cancel"},
        // Win32 menu accelerator suffixes:
        {L"セーブ\tCtrl+S", L"Save\tCtrl+S"},
        {L"キャンセル(&C)", L"Cancel(&C)"},
        {L"環境設定 (&O)\tCtrl+O", L"Configuration (&O)\tCtrl+O"},
    };

    for (const auto& tc : kMenuItems) {
        const auto actual = TranslateChromeWide(tc.jp);
        ASSERT_TRUE(actual.has_value()) << "missing menu translation";
        EXPECT_EQ(*actual, tc.expected_en);
    }
}

TEST(ChromeTextLogic, TranslatesConfigAndSaveLoadDialogControlsTabsAndColumns) {
    struct ControlCase {
        std::wstring_view jp;
        std::wstring_view expected_en;
    };

    constexpr ControlCase kControls[] = {
        // Gameexe.dat #DIALOG.* checkboxes
        {L"文章を色分けする。", L"Color-code dialogue by character."},
        {L"本プログラムの動作を遅くして、他のプログラムがスムーズに動作するようにする。",
         L"Reduce CPU usage so other programs run smoothly."},
        {L"画面暗転効果のアニメを無効にする。",
         L"Disable screen transition animations."},
        {L"画面暗転効果をマウスクリックで飛ばす。",
         L"Skip screen transitions on mouse click."},
        {L"メッセージウィンドウの開閉時のアニメを無効にする。",
         L"Disable message window open/close animations."},
        {L"マウスのホイールボタンの下回しで文章を読み進める。",
         L"Advance text by scrolling the mouse wheel down."},
        {L"声の再生中に次の文章に進んでも再生を続ける。",
         L"Continue playing voice when advancing to the next line."},
        {L"未読の文章も早送りできるようにする。",
         L"Allow skipping unread text."},
        {L"サウンド再生時に雑音が入る場合はチェックして下さい。",
         L"Check this if audio playback crackles."},
        // Gameexe.dat #SAVE* / #LOAD* & Dialog #146 labels
        {L"セーブ／ロード", L"Save / Load"},
        {L"データがありません。", L"No Data"},
        {L"セーブする", L"Save"},
        {L"セーブする場所を選択してください。", L"Select a slot to save."},
        {L"セーブする前に上書きの確認メッセージを表示する。",
         L"Confirm before overwriting existing save data."},
        {L"リストをダブルクリックでセーブする。",
         L"Double-click a slot to save."},
        {L"ロードする", L"Load"},
        {L"ロードするデータを選択してください。", L"Select save data to load."},
        {L"ロードする前に確認メッセージを表示する。",
         L"Confirm before loading save data."},
        {L"リストをダブルクリックでロードする。",
         L"Double-click a slot to load."},
        // Native Win32 dialog buttons & tabs
        {L"初期値に戻す", L"Reset to Default"},
        {L"決定", L"Confirm"},
        {L"閉じる", L"Close"},
        {L"画面", L"Screen"},
        {L"フォント", L"Font"},
        {L"文字表示速度", L"Text Speed"},
        {L"音量", L"Volume"},
        {L"システム", L"System"},
        {L"キャラクター別の音量", L"Character Voice Volume"},
        // Dialog #146 SysListView32 column headers
        {L"番号", L"No."},
        {L"日付", L"Date"},
        {L"タイトル", L"Scene"},
        {L"コメント", L"Comment"},
        // Unknown speaker placeholder
        {L"？？？", L"???"},
    };

    for (const auto& tc : kControls) {
        const auto actual = TranslateChromeWide(tc.jp);
        ASSERT_TRUE(actual.has_value())
            << "missing dialog/control entry for expected: "
            << std::wstring(tc.expected_en);
        EXPECT_EQ(*actual, tc.expected_en);
    }
}

TEST(ChromeTextLogic, ReturnsNulloptOnUnknownOrAsciiInputsAndSupportsCopy) {
    EXPECT_EQ(TranslateChromeWide(L""), std::nullopt);
    EXPECT_EQ(TranslateChromeWide(L"OK"), std::nullopt);
    EXPECT_EQ(TranslateChromeWide(L"Cancel"), std::nullopt);
    EXPECT_EQ(TranslateChromeWide(L"未登録のテスト文字列"), std::nullopt);

    ChromeDictionary dict;
    EXPECT_GT(dict.Size(), 30u);
    ChromeDictionary copy(dict);
    EXPECT_EQ(copy.Size(), dict.Size());
    EXPECT_EQ(copy.TranslateControlOrMenuText(L"初期値に戻す"),
              std::optional<std::wstring>(L"Reset to Default"));
}
