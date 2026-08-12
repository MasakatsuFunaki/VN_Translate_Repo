// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — English word wrap for the shingakkou message window.
//
// Tests translator_logic::EngineCharWidthPx (mirror of the engine's
// per-char measure with the proportionalizer patches: 8px halfwidth /
// 21px full cell) and translator_logic::WordWrapMessage, which pre-wraps
// English replacements at word boundaries by swapping the last space of
// an overflowing line for '\r' -- the engine's hard-break char. The
// motivating bug: the engine wraps at character granularity, slicing
// words mid-letter ("in his yo / uth", "Fath / er's heart?").
//
// All inputs are synthetic in-memory strings; no file I/O.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using translator_logic::EngineCharWidthPx;
using translator_logic::WordWrapMessage;

namespace {

const int kBudget = 536;  // message-window wrap budget (67 ASCII chars)

// Split on '\r' and return the engine-measured width of each line.
std::vector<int> LineWidths(const std::wstring& s) {
    std::vector<int> widths;
    int w = 0;
    for (wchar_t c : s) {
        if (c == L'\r') { widths.push_back(w); w = 0; continue; }
        w += EngineCharWidthPx(c);
    }
    widths.push_back(w);
    return widths;
}

}  // namespace

TEST(EngineCharWidthPx, AsciiIsHalfwidth) {
    EXPECT_EQ(EngineCharWidthPx(L'A'), 8);
    EXPECT_EQ(EngineCharWidthPx(L' '), 8);
    EXPECT_EQ(EngineCharWidthPx(L'?'), 8);
    EXPECT_EQ(EngineCharWidthPx(L'\''), 8);
}

TEST(EngineCharWidthPx, MathSignsAreFullwidthExceptions) {
    // 0xB1/0xD7/0xF7 are the engine's explicit fullwidth exceptions
    // below 0xFF.
    EXPECT_EQ(EngineCharWidthPx(L'±'), 21);  // plus-minus
    EXPECT_EQ(EngineCharWidthPx(L'×'), 21);  // multiply
    EXPECT_EQ(EngineCharWidthPx(L'÷'), 21);  // divide
}

TEST(EngineCharWidthPx, CjkAndEllipsisAreFullwidth) {
    EXPECT_EQ(EngineCharWidthPx(L'…'), 21);  // … (frequent in EN text)
    EXPECT_EQ(EngineCharWidthPx(L'神'), 21);  // 神
    EXPECT_EQ(EngineCharWidthPx(L'「'), 21);  // 「
}

TEST(EngineCharWidthPx, HalfwidthKatakanaIsHalfwidth) {
    EXPECT_EQ(EngineCharWidthPx(L'｡'), 8);
    EXPECT_EQ(EngineCharWidthPx(L'ﾟ'), 8);
    EXPECT_EQ(EngineCharWidthPx(L'ﾠ'), 21);  // just past the block
}

TEST(WordWrapMessage, ShortStringUntouched) {
    std::wstring s = L"Make sure to get to class, Prefect!";
    const std::wstring orig = s;
    EXPECT_EQ(WordWrapMessage(s, kBudget), 0);
    EXPECT_EQ(s, orig);
}

TEST(WordWrapMessage, BreaksAtSpacesNeverMidWord) {
    // The screenshot sentence that wrapped as "his yo / uth" in-game.
    std::wstring s =
        L"I narrowed my eyes the same way Uncle did. Had my father, in his "
        L"youth, looked out upon this same scene? And how had Mother saved "
        L"Father's heart?";
    const std::wstring orig = s;

    const int breaks = WordWrapMessage(s, kBudget);
    EXPECT_GE(breaks, 2);

    // Same length; '\r' appears only where a space was.
    ASSERT_EQ(s.size(), orig.size());
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] != orig[i]) {
            EXPECT_EQ(orig[i], L' ');
            EXPECT_EQ(s[i], L'\r');
        }
    }

    // Every produced line fits the budget, so the engine never re-breaks.
    for (int w : LineWidths(s)) EXPECT_LE(w, kBudget);
}

TEST(WordWrapMessage, ExactFitLineBreaksBeforeNextWord) {
    // 67 ASCII chars = exactly 536px; the following word must move down
    // whole, with the separating space consumed by the break.
    std::wstring s(67, L'a');
    s += L" next";
    EXPECT_EQ(WordWrapMessage(s, kBudget), 1);
    EXPECT_EQ(s[67], L'\r');
    std::vector<int> w = LineWidths(s);
    ASSERT_EQ(w.size(), 2u);
    EXPECT_EQ(w[0], 536);
    EXPECT_EQ(w[1], 4 * 8);  // "next" -- the space became the break
}

TEST(WordWrapMessage, OverlongWordLeftToEngine) {
    // A single word wider than the whole line (the 121-char
    // "Gakungakun..." onomatopoeia) cannot be word-wrapped; leave it to
    // the engine's char break and insert nothing.
    std::wstring s(121, L'g');
    const std::wstring orig = s;
    EXPECT_EQ(WordWrapMessage(s, kBudget), 0);
    EXPECT_EQ(s, orig);
}

TEST(WordWrapMessage, RespectsExistingHardBreaks) {
    // A pre-existing '\r' resets the line width; text that fits per
    // segment gains no extra breaks.
    std::wstring s(60, L'a');
    s += L'\r';
    s += std::wstring(60, L'b');
    const std::wstring orig = s;
    EXPECT_EQ(WordWrapMessage(s, kBudget), 0);
    EXPECT_EQ(s, orig);
}

TEST(WordWrapMessage, FullwidthCharsCountAt21px) {
    // 25 ellipses = 525px; one more fullwidth char would overflow, so the
    // word after the space moves down.
    std::wstring s(25, L'…');
    s += L" ……";
    EXPECT_EQ(WordWrapMessage(s, kBudget), 1);
    EXPECT_EQ(s[25], L'\r');
    for (int w : LineWidths(s)) EXPECT_LE(w, kBudget);
}

TEST(WordWrapMessage, MultipleBreaksAccumulate) {
    // 10 words of 30 chars: two per line (30+1+30 = 61 chars = 488px;
    // adding another word overflows), so breaks land every two words.
    std::wstring word(30, L'w');
    std::wstring s;
    for (int i = 0; i < 10; i++) {
        if (i) s += L' ';
        s += word;
    }
    const int breaks = WordWrapMessage(s, kBudget);
    EXPECT_EQ(breaks, 4);
    for (int w : LineWidths(s)) EXPECT_LE(w, kBudget);
}
