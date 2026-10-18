// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Word-wrap tests for the message box (71 columns, 3 engine lines).  Values
// wider than the box are re-broken by the engine and the tail falls off screen.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using translator_logic::BOX_COLUMNS;
using translator_logic::MAX_LINES;
using translator_logic::SINGLE_LINE_MAX;
using translator_logic::WRAP_AT;
using translator_logic::WRAP_INDENT;
using translator_logic::WrapForMessageBox;
using translator_logic::WrappedText;

namespace {

// The physical lines the engine will draw, indent included.
std::vector<std::string> DrawnLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    for (;;) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(pos));
            return lines;
        }
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
}

size_t WidestLine(const std::string& text) {
    size_t widest = 0;
    for (const std::string& line : DrawnLines(text))
        if (line.size() > widest) widest = line.size();
    return widest;
}

// `n` characters of single-spaced 4-letter words: "wooo wooo wooo ...".
// Never ends on a space, so the length is exactly what was asked for.
std::string Words(size_t n) {
    std::string s;
    while (s.size() < n) s += (s.size() % 5 == 4) ? ' ' : 'w';
    s.resize(n);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (s.size() < n) s += 'w';
    return s;
}

// `n` bytes of CP932 hiragana "a" (82 A0), so every character is two bytes
// and any break at an odd offset would land inside one.
std::string Kana(size_t pairs) {
    std::string s;
    for (size_t i = 0; i < pairs; ++i) s += "\x82\xA0";
    return s;
}

// True if every 2-byte CP932 character in `line` has both of its bytes,
// scanning from the start of the line.
bool Cp932Intact(const std::string& line) {
    size_t i = 0;
    while (i < line.size()) {
        unsigned char b = static_cast<unsigned char>(line[i]);
        if (translator_logic::IsCp932LeadByte(b)) {
            if (i + 1 >= line.size()) return false;  // lead byte, no trail
            i += 2;
        } else {
            ++i;
        }
    }
    return true;
}

// Space-separated tokens, indents dropped.
std::vector<std::string> Tokens(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n')) ++i;
        size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\n') ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

// Content columns one box can draw: the first line at the full width, every
// line after it at the wrap width.
const size_t kBoxContent = SINGLE_LINE_MAX + (MAX_LINES - 1) * WRAP_AT;

}  // namespace

// The numbers themselves, so a silent drift away from the measurement
// fails here rather than on screen.
TEST(MessageBoxGeometry, ConstantsMatchTheMeasuredBox) {
    EXPECT_EQ(BOX_COLUMNS, 71u);
    EXPECT_EQ(WRAP_AT, 71u);
    EXPECT_EQ(SINGLE_LINE_MAX, 71u);
    EXPECT_EQ(WRAP_INDENT, 0u);
    // Four lines: made by the 30 px pitch in translator.cpp (stock engine
    // shows three).  The pitch and the line budget must change together.
    EXPECT_EQ(MAX_LINES, 4u);
    // 28 px at 14.0 px per glyph, measured off the panel on two shapes.
    EXPECT_EQ(translator_logic::CHAIN_INDENT, 2u);
}

// The pitch must put every budgeted line inside the panel; these measurements
// are from translator_logic.h and fail if the pitch and budget diverge.
TEST(MessageBoxGeometry, EveryBudgetedLineLandsInsideThePanel) {
    const size_t kPitch = 30;            // DIALOGUE_LINE_PITCH
    const size_t kFirstBandTop = 590;    // the pitch cannot move line one
    const size_t kBandHeight = 25;       // ascenders to descenders
    const size_t kPanelBottom = 709;     // below this is the panel's border

    const size_t last_band_bottom =
        kFirstBandTop + (MAX_LINES - 1) * kPitch + kBandHeight - 1;
    EXPECT_LE(last_band_bottom, kPanelBottom);

    // And one line more would not fit, which is what makes the budget this
    // number rather than a larger one.
    EXPECT_GT(kFirstBandTop + MAX_LINES * kPitch + kBandHeight - 1,
              kPanelBottom);

    // Consecutive bands must not overlap, or the lines collide.
    EXPECT_GE(kPitch, kBandHeight);
}

TEST(MessageBoxGeometry, AChainedLineStillEndsInsideTheBox) {
    using translator_logic::CHAIN_INDENT;
    // The engine's own indent, our wrap indent and the content together have
    // to stop at the width the engine breaks on -- that is the whole reason
    // the chain indent is a budget item and not a cosmetic detail.
    EXPECT_LE(CHAIN_INDENT + WRAP_INDENT + (WRAP_AT - CHAIN_INDENT), BOX_COLUMNS);
    EXPECT_LE(CHAIN_INDENT + (SINGLE_LINE_MAX - CHAIN_INDENT), BOX_COLUMNS);
}

TEST(MessageBoxGeometry, EveryPermittedLineFitsTheBox) {
    // A continuation line draws its content behind any indent.
    EXPECT_LE(WRAP_AT + WRAP_INDENT, BOX_COLUMNS);
    // An unwrapped line draws no indent.
    EXPECT_LE(SINGLE_LINE_MAX, BOX_COLUMNS);
}

TEST(WordWrap, AShortValueIsHandedOverUnchanged) {
    const std::string value = "He said nothing at all.";
    WrappedText w = WrapForMessageBox(value);
    EXPECT_EQ(w.text, value);
    EXPECT_EQ(w.lines, 1u);
    EXPECT_FALSE(w.truncated);
}

TEST(WordWrap, AnEmptyValueStaysEmpty) {
    WrappedText w = WrapForMessageBox(std::string());
    EXPECT_TRUE(w.text.empty());
    EXPECT_EQ(w.lines, 1u);
    EXPECT_FALSE(w.truncated);
}

TEST(WordWrap, AValueAtTheWrapWidthIsOneLine) {
    const std::string value = Words(WRAP_AT);
    ASSERT_EQ(value.size(), WRAP_AT);
    WrappedText w = WrapForMessageBox(value);
    EXPECT_EQ(w.text, value);
    EXPECT_EQ(w.lines, 1u);
    EXPECT_EQ(w.text.find('\n'), std::string::npos);
}

TEST(WordWrap, AValueAtTheSingleLineCeilingIsOneLine) {
    const std::string value = Words(SINGLE_LINE_MAX);
    ASSERT_EQ(value.size(), SINGLE_LINE_MAX);
    WrappedText w = WrapForMessageBox(value);
    EXPECT_EQ(w.text, value);
    EXPECT_EQ(w.lines, 1u);
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
}

TEST(WordWrap, AValueOneOverTheCeilingIsWrapped) {
    const std::string value = Words(SINGLE_LINE_MAX + 1);
    ASSERT_EQ(value.size(), SINGLE_LINE_MAX + 1);
    WrappedText w = WrapForMessageBox(value);
    EXPECT_EQ(w.lines, 2u);
    EXPECT_NE(w.text.find('\n'), std::string::npos);
    EXPECT_FALSE(w.truncated);
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
}

TEST(WordWrap, NoDrawnLineEverLeavesTheBox) {
    for (size_t n = 1; n <= 400; ++n) {
        WrappedText w = WrapForMessageBox(Words(n));
        EXPECT_LE(WidestLine(w.text), BOX_COLUMNS) << "at length " << n;
        EXPECT_LE(w.lines, MAX_LINES) << "at length " << n;
    }
}

TEST(WordWrap, ContinuationLinesCarryTheIndent) {
    WrappedText w = WrapForMessageBox(Words(200));
    std::vector<std::string> lines = DrawnLines(w.text);
    ASSERT_GT(lines.size(), 1u);
    EXPECT_NE(lines[0][0], ' ') << "the first line is not indented";
    for (size_t i = 1; i < lines.size(); ++i) {
        ASSERT_GT(lines[i].size(), WRAP_INDENT);
        EXPECT_EQ(lines[i].substr(0, WRAP_INDENT),
                  std::string(WRAP_INDENT, ' ')) << "line " << i;
        EXPECT_NE(lines[i][WRAP_INDENT], ' ') << "line " << i;
        EXPECT_LE(lines[i].size() - WRAP_INDENT, WRAP_AT) << "line " << i;
    }
}

TEST(WordWrap, WordsAreNotSplitWhenTheyFitTheBox) {
    const std::string value = Words(150);
    WrappedText w = WrapForMessageBox(value);
    ASSERT_FALSE(w.truncated);
    // Every token that comes out is a token that went in, and the sequence
    // is unchanged: the breaks landed on spaces.
    EXPECT_EQ(Tokens(w.text), Tokens(value));
}

TEST(WordWrap, AWordWiderThanTheBoxIsSplitBecauseItHasToBe) {
    const std::string value(200, 'x');  // one word, no space to break on
    WrappedText w = WrapForMessageBox(value);
    std::vector<std::string> lines = DrawnLines(w.text);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0].size(), SINGLE_LINE_MAX);
    EXPECT_EQ(lines[1].size(), WRAP_INDENT + WRAP_AT);
    EXPECT_FALSE(w.truncated);
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
}

TEST(WordWrap, TheFirstLineTakesTheWholeWidthBecauseItDrawsNoIndent) {
    // Only a continuation line pays for the indent.  Charging the first line
    // for one it does not draw throws away WRAP_INDENT columns of every box.
    const std::string value(SINGLE_LINE_MAX + 1, 'x');
    std::vector<std::string> lines = DrawnLines(WrapForMessageBox(value).text);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].size(), SINGLE_LINE_MAX);
    EXPECT_EQ(lines[0][0], 'x') << "the first line starts at the margin";
    EXPECT_EQ(lines[1], std::string(WRAP_INDENT, ' ') + "x");
}

TEST(WordWrap, TheBoxHoldsWhatItsThreeLinesCanDraw) {
    // One line at the full width and MAX_LINES - 1 continuation lines behind
    // the indent.  One character more than that is one the box cannot show.
    const std::string exactly(kBoxContent, 'x');
    WrappedText w = WrapForMessageBox(exactly);
    EXPECT_EQ(w.lines, MAX_LINES);
    EXPECT_FALSE(w.truncated);

    WrappedText over = WrapForMessageBox(exactly + "x");
    EXPECT_EQ(over.lines, MAX_LINES);
    EXPECT_TRUE(over.truncated);
}

TEST(WordWrap, AValueThatFillsTheBudgetIsNotTruncated) {
    // Three lines of content: WRAP_AT + WRAP_AT + a short tail.
    const std::string value = Words(2 * WRAP_AT);
    WrappedText w = WrapForMessageBox(value);
    EXPECT_LE(w.lines, MAX_LINES);
    EXPECT_FALSE(w.truncated);
    EXPECT_EQ(Tokens(w.text), Tokens(value));
}

TEST(WordWrap, AValueOverTheLineBudgetIsTruncated) {
    const std::string value = Words(400);
    WrappedText w = WrapForMessageBox(value);
    EXPECT_TRUE(w.truncated);
    EXPECT_EQ(w.lines, MAX_LINES);
    EXPECT_EQ(w.text.substr(w.text.size() - 3), "...");
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
}

TEST(WordWrap, TruncationDropsWholeWordsAndKeepsTheOrder) {
    const std::string value = Words(400);
    WrappedText w = WrapForMessageBox(value);
    ASSERT_TRUE(w.truncated);

    std::vector<std::string> kept = Tokens(w.text);
    std::vector<std::string> want = Tokens(value);
    ASSERT_FALSE(kept.empty());
    ASSERT_LT(kept.size(), want.size());

    // The last kept token carries the marker; the rest are whole words of
    // the value, in the order the value had them.
    std::string last = kept.back();
    ASSERT_GE(last.size(), 3u);
    EXPECT_EQ(last.substr(last.size() - 3), "...");
    kept.pop_back();
    for (size_t i = 0; i < kept.size(); ++i) EXPECT_EQ(kept[i], want[i]);
}

// --- breaks the value brought with it --------------------------------

TEST(WordWrap, ABreakInsideTheValueEndsTheLineWhereItStands) {
    // The engine hands multi-line entries over in one buffer, so a 0x0A in
    // the English is a break the translation asked for.
    WrappedText w = WrapForMessageBox("First half.\nSecond half.");
    std::vector<std::string> lines = DrawnLines(w.text);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "First half.");
    EXPECT_EQ(lines[1], std::string(WRAP_INDENT, ' ') + "Second half.");
    EXPECT_EQ(w.lines, 2u);
    EXPECT_FALSE(w.truncated);
}

TEST(WordWrap, BreaksAndWrappingShareTheOneLineBudget) {
    // A paragraph that already needs two lines, plus a break, is three.
    const std::string value = Words(WRAP_AT + 10) + "\ntail";
    WrappedText w = WrapForMessageBox(value);
    EXPECT_EQ(w.lines, 3u);
    EXPECT_LE(w.lines, MAX_LINES);
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
    EXPECT_EQ(DrawnLines(w.text).back(),
              std::string(WRAP_INDENT, ' ') + "tail");
}

TEST(WordWrap, TooManyBreaksTruncateLikeTooMuchText) {
    std::string value = "one";
    for (size_t i = 0; i < MAX_LINES + 2; ++i) value += "\nmore";
    WrappedText w = WrapForMessageBox(value);
    EXPECT_TRUE(w.truncated);
    EXPECT_EQ(w.lines, MAX_LINES);
    EXPECT_EQ(w.text.substr(w.text.size() - 3), "...");
}

TEST(WordWrap, ATrailingBreakRidesAlongAndCostsNoLine) {
    // The trailing 0x0A chains this box to the next segment; it draws
    // nothing here, so it must not eat a line of the budget.
    WrappedText w = WrapForMessageBox("Short line.\n");
    EXPECT_EQ(w.text, "Short line.\n");
    EXPECT_EQ(w.lines, 1u);

    const std::string long_value = Words(150) + "\r\n";
    WrappedText v = WrapForMessageBox(long_value);
    EXPECT_EQ(v.text.substr(v.text.size() - 2), "\r\n");
    EXPECT_EQ(v.lines, 3u);
    EXPECT_FALSE(v.truncated);
}

// --- CP932 pairs -----------------------------------------------------

TEST(WordWrap, AHardSplitNeverLandsInsideACp932Character) {
    // One unbroken run of two-byte characters, offset by as many ASCII
    // bytes as it takes for the width limit to fall on a trail byte.
    const std::string value =
        std::string(SINGLE_LINE_MAX % 2 == 0 ? 1 : 0, 'x') + Kana(120);
    WrappedText w = WrapForMessageBox(value);
    for (const std::string& line : DrawnLines(w.text))
        EXPECT_TRUE(Cp932Intact(line)) << "line: " << line;
    // The break steps back one byte from the limit to stay on a boundary.
    EXPECT_EQ(DrawnLines(w.text)[0].size(), SINGLE_LINE_MAX - 1);
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
}

TEST(WordWrap, TheTruncationMarkerNeverSplitsACp932Character) {
    const std::string value = "x" + Kana(400);
    WrappedText w = WrapForMessageBox(value);
    ASSERT_TRUE(w.truncated);
    std::vector<std::string> lines = DrawnLines(w.text);
    for (const std::string& line : lines) {
        std::string content = line;
        // The marker is ASCII; strip it before checking the pairs.
        if (content.size() >= 3 && content.substr(content.size() - 3) == "...")
            content.resize(content.size() - 3);
        EXPECT_TRUE(Cp932Intact(content)) << "line: " << line;
    }
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
}

// --- the line budget -------------------------------------------------

TEST(WordWrap, AValueThatExactlyFillsEveryLineIsNotTruncated) {
    // MAX_LINES lines of solid content: the last byte that still fits.
    const std::string value(kBoxContent, 'x');
    WrappedText w = WrapForMessageBox(value);
    EXPECT_EQ(w.lines, MAX_LINES);
    EXPECT_FALSE(w.truncated);
    EXPECT_EQ(DrawnLines(w.text).size(), MAX_LINES);
}

TEST(WordWrap, OneByteMoreThanTheBudgetTruncates) {
    const std::string value(kBoxContent + 1, 'x');
    WrappedText w = WrapForMessageBox(value);
    EXPECT_EQ(w.lines, MAX_LINES);
    EXPECT_TRUE(w.truncated);
    EXPECT_EQ(w.text.substr(w.text.size() - 3), "...");
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
}

// --- a budget smaller than the whole box -----------------------------

TEST(WordWrap, AValueWrappedIntoOneLeftLineStopsThere) {
    // What a segment gets when the earlier segments of its box already used
    // two of the three lines.
    const std::string value = Words(200);
    WrappedText w = WrapForMessageBox(value, 1);
    EXPECT_EQ(w.lines, 1u);
    EXPECT_TRUE(w.truncated);
    EXPECT_EQ(DrawnLines(w.text).size(), 1u);
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
    EXPECT_EQ(w.text.substr(w.text.size() - 3), "...");
}

TEST(WordWrap, AShortValueDoesNotCareHowManyLinesAreLeft) {
    const std::string value = "He said nothing at all.";
    for (size_t budget = 1; budget <= MAX_LINES; ++budget) {
        WrappedText w = WrapForMessageBox(value, budget);
        EXPECT_EQ(w.text, value) << "budget " << budget;
        EXPECT_EQ(w.lines, 1u) << "budget " << budget;
        EXPECT_FALSE(w.truncated) << "budget " << budget;
    }
}

TEST(WordWrap, ABudgetOfNoLinesDrawsNothing) {
    WrappedText w = WrapForMessageBox(Words(100), 0);
    EXPECT_TRUE(w.text.empty());
    EXPECT_EQ(w.lines, 0u);
    EXPECT_TRUE(w.truncated);
}

TEST(WordWrap, NoBudgetEverDrawsMoreLinesThanItWasGiven) {
    for (size_t budget = 1; budget <= MAX_LINES; ++budget)
        for (size_t n = 1; n <= 300; ++n) {
            WrappedText w = WrapForMessageBox(Words(n), budget);
            EXPECT_LE(w.lines, budget) << "budget " << budget << " at " << n;
            EXPECT_LE(WidestLine(w.text), BOX_COLUMNS)
                << "budget " << budget << " at " << n;
        }
}

// --- marking a line that is already drawn to the width ----------------

TEST(WordWrap, TheMarkerCanBePutOnAFinishedWrap) {
    // The last segment that fits marks itself, because the segment that
    // gets dropped arrives after the box is already full.
    std::string text = WrapForMessageBox(Words(150)).text;
    const size_t before = DrawnLines(text).size();
    translator_logic::MarkLastLineTruncated(text);
    EXPECT_EQ(DrawnLines(text).size(), before);
    EXPECT_EQ(text.substr(text.size() - 3), "...");
    EXPECT_LE(WidestLine(text), BOX_COLUMNS);
}

TEST(WordWrap, MarkingAFullWidthLineCutsItBackToFit) {
    std::string text = Words(SINGLE_LINE_MAX);
    ASSERT_EQ(text.size(), SINGLE_LINE_MAX);
    translator_logic::MarkLastLineTruncated(text);
    EXPECT_LE(text.size(), SINGLE_LINE_MAX);
    EXPECT_EQ(text.substr(text.size() - 3), "...");
}

TEST(WordWrap, MarkingNeverSplitsACp932Character) {
    std::string text = WrapForMessageBox("x" + Kana(100)).text;
    translator_logic::MarkLastLineTruncated(text);
    for (const std::string& line : DrawnLines(text)) {
        std::string content = line;
        if (content.size() >= 3 && content.substr(content.size() - 3) == "...")
            content.resize(content.size() - 3);
        EXPECT_TRUE(Cp932Intact(content)) << "line: " << line;
    }
    EXPECT_LE(WidestLine(text), BOX_COLUMNS);
}

TEST(WordWrap, TheTruncatedLineStillFitsTheBox) {
    // A single word longer than the whole budget: the hard split and the
    // marker have to coexist inside one line.
    const std::string value(500, 'x');
    WrappedText w = WrapForMessageBox(value);
    ASSERT_TRUE(w.truncated);
    EXPECT_EQ(w.lines, MAX_LINES);
    EXPECT_LE(WidestLine(w.text), BOX_COLUMNS);
    std::vector<std::string> lines = DrawnLines(w.text);
    EXPECT_LE(lines.back().size() - WRAP_INDENT, WRAP_AT);
    EXPECT_EQ(w.text.substr(w.text.size() - 3), "...");
}

// --- the width the engine takes back from a chained line --------------

TEST(WordWrap, ReservedColumnsNarrowEveryLine) {
    using translator_logic::CHAIN_INDENT;
    // The engine starts a chained line a little in from the margin, so the
    // wrap has to stay that much narrower or the engine breaks it again.
    const std::string value = Words(SINGLE_LINE_MAX - CHAIN_INDENT);
    WrappedText w = WrapForMessageBox(value, MAX_LINES, CHAIN_INDENT);
    EXPECT_EQ(w.lines, 1u);
    EXPECT_EQ(w.text, value);

    WrappedText v = WrapForMessageBox(Words(SINGLE_LINE_MAX), MAX_LINES,
                                      CHAIN_INDENT);
    EXPECT_EQ(v.lines, 2u) << "one column too wide has to wrap, not overflow";

    for (size_t n = 1; n <= 300; ++n) {
        WrappedText x = WrapForMessageBox(Words(n), MAX_LINES, CHAIN_INDENT);
        for (const std::string& line : DrawnLines(x.text))
            EXPECT_LE(line.size() + CHAIN_INDENT, BOX_COLUMNS) << "at " << n;
    }
}

TEST(WordWrap, TheMarkerRespectsTheReservedColumnsToo) {
    using translator_logic::CHAIN_INDENT;
    std::string text = WrapForMessageBox(Words(150), MAX_LINES,
                                         CHAIN_INDENT).text;
    translator_logic::MarkLastLineTruncated(text, CHAIN_INDENT);
    for (const std::string& line : DrawnLines(text))
        EXPECT_LE(line.size() + CHAIN_INDENT, BOX_COLUMNS);
    EXPECT_EQ(text.substr(text.size() - 3), "...");
}

// --- continuing the line a chained box left open ----------------------

TEST(WordWrap, AContinuedLineTakesWhatFitsAndWrapsTheRest) {
    // Six columns left on the open line: "alpha" fits, "beta" wraps.
    WrappedText w = WrapForMessageBox("alpha beta", MAX_LINES, 0,
                                      SINGLE_LINE_MAX - 6, true);
    EXPECT_EQ(w.text, "alpha\nbeta");
    EXPECT_EQ(w.lines, 2u) << "the shared line counts as the first";
    EXPECT_EQ(w.end_columns, 4u);
}

TEST(WordWrap, AWordThatMissesTheStubMovesDownWhole) {
    // Three columns left and a seven-letter word: it moves down whole
    // rather than splitting against the stub.
    WrappedText w = WrapForMessageBox("granite", MAX_LINES, 0,
                                      SINGLE_LINE_MAX - 3, true);
    EXPECT_EQ(w.text, "\ngranite");
    EXPECT_EQ(w.lines, 2u);
}

TEST(WordWrap, AFullSharedLineStillCountsAsTheFirst) {
    WrappedText w = WrapForMessageBox("tail", MAX_LINES, 0,
                                      SINGLE_LINE_MAX, true);
    EXPECT_EQ(w.text, "\ntail");
    EXPECT_EQ(w.lines, 2u);
    EXPECT_EQ(w.end_columns, 4u);
}

TEST(WordWrap, EndColumnsReportThePenAfterTheLastLine) {
    using translator_logic::CHAIN_INDENT;
    EXPECT_EQ(WrapForMessageBox("short line").end_columns, 10u);
    // A value joining an open line reports the absolute column.
    EXPECT_EQ(WrapForMessageBox("joined", MAX_LINES, 0, 10, true).end_columns,
              16u);
    // A wrapped line of a prefixed segment starts behind the engine's indent.
    const std::string two = std::string(SINGLE_LINE_MAX - 2, 'a') + " tail";
    WrappedText w = WrapForMessageBox(two, MAX_LINES, CHAIN_INDENT);
    EXPECT_EQ(w.lines, 2u);
    EXPECT_EQ(w.end_columns, CHAIN_INDENT + 4u);
}

TEST(WordWrap, AContinuedWrapNeverWidensTheSharedLinePastTheBox) {
    for (size_t taken = 1; taken <= SINGLE_LINE_MAX; ++taken) {
        WrappedText w = WrapForMessageBox(Words(200), MAX_LINES, 0,
                                          taken, true);
        const std::vector<std::string> lines = DrawnLines(w.text);
        ASSERT_FALSE(lines.empty());
        EXPECT_LE(lines[0].size() + taken, BOX_COLUMNS) << "at " << taken;
        for (size_t i = 1; i < lines.size(); ++i)
            EXPECT_LE(lines[i].size(), BOX_COLUMNS) << "at " << taken;
        EXPECT_LE(w.lines, MAX_LINES);
    }
}
