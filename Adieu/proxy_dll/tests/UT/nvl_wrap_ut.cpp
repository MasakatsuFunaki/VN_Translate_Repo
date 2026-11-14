// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for 640x480 full-screen NVL (#MWND.002) word-wrapping and
// spoken-dialogue detection in translator_logic.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace {

using translator_logic::IsDialogueLine;
using translator_logic::kDefaultDialogueCols;
using translator_logic::kDefaultNarrationCols;
using translator_logic::NvlWrapConfig;
using translator_logic::WrapForSiglusNvl;

constexpr std::size_t kNarrationBelowLimitLen = 55u;
constexpr std::size_t kNarrationExactLimitLen = 56u;
constexpr std::size_t kNarrationOverLimitLen = 57u;
constexpr std::size_t kDialogueExactLimitLen = 50u;
constexpr std::size_t kDialogueOverLimitLen = 51u;
constexpr std::size_t kOverlongTokenLen = 120u;
constexpr std::size_t kTailRemainderLen = 8u;

}  // namespace

TEST(NvlWrap, DetectsOpeningDialogueQuotesAcrossAsciiAndCjkForms) {
    EXPECT_TRUE(IsDialogueLine(L"\"Good morning, Sensei.\""));
    EXPECT_TRUE(IsDialogueLine(L"  'Wait right there.'"));
    EXPECT_TRUE(IsDialogueLine(L"“Did you sleep at all?”"));
    EXPECT_TRUE(IsDialogueLine(L"‘Just a dream.’"));
    EXPECT_TRUE(IsDialogueLine(L"「おはよう、先生」"));
    EXPECT_TRUE(IsDialogueLine(L"『昨日の続き』"));

    EXPECT_FALSE(IsDialogueLine(L""));
    EXPECT_FALSE(IsDialogueLine(L"   "));
    EXPECT_FALSE(IsDialogueLine(L"I stared up at the stained infirmary ceiling."));
    EXPECT_FALSE(IsDialogueLine(L"Hitomi"));
}

TEST(NvlWrap, NarrationBoundaryAt55And56ColumnsStaysUnwrappedAnd57Wraps) {
    // 55 code units (N - 1): stays on a single line.
    const std::wstring line_55 =
        L"I walked slowly down the quiet hallway toward the roof.";
    ASSERT_EQ(line_55.size(), kNarrationBelowLimitLen);
    EXPECT_EQ(WrapForSiglusNvl(line_55), line_55);

    // 56 code units (exact N): stays on a single line.
    const std::wstring line_56 =
        L"I walked slowly down the silent hallway toward the roof.";
    ASSERT_EQ(line_56.size(), kNarrationExactLimitLen);
    EXPECT_EQ(WrapForSiglusNvl(line_56), line_56);

    // 57 code units (N + 1): wraps before "roof." with a real U+000A newline
    // and zero trailing space on the first line.
    const std::wstring line_57 =
        L"I walked slowly down the echoing hallway toward the roof.";
    ASSERT_EQ(line_57.size(), kNarrationOverLimitLen);
    const std::wstring wrapped_57 = WrapForSiglusNvl(line_57);
    EXPECT_EQ(wrapped_57,
              L"I walked slowly down the echoing hallway toward the\nroof.");
    EXPECT_EQ(wrapped_57.find(L"\\n"), std::wstring::npos);
    EXPECT_EQ(wrapped_57.find(L" \n"), std::wstring::npos);
}

TEST(NvlWrap, DialogueLinesUseNarrower50ColumnBudget) {
    // 50 code units (exact dialogue_cols): remains a single line.
    const std::wstring quote_50 =
        L"\"Please wait for me outside the infirmary, Sensei\"";
    ASSERT_EQ(quote_50.size(), kDialogueExactLimitLen);
    EXPECT_EQ(WrapForSiglusNvl(quote_50), quote_50);

    // 51 code units (dialogue_cols + 1, still <= narration_cols 56!):
    // MUST wrap because opening quote selects dialogue_cols (50).
    const std::wstring quote_51 =
        L"\"Please wait for me outside the infirmary, Sensei.\"";
    ASSERT_EQ(quote_51.size(), kDialogueOverLimitLen);
    EXPECT_EQ(WrapForSiglusNvl(quote_51),
              L"\"Please wait for me outside the infirmary,\nSensei.\"");

    // Unquoted 54-char narration (> 50, <= 56) stays unwrapped, proving the
    // two column budgets are distinct.
    const std::wstring narration_54 =
        L"Please wait for me outside the quiet infirmary, Sensei";
    ASSERT_EQ(narration_54.size(), 54u);
    EXPECT_EQ(WrapForSiglusNvl(narration_54), narration_54);
}

TEST(NvlWrap, PreservesExplicitNewlinesAndBlankParagraphSeparators) {
    const std::wstring input =
        L"It was a dream.\n\n"
        L"The cicadas outside the open window kept screaming across the empty "
        L"courtyard.";
    const std::wstring expected =
        L"It was a dream.\n\n"
        L"The cicadas outside the open window kept screaming\n"
        L"across the empty courtyard.";
    EXPECT_EQ(WrapForSiglusNvl(input), expected);
}

TEST(NvlWrap, ChoiceModeLeavesStringsUnwrappedRegardlessOfLength) {
    NvlWrapConfig choice_cfg{};
    choice_cfg.is_choice = true;

    const std::wstring choice_label =
        L"Head up to the school rooftop before the afternoon bell rings";
    ASSERT_GT(choice_label.size(), kDefaultNarrationCols);
    EXPECT_EQ(WrapForSiglusNvl(choice_label, choice_cfg), choice_label);
}

TEST(NvlWrap, HardSplitsPathologicalUnbrokenTokensAndHandlesZeroBudgetSafely) {
    const std::wstring long_token(kOverlongTokenLen, L'A');
    const std::wstring expected =
        std::wstring(kDefaultNarrationCols, L'A') + L"\n" +
        std::wstring(kDefaultNarrationCols, L'A') + L"\n" +
        std::wstring(kTailRemainderLen, L'A');
    EXPECT_EQ(WrapForSiglusNvl(long_token), expected);

    const std::wstring prefixed =
        L"Note: " + std::wstring(60u, L'x');
    const std::wstring expected_prefixed =
        L"Note:\n" + std::wstring(kDefaultNarrationCols, L'x') + L"\nxxxx";
    const std::wstring actual_prefixed = WrapForSiglusNvl(prefixed);
    EXPECT_EQ(actual_prefixed, expected_prefixed);
    EXPECT_EQ(actual_prefixed.find(L" \n"), std::wstring::npos);

    // Degenerate zero column budget must never divide by zero or hang.
    NvlWrapConfig zero_cfg{};
    zero_cfg.narration_cols = 0u;
    zero_cfg.dialogue_cols = 0u;
    EXPECT_EQ(WrapForSiglusNvl(L"Safe fallback text", zero_cfg),
              L"Safe fallback text");
}
