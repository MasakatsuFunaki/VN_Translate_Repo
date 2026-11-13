// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Word-wrap tests for the message box (71 columns, 4 substituted lines).
// Values wider than the box are re-broken by the engine and the tail falls
// off screen.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

using translator_logic::BOX_COLUMNS;
using translator_logic::MAX_LINES;
using translator_logic::SINGLE_LINE_MAX;
using translator_logic::Utf8ToCp932;
using translator_logic::WRAP_AT;
using translator_logic::WRAP_INDENT;
using translator_logic::WrapForMessageBox;
using translator_logic::WrappedText;

namespace {

constexpr std::size_t kWordCycleLen = 5;
constexpr std::size_t kWordSpacePos = 4;
constexpr std::size_t kCp932PairBytes = 2;
constexpr std::size_t kEllipsisLen = 3;
constexpr std::size_t kCrlfLen = 2;

constexpr std::size_t kExpectedBoxColumns = 71;
constexpr std::size_t kExpectedWrapIndent = 0;
constexpr std::size_t kExpectedMaxLines = 4;
constexpr std::size_t kExpectedChainIndent = 2;

constexpr std::size_t kPanelPitchPx = 30;
constexpr std::size_t kFirstBandTopPx = 590;
constexpr std::size_t kBandHeightPx = 25;
constexpr std::size_t kPanelBottomPx = 709;

constexpr std::size_t kSingleLineCount = 1;
constexpr std::size_t kTwoLineCount = 2;
constexpr std::size_t kThreeLineCount = 3;
constexpr std::size_t kZeroBudgetLines = 0;

constexpr std::size_t kSweepMaxLength = 400;
constexpr std::size_t kBudgetSweepMaxLength = 300;
constexpr std::size_t kMediumProseLength = 150;
constexpr std::size_t kMultiLineProseLength = 200;
constexpr std::size_t kShortBudgetLength = 100;
constexpr std::size_t kOversizedWordLength = 500;
constexpr std::size_t kExtraParagraphTailLen = 10;
constexpr std::size_t kExtraBreakLines = 2;
constexpr std::size_t kKanaRunPairs = 120;
constexpr std::size_t kLongKanaRunPairs = 400;
constexpr std::size_t kShortKanaRunPairs = 100;
constexpr std::size_t kSixColumnsRemaining = 6;
constexpr std::size_t kThreeColumnsRemaining = 3;
constexpr std::size_t kFourColumnWordEnd = 4;
constexpr std::size_t kShortLineEndColumns = 10;
constexpr std::size_t kJoinedLineStartColumn = 10;
constexpr std::size_t kJoinedLineEndColumns = 16;
constexpr std::size_t kTwoColumnsShort = 2;

// Content columns one box can draw: the first line at the full width, every
// line after it at the wrap width.
constexpr std::size_t kBoxContent = SINGLE_LINE_MAX + (MAX_LINES - 1) * WRAP_AT;

// The physical lines the engine will draw, indent included.
std::vector<std::string> DrawnLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(pos));
            return lines;
        }
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
}

std::size_t WidestLine(const std::string& text) {
    std::size_t widest = 0;
    for (const std::string& line : DrawnLines(text)) {
        if (line.size() > widest) widest = line.size();
    }
    return widest;
}

// `n` characters of single-spaced 4-letter words: "wooo wooo wooo ...".
// Never ends on a space, so the length is exactly what was asked for.
std::string Words(std::size_t n) {
    std::string s;
    while (s.size() < n) {
        s += (s.size() % kWordCycleLen == kWordSpacePos) ? ' ' : 'w';
    }
    s.resize(n);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (s.size() < n) s += 'w';
    return s;
}

// `pairs` two-byte CP932 hiragana "あ" characters, so any break at an odd
// offset would land inside a glyph.
std::string Kana(std::size_t pairs) {
    const std::string cp932_a = Utf8ToCp932("あ");
    std::string s;
    s.reserve(pairs * cp932_a.size());
    for (std::size_t i = 0; i < pairs; ++i) s += cp932_a;
    return s;
}

// True if every 2-byte CP932 character in `line` has both of its bytes,
// scanning from the start of the line.
bool Cp932Intact(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size()) {
        const unsigned char b = static_cast<unsigned char>(line[i]);
        if (!translator_logic::IsCp932LeadByte(b)) {
            ++i;
            continue;
        }
        if (i + 1 >= line.size()) return false;
        i += kCp932PairBytes;
    }
    return true;
}

// Space-separated tokens, indents dropped.
std::vector<std::string> Tokens(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n')) ++i;
        const std::size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\n') ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

}  // namespace

// The numbers themselves, so a silent drift away from the measurement
// fails here rather than on screen.
TEST(MessageBoxGeometry, ConstantsMatchTheMeasuredBox) {
    EXPECT_EQ(BOX_COLUMNS, kExpectedBoxColumns);
    EXPECT_EQ(WRAP_AT, kExpectedBoxColumns);
    EXPECT_EQ(SINGLE_LINE_MAX, kExpectedBoxColumns);
    EXPECT_EQ(WRAP_INDENT, kExpectedWrapIndent);
    // Four lines: made by the 30 px pitch in translator.cpp (stock engine
    // shows three).  The pitch and the line budget must change together.
    EXPECT_EQ(MAX_LINES, kExpectedMaxLines);
    // 28 px at 14.0 px per glyph, measured off the panel on two shapes.
    EXPECT_EQ(translator_logic::CHAIN_INDENT, kExpectedChainIndent);
}

// The pitch must put every budgeted line inside the panel; these measurements
// are from translator_logic.h and fail if the pitch and budget diverge.
TEST(MessageBoxGeometry, EveryBudgetedLineLandsInsideThePanel) {
    const std::size_t last_band_bottom =
        kFirstBandTopPx + (MAX_LINES - 1) * kPanelPitchPx + kBandHeightPx - 1;
    EXPECT_LE(last_band_bottom, kPanelBottomPx);

    // And one line more would not fit, which is what makes the budget this
    // number rather than a larger one.
    EXPECT_GT(kFirstBandTopPx + MAX_LINES * kPanelPitchPx + kBandHeightPx - 1,
              kPanelBottomPx);

    // Consecutive bands must not overlap, or the lines collide.
    EXPECT_GE(kPanelPitchPx, kBandHeightPx);
}

TEST(MessageBoxGeometry, AChainedLineStillEndsInsideTheBox) {
    using translator_logic::CHAIN_INDENT;
    EXPECT_LE(CHAIN_INDENT + WRAP_INDENT + (WRAP_AT - CHAIN_INDENT), BOX_COLUMNS);
    EXPECT_LE(CHAIN_INDENT + (SINGLE_LINE_MAX - CHAIN_INDENT), BOX_COLUMNS);
}

TEST(MessageBoxGeometry, EveryPermittedLineFitsTheBox) {
    EXPECT_LE(WRAP_AT + WRAP_INDENT, BOX_COLUMNS);
    EXPECT_LE(SINGLE_LINE_MAX, BOX_COLUMNS);
}

TEST(WordWrap, AShortValueIsHandedOverUnchanged) {
    const std::string value = "He said nothing at all.";
    const WrappedText w = WrapForMessageBox(value);
    EXPECT_EQ(w.text, value);
    EXPECT_EQ(w.lines, kSingleLineCount);
    EXPECT_FALSE(w.truncated);
}

TEST(WordWrap, AnEmptyValueStaysEmpty) {
    const WrappedText w = WrapForMessageBox(std::string());
    EXPECT_TRUE(w.text.empty());
    EXPECT_EQ(w.lines, kSingleLineCount);
    EXPECT_FALSE(w.truncated);
}

TEST(WordWrap, AValueAtTheWrapWidthIsOneLine) {
    const std::string value = Words(WRAP_AT);
    ASSERT_EQ(value.size(), WRAP_AT);
    const WrappedText w = WrapForMessageBox(value);
    EXPECT_EQ(w.text, value);
    EXPECT_EQ(w.lines, kSingleLineCount);
    EXPECT_EQ(w.text.find('\n'), std::string::npos);
}

TEST(WordWrap, AValueAtTheSingleLineCeilingIsOneLine) {
    const std::string value = Words(SINGLE_LINE_MAX);
    ASSERT_EQ(value.size(), SINGLE_LINE_MAX);
    const WrappedText w = WrapForMessageBox(value);
    EXPECT_EQ(w.text, value);
    EXPECT_EQ(w.lines, kSingleLineCount);
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
}

TEST(WordWrap, AValueOneOverTheCeilingIsWrapped) {
    const std::string value = Words(SINGLE_LINE_MAX + 1);
    ASSERT_EQ(value.size(), SINGLE_LINE_MAX + 1);
    const WrappedText w = WrapForMessageBox(value);
    EXPECT_EQ(w.lines, kTwoLineCount);
    EXPECT_NE(w.text.find('\n'), std::string::npos);
    EXPECT_FALSE(w.truncated);
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
}

TEST(WordWrap, NoDrawnLineEverLeavesTheBox) {
    for (std::size_t n = 1; n <= kSweepMaxLength; ++n) {
        const WrappedText w = WrapForMessageBox(Words(n));
        EXPECT_LE(WidestLine(w.text), BOX_COLUMNS) << "at length " << n;
        EXPECT_LE(w.lines, MAX_LINES) << "at length " << n;
    }
}

TEST(WordWrap, ContinuationLinesCarryTheIndent) {
    const WrappedText w = WrapForMessageBox(Words(kMultiLineProseLength));
    const std::vector<std::string> lines = DrawnLines(w.text);
    ASSERT_GT(lines.size(), kSingleLineCount);
    EXPECT_NE(lines[0][0], ' ') << "the first line is not indented";
    for (std::size_t i = 1; i < lines.size(); ++i) {
        ASSERT_GT(lines[i].size(), WRAP_INDENT);
        EXPECT_EQ(lines[i].substr(0, WRAP_INDENT),
                  std::string(WRAP_INDENT, ' ')) << "line " << i;
        EXPECT_NE(lines[i][WRAP_INDENT], ' ') << "line " << i;
        EXPECT_LE(lines[i].size() - WRAP_INDENT, WRAP_AT) << "line " << i;
    }
}

TEST(WordWrap, WordsAreNotSplitWhenTheyFitTheBox) {
    const std::string value = Words(kMediumProseLength);
    const WrappedText w = WrapForMessageBox(value);
    ASSERT_FALSE(w.truncated);
    EXPECT_EQ(Tokens(w.text), Tokens(value));
}

TEST(WordWrap, AWordWiderThanTheBoxIsSplitBecauseItHasToBe) {
    const std::string value(kMultiLineProseLength, 'x');
    const WrappedText w = WrapForMessageBox(value);
    const std::vector<std::string> lines = DrawnLines(w.text);
    ASSERT_EQ(lines.size(), kThreeLineCount);
    EXPECT_EQ(lines[0].size(), SINGLE_LINE_MAX);
    EXPECT_EQ(lines[1].size(), WRAP_INDENT + WRAP_AT);
    EXPECT_FALSE(w.truncated);
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
}

TEST(WordWrap, TheFirstLineTakesTheWholeWidthBecauseItDrawsNoIndent) {
    const std::string value(SINGLE_LINE_MAX + 1, 'x');
    const std::vector<std::string> lines =
        DrawnLines(WrapForMessageBox(value).text);
    ASSERT_EQ(lines.size(), kTwoLineCount);
    EXPECT_EQ(lines[0].size(), SINGLE_LINE_MAX);
    EXPECT_EQ(lines[0][0], 'x') << "the first line starts at the margin";
    EXPECT_EQ(lines[1], std::string(WRAP_INDENT, ' ') + "x");
}

TEST(WordWrap, TheBoxHoldsWhatItsThreeLinesCanDraw) {
    const std::string exactly(kBoxContent, 'x');
    const WrappedText w = WrapForMessageBox(exactly);
    EXPECT_EQ(w.lines, MAX_LINES);
    EXPECT_FALSE(w.truncated);

    const WrappedText over = WrapForMessageBox(exactly + "x");
    EXPECT_EQ(over.lines, MAX_LINES);
    EXPECT_TRUE(over.truncated);
}

TEST(WordWrap, AValueThatFillsTheBudgetIsNotTruncated) {
    const std::string value = Words(kTwoLineCount * WRAP_AT);
    const WrappedText w = WrapForMessageBox(value);
    EXPECT_LE(w.lines, MAX_LINES);
    EXPECT_FALSE(w.truncated);
    EXPECT_EQ(Tokens(w.text), Tokens(value));
}

TEST(WordWrap, AValueOverTheLineBudgetIsTruncated) {
    const std::string value = Words(kSweepMaxLength);
    const WrappedText w = WrapForMessageBox(value);
    EXPECT_TRUE(w.truncated);
    EXPECT_EQ(w.lines, MAX_LINES);
    EXPECT_EQ(w.text.substr(w.text.size() - kEllipsisLen), "...");
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
}

TEST(WordWrap, TruncationDropsWholeWordsAndKeepsTheOrder) {
    const std::string value = Words(kSweepMaxLength);
    const WrappedText w = WrapForMessageBox(value);
    ASSERT_TRUE(w.truncated);

    std::vector<std::string> kept = Tokens(w.text);
    const std::vector<std::string> want = Tokens(value);
    ASSERT_FALSE(kept.empty());
    ASSERT_LT(kept.size(), want.size());

    const std::string last = kept.back();
    ASSERT_GE(last.size(), kEllipsisLen);
    EXPECT_EQ(last.substr(last.size() - kEllipsisLen), "...");
    kept.pop_back();
    for (std::size_t i = 0; i < kept.size(); ++i) EXPECT_EQ(kept[i], want[i]);
}

// Embedded line breaks inside the English value.

TEST(WordWrap, ABreakInsideTheValueEndsTheLineWhereItStands) {
    const WrappedText w = WrapForMessageBox("First half.\nSecond half.");
    const std::vector<std::string> lines = DrawnLines(w.text);
    ASSERT_EQ(lines.size(), kTwoLineCount);
    EXPECT_EQ(lines[0], "First half.");
    EXPECT_EQ(lines[1], std::string(WRAP_INDENT, ' ') + "Second half.");
    EXPECT_EQ(w.lines, kTwoLineCount);
    EXPECT_FALSE(w.truncated);
}

TEST(WordWrap, BreaksAndWrappingShareTheOneLineBudget) {
    const std::string value = Words(WRAP_AT + kExtraParagraphTailLen) + "\ntail";
    const WrappedText w = WrapForMessageBox(value);
    EXPECT_EQ(w.lines, kThreeLineCount);
    EXPECT_LE(w.lines, MAX_LINES);
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
    EXPECT_EQ(DrawnLines(w.text).back(),
              std::string(WRAP_INDENT, ' ') + "tail");
}

TEST(WordWrap, TooManyBreaksTruncateLikeTooMuchText) {
    std::string value = "one";
    for (std::size_t i = 0; i < MAX_LINES + kExtraBreakLines; ++i) value += "\nmore";
    const WrappedText w = WrapForMessageBox(value);
    EXPECT_TRUE(w.truncated);
    EXPECT_EQ(w.lines, MAX_LINES);
    EXPECT_EQ(w.text.substr(w.text.size() - kEllipsisLen), "...");
}

TEST(WordWrap, ATrailingBreakRidesAlongAndCostsNoLine) {
    const WrappedText w = WrapForMessageBox("Short line.\n");
    EXPECT_EQ(w.text, "Short line.\n");
    EXPECT_EQ(w.lines, kSingleLineCount);

    const std::string long_value = Words(kMediumProseLength) + "\r\n";
    const WrappedText v = WrapForMessageBox(long_value);
    EXPECT_EQ(v.text.substr(v.text.size() - kCrlfLen), "\r\n");
    EXPECT_EQ(v.lines, kThreeLineCount);
    EXPECT_FALSE(v.truncated);
}

// CP932 two-byte character boundaries.

TEST(WordWrap, AHardSplitNeverLandsInsideACp932Character) {
    const std::string value =
        std::string(SINGLE_LINE_MAX % kCp932PairBytes == 0 ? 1 : 0, 'x') +
        Kana(kKanaRunPairs);
    const WrappedText w = WrapForMessageBox(value);
    for (const std::string& line : DrawnLines(w.text)) {
        EXPECT_TRUE(Cp932Intact(line)) << "line: " << line;
    }
    EXPECT_EQ(DrawnLines(w.text)[0].size(), SINGLE_LINE_MAX - 1);
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
}

TEST(WordWrap, TheTruncationMarkerNeverSplitsACp932Character) {
    const std::string value = "x" + Kana(kLongKanaRunPairs);
    const WrappedText w = WrapForMessageBox(value);
    ASSERT_TRUE(w.truncated);
    const std::vector<std::string> lines = DrawnLines(w.text);
    for (const std::string& line : lines) {
        std::string content = line;
        if (content.size() >= kEllipsisLen &&
            content.substr(content.size() - kEllipsisLen) == "...") {
            content.resize(content.size() - kEllipsisLen);
        }
        EXPECT_TRUE(Cp932Intact(content)) << "line: " << line;
    }
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
}

// Full-box line budget boundaries.

TEST(WordWrap, AValueThatExactlyFillsEveryLineIsNotTruncated) {
    const std::string value(kBoxContent, 'x');
    const WrappedText w = WrapForMessageBox(value);
    EXPECT_EQ(w.lines, MAX_LINES);
    EXPECT_FALSE(w.truncated);
    EXPECT_EQ(DrawnLines(w.text).size(), MAX_LINES);
}

TEST(WordWrap, OneByteMoreThanTheBudgetTruncates) {
    const std::string value(kBoxContent + 1, 'x');
    const WrappedText w = WrapForMessageBox(value);
    EXPECT_EQ(w.lines, MAX_LINES);
    EXPECT_TRUE(w.truncated);
    EXPECT_EQ(w.text.substr(w.text.size() - kEllipsisLen), "...");
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
}

// Budgets smaller than the whole box.

TEST(WordWrap, AValueWrappedIntoOneLeftLineStopsThere) {
    const std::string value = Words(kMultiLineProseLength);
    const WrappedText w = WrapForMessageBox(value, kSingleLineCount);
    EXPECT_EQ(w.lines, kSingleLineCount);
    EXPECT_TRUE(w.truncated);
    EXPECT_EQ(DrawnLines(w.text).size(), kSingleLineCount);
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
    EXPECT_EQ(w.text.substr(w.text.size() - kEllipsisLen), "...");
}

TEST(WordWrap, AShortValueDoesNotCareHowManyLinesAreLeft) {
    const std::string value = "He said nothing at all.";
    for (std::size_t budget = 1; budget <= MAX_LINES; ++budget) {
        const WrappedText w = WrapForMessageBox(value, budget);
        EXPECT_EQ(w.text, value) << "budget " << budget;
        EXPECT_EQ(w.lines, kSingleLineCount) << "budget " << budget;
        EXPECT_FALSE(w.truncated) << "budget " << budget;
    }
}

TEST(WordWrap, ABudgetOfNoLinesDrawsNothing) {
    const WrappedText w =
        WrapForMessageBox(Words(kShortBudgetLength), kZeroBudgetLines);
    EXPECT_TRUE(w.text.empty());
    EXPECT_EQ(w.lines, kZeroBudgetLines);
    EXPECT_TRUE(w.truncated);
}

TEST(WordWrap, NoBudgetEverDrawsMoreLinesThanItWasGiven) {
    for (std::size_t budget = 1; budget <= MAX_LINES; ++budget) {
        for (std::size_t n = 1; n <= kBudgetSweepMaxLength; ++n) {
            const WrappedText w = WrapForMessageBox(Words(n), budget);
            EXPECT_LE(w.lines, budget) << "budget " << budget << " at " << n;
            EXPECT_LE(WidestLine(w.text), BOX_COLUMNS)
                << "budget " << budget << " at " << n;
        }
    }
}

// Marking a line that is already drawn to the width.

TEST(WordWrap, TheMarkerCanBePutOnAFinishedWrap) {
    std::string text = WrapForMessageBox(Words(kMediumProseLength)).text;
    const std::size_t before = DrawnLines(text).size();
    translator_logic::MarkLastLineTruncated(text);
    EXPECT_EQ(DrawnLines(text).size(), before);
    EXPECT_EQ(text.substr(text.size() - kEllipsisLen), "...");
    EXPECT_LE(WidestLine(text), BOX_COLUMNS);
}

TEST(WordWrap, MarkingAFullWidthLineCutsItBackToFit) {
    std::string text = Words(SINGLE_LINE_MAX);
    ASSERT_EQ(text.size(), SINGLE_LINE_MAX);
    translator_logic::MarkLastLineTruncated(text);
    EXPECT_LE(text.size(), SINGLE_LINE_MAX);
    EXPECT_EQ(text.substr(text.size() - kEllipsisLen), "...");
}

TEST(WordWrap, MarkingNeverSplitsACp932Character) {
    std::string text = WrapForMessageBox("x" + Kana(kShortKanaRunPairs)).text;
    translator_logic::MarkLastLineTruncated(text);
    for (const std::string& line : DrawnLines(text)) {
        std::string content = line;
        if (content.size() >= kEllipsisLen &&
            content.substr(content.size() - kEllipsisLen) == "...") {
            content.resize(content.size() - kEllipsisLen);
        }
        EXPECT_TRUE(Cp932Intact(content)) << "line: " << line;
    }
    EXPECT_LE(WidestLine(text), BOX_COLUMNS);
}

TEST(WordWrap, TheTruncatedLineStillFitsTheBox) {
    const std::string value(kOversizedWordLength, 'x');
    const WrappedText w = WrapForMessageBox(value);
    ASSERT_TRUE(w.truncated);
    EXPECT_EQ(w.lines, MAX_LINES);
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
    const std::vector<std::string> lines = DrawnLines(w.text);
    EXPECT_LE(lines.back().size() - WRAP_INDENT, WRAP_AT);
    EXPECT_EQ(w.text.substr(w.text.size() - kEllipsisLen), "...");
}

// Reserved columns for chained lines.

TEST(WordWrap, ReservedColumnsNarrowEveryLine) {
    using translator_logic::CHAIN_INDENT;
    const std::string value = Words(SINGLE_LINE_MAX - CHAIN_INDENT);
    const WrappedText w = WrapForMessageBox(value, MAX_LINES, CHAIN_INDENT);
    EXPECT_EQ(w.lines, kSingleLineCount);
    EXPECT_EQ(w.text, value);

    const WrappedText v = WrapForMessageBox(Words(SINGLE_LINE_MAX), MAX_LINES,
                                            CHAIN_INDENT);
    EXPECT_EQ(v.lines, kTwoLineCount)
        << "one column too wide has to wrap, not overflow";

    for (std::size_t n = 1; n <= kBudgetSweepMaxLength; ++n) {
        const WrappedText x = WrapForMessageBox(Words(n), MAX_LINES, CHAIN_INDENT);
        for (const std::string& line : DrawnLines(x.text)) {
            EXPECT_LE(line.size() + CHAIN_INDENT, BOX_COLUMNS) << "at " << n;
        }
    }
}

TEST(WordWrap, TheMarkerRespectsTheReservedColumnsToo) {
    using translator_logic::CHAIN_INDENT;
    std::string text =
        WrapForMessageBox(Words(kMediumProseLength), MAX_LINES, CHAIN_INDENT).text;
    translator_logic::MarkLastLineTruncated(text, CHAIN_INDENT);
    for (const std::string& line : DrawnLines(text)) {
        EXPECT_LE(line.size() + CHAIN_INDENT, BOX_COLUMNS);
    }
    EXPECT_EQ(text.substr(text.size() - kEllipsisLen), "...");
}

// Continuing an open line in a chained box.

TEST(WordWrap, AContinuedLineTakesWhatFitsAndWrapsTheRest) {
    const WrappedText w = WrapForMessageBox(
        "alpha beta", MAX_LINES, 0,
        SINGLE_LINE_MAX - kSixColumnsRemaining, true);
    EXPECT_EQ(w.text, "alpha\nbeta");
    EXPECT_EQ(w.lines, kTwoLineCount) << "the shared line counts as the first";
    EXPECT_EQ(w.end_columns, kFourColumnWordEnd);
}

TEST(WordWrap, AWordThatMissesTheStubMovesDownWhole) {
    const WrappedText w = WrapForMessageBox(
        "granite", MAX_LINES, 0,
        SINGLE_LINE_MAX - kThreeColumnsRemaining, true);
    EXPECT_EQ(w.text, "\ngranite");
    EXPECT_EQ(w.lines, kTwoLineCount);
}

TEST(WordWrap, AFullSharedLineStillCountsAsTheFirst) {
    const WrappedText w = WrapForMessageBox("tail", MAX_LINES, 0,
                                            SINGLE_LINE_MAX, true);
    EXPECT_EQ(w.text, "\ntail");
    EXPECT_EQ(w.lines, kTwoLineCount);
    EXPECT_EQ(w.end_columns, kFourColumnWordEnd);
}

TEST(WordWrap, EndColumnsReportThePenAfterTheLastLine) {
    using translator_logic::CHAIN_INDENT;
    EXPECT_EQ(WrapForMessageBox("short line").end_columns,
              kShortLineEndColumns);
    EXPECT_EQ(WrapForMessageBox("joined", MAX_LINES, 0,
                                kJoinedLineStartColumn, true).end_columns,
              kJoinedLineEndColumns);
    const std::string two =
        std::string(SINGLE_LINE_MAX - kTwoColumnsShort, 'a') + " tail";
    const WrappedText w = WrapForMessageBox(two, MAX_LINES, CHAIN_INDENT);
    EXPECT_EQ(w.lines, kTwoLineCount);
    EXPECT_EQ(w.end_columns, CHAIN_INDENT + kFourColumnWordEnd);
}

TEST(WordWrap, AContinuedWrapNeverWidensTheSharedLinePastTheBox) {
    for (std::size_t taken = 1; taken <= SINGLE_LINE_MAX; ++taken) {
        const WrappedText w = WrapForMessageBox(
            Words(kMultiLineProseLength), MAX_LINES, 0, taken, true);
        const std::vector<std::string> lines = DrawnLines(w.text);
        ASSERT_FALSE(lines.empty());
        EXPECT_LE(lines[0].size() + taken, BOX_COLUMNS) << "at " << taken;
        for (std::size_t i = 1; i < lines.size(); ++i) {
            EXPECT_LE(lines[i].size(), BOX_COLUMNS) << "at " << taken;
        }
        EXPECT_LE(w.lines, MAX_LINES);
    }
}
