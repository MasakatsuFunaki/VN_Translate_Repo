// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT -- BuildSubstitution round-trip: control prefix, trailing chain bytes,
// both key forms, and word wrap.  Keys are ASCII; CP932 changes nothing here.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using translator_logic::BoxLineBudget;
using translator_logic::BuildSubstitution;
using translator_logic::HISTORY_WINDOW;
using translator_logic::MAX_LINES;
using translator_logic::SINGLE_LINE_MAX;
using translator_logic::Substitution;
using translator_logic::SubstitutionStatus;
using translator_logic::TranslationMap;

namespace {

TranslationMap MapOf(const std::string& key, const std::string& value) {
    TranslationMap m;
    m[key] = value;
    return m;
}

}  // namespace

TEST(BuildSubstitution, AKnownKeyIsReplacedByItsValue) {
    TranslationMap m = MapOf("KEY", "the English");
    Substitution sub;
    EXPECT_EQ(BuildSubstitution("KEY", m, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, "the English");
    EXPECT_EQ(sub.tail, 0u);
    EXPECT_FALSE(sub.truncated);
}

TEST(BuildSubstitution, AnUnknownKeyIsLeftToTheEngine) {
    TranslationMap m = MapOf("KEY", "the English");
    Substitution sub;
    EXPECT_EQ(BuildSubstitution("OTHER", m, sub),
              SubstitutionStatus::NotFound);
    EXPECT_TRUE(sub.bytes.empty());
}

TEST(BuildSubstitution, ANullBufferIsReportedEmpty) {
    TranslationMap m = MapOf("KEY", "the English");
    Substitution sub;
    EXPECT_EQ(BuildSubstitution(nullptr, m, sub), SubstitutionStatus::EmptyKey);
}

TEST(BuildSubstitution, ABufferOfNothingButLineBreaksIsReportedEmpty) {
    TranslationMap m = MapOf("KEY", "the English");
    Substitution sub;
    EXPECT_EQ(BuildSubstitution("\r\n", m, sub), SubstitutionStatus::EmptyKey);
    EXPECT_EQ(BuildSubstitution("", m, sub), SubstitutionStatus::EmptyKey);
}

TEST(BuildSubstitution, ATrailingLineBreakIsStrippedForLookupAndPutBack) {
    TranslationMap m = MapOf("KEY", "the English");
    Substitution sub;
    EXPECT_EQ(BuildSubstitution("KEY\n", m, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, "the English\n");
    EXPECT_EQ(sub.tail, 1u);
}

TEST(BuildSubstitution, EveryTrailingControlByteComesBackInOrder) {
    TranslationMap m = MapOf("KEY", "the English");
    Substitution sub;
    EXPECT_EQ(BuildSubstitution("KEY\r\n", m, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, "the English\r\n");
    EXPECT_EQ(sub.tail, 2u);
}

TEST(BuildSubstitution, AKeyStoredWithItsLineBreakStillMatches) {
    // The table holds both forms; the buffer form wins the first lookup.
    // The value stored here ends in the break too, which is what a table
    // that preserves the source's own newlines produces.
    TranslationMap m = MapOf("KEY\n", "kept the break\n");
    m["KEY"] = "lost the break";
    Substitution sub;
    EXPECT_EQ(BuildSubstitution("KEY\n", m, sub),
              SubstitutionStatus::Substituted);
    // Exactly one break comes out -- the wire's, which chains the box to
    // the next segment.  A second would draw a blank line and spend one of
    // the box's three on nothing.
    EXPECT_EQ(sub.bytes, "kept the break\n");
    EXPECT_EQ(sub.tail, 1u);
    EXPECT_EQ(sub.lines, 1u);
}

TEST(BuildSubstitution, AValueThatEndsInABreakDoesNotDrawTwo) {
    TranslationMap m = MapOf("KEY", "the English\r\n");
    Substitution sub;
    EXPECT_EQ(BuildSubstitution("KEY\n", m, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, "the English\n");
    EXPECT_EQ(sub.lines, 1u);

    EXPECT_EQ(BuildSubstitution("KEY", m, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, "the English");
    EXPECT_EQ(sub.tail, 0u);
}

TEST(BuildSubstitution, TheControlPrefixIsNotPartOfTheKeyAndIsPutBack) {
    TranslationMap m = MapOf("KEY", "the English");
    Substitution sub;
    EXPECT_EQ(BuildSubstitution("\x03KEY", m, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, std::string("\x03") + "the English");
}

TEST(BuildSubstitution, OnlyTheDocumentedPrefixRangeIsSkipped) {
    TranslationMap m = MapOf("KEY", "the English");
    Substitution sub;
    // 0x01 and 0x09 are outside 0x02..0x08, so they belong to the key.
    EXPECT_EQ(BuildSubstitution("\x01KEY", m, sub),
              SubstitutionStatus::NotFound);
    EXPECT_EQ(BuildSubstitution("\x09KEY", m, sub),
              SubstitutionStatus::NotFound);
}

TEST(BuildSubstitution, ALongValueIsWrappedOnTheWayOut) {
    const std::string value(SINGLE_LINE_MAX + 20, 'x');
    TranslationMap m = MapOf("KEY", value);
    Substitution sub;
    EXPECT_EQ(BuildSubstitution("KEY", m, sub),
              SubstitutionStatus::Substituted);
    EXPECT_NE(sub.bytes.find('\n'), std::string::npos);
    EXPECT_EQ(sub.lines, 2u);
}

TEST(BuildSubstitution, WrapAndTrailingBytesAndPrefixAllSurviveTogether) {
    const std::string value(SINGLE_LINE_MAX + 20, 'x');
    TranslationMap m = MapOf("KEY", value);
    Substitution sub;
    EXPECT_EQ(BuildSubstitution("\x04KEY\n", m, sub),
              SubstitutionStatus::Substituted);
    ASSERT_FALSE(sub.bytes.empty());
    EXPECT_EQ(sub.bytes[0], '\x04');
    EXPECT_EQ(sub.bytes[sub.bytes.size() - 1], '\n');
    EXPECT_EQ(sub.tail, 1u);
    EXPECT_EQ(sub.lines, 2u);
}

// --- the box a chain of segments shares -------------------------------
//
// Segments share one box's lines: the first has no prefix, continuations
// carry one, and 0x0A says more follows.  The panel flows the chain: the
// wire's break becomes a joining space and the pen carries over.

namespace {

const uintptr_t kDialogue = 0xADEE28;
const uintptr_t kNamePlate = 0xADED20;

}  // namespace

TEST(BoxBudget, SegmentsOfOneBoxFlowOntoItsOpenLine) {
    TranslationMap m;
    m["A"] = "first segment";
    m["B"] = "second segment";
    m["C"] = "third segment";
    BoxLineBudget budget;
    Substitution sub;

    // "A\n" opens the box, "\x05B\n" and "\x05C" continue it.
    ASSERT_EQ(BuildSubstitution("A\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, "first segment ");
    EXPECT_EQ(sub.box_lines_before, 0u);
    EXPECT_EQ(sub.box_lines_after, 1u);
    EXPECT_TRUE(sub.more_follows);

    ASSERT_EQ(BuildSubstitution("\x05" "B\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, std::string("\x05") + "second segment ");
    EXPECT_EQ(sub.lines, 0u) << "it joined the line the box left open";
    EXPECT_EQ(sub.box_lines_after, 1u);
    EXPECT_TRUE(sub.continues);

    ASSERT_EQ(BuildSubstitution("\x05" "C", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, std::string("\x05") + "third segment");
    EXPECT_EQ(sub.box_lines_after, 1u);
    EXPECT_FALSE(sub.more_follows);
    EXPECT_FALSE(sub.truncated);
}

TEST(BoxBudget, AChainedBoxReadsAsOneStream) {
    // The shape the screenshots showed: three sentences, one box.  Drawn a
    // line each the box came out ragged -- a short second line, then two
    // indented ones.  Flowed, it reads as one wrapped paragraph.
    TranslationMap m;
    m["A"] = "\"But that doesn't mean believing in just anything.";
    m["B"] = "It doesn't mean believing carelessly, blindly, in order to "
             "stop thinking.";
    m["C"] = "Believing in order to stop thinking is sloth.\"";
    BoxLineBudget budget;
    Substitution sub;

    std::string drawn;
    ASSERT_EQ(BuildSubstitution("A\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    drawn += sub.bytes;
    ASSERT_EQ(BuildSubstitution("\x05" "B\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    drawn += sub.bytes.substr(1);  // the prefix byte draws nothing
    ASSERT_EQ(BuildSubstitution("\x05" "C", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    drawn += sub.bytes.substr(1);

    EXPECT_EQ(sub.box_lines_after, 3u) << "a line each would have taken four";
    EXPECT_FALSE(sub.truncated);
    // Every line but the last is more than half full -- the gaps are gone.
    size_t start = 0;
    for (size_t nl = drawn.find('\n'); nl != std::string::npos;
         nl = drawn.find('\n', nl + 1)) {
        EXPECT_GT(nl - start, translator_logic::BOX_COLUMNS / 2);
        start = nl + 1;
    }
    EXPECT_NE(drawn.find("anything. It doesn't"), std::string::npos)
        << "the sentences run on within one line";
}

TEST(BoxBudget, ASegmentIsWrappedIntoWhatTheBoxHasLeft) {
    // The first segment ends three lines in with its line still open; the
    // continuation fills the stub, takes the one fresh line that is left,
    // and is cut at a word and marked there.
    const std::string x71(SINGLE_LINE_MAX, 'x');
    TranslationMap m;
    m["A"] = x71 + " " + x71 + " " + std::string(40, 'x');
    m["B"] = std::string(30, 'y') + " " + std::string(69, 'y') + " " +
             std::string(20, 'y');
    BoxLineBudget budget;
    Substitution sub;

    ASSERT_EQ(BuildSubstitution("A\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.lines, 3u);
    EXPECT_EQ(sub.bytes.back(), ' ') << "the chain break became a space";

    ASSERT_EQ(BuildSubstitution("\x05" "B", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.lines, 1u) << "only one fresh line was left for it";
    EXPECT_TRUE(sub.truncated);
    EXPECT_EQ(sub.bytes.substr(sub.bytes.size() - 3), "...");
    EXPECT_EQ(sub.box_lines_after, MAX_LINES);
}

TEST(BoxBudget, AFullBoxWithMoreComingSaysSoOnItsLastVisibleLine) {
    // The segment that gets dropped arrives after the box is already full,
    // so the marker has to go on the segment that filled it.
    TranslationMap m;
    m["A"] = std::string(translator_logic::MAX_LINES * translator_logic::WRAP_AT, 'x');
    m["B"] = "this one cannot be drawn";
    BoxLineBudget budget;
    Substitution sub;

    ASSERT_EQ(BuildSubstitution("A\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.lines, MAX_LINES);
    EXPECT_TRUE(sub.truncated) << "the box is full and the wire says more";
    EXPECT_EQ(sub.bytes.substr(sub.bytes.size() - 3), "...");

    ASSERT_EQ(BuildSubstitution("\x05" "B", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_TRUE(sub.dropped);
    EXPECT_EQ(sub.lines, 0u);
    EXPECT_EQ(sub.bytes, "\x05");
}

TEST(BoxBudget, TheJoiningSpaceYieldsToAFreshLineWhenTheLineIsFull) {
    // A segment that ends its line exactly cannot take the joining space,
    // so the chain opens the next line instead and nothing overflows.
    TranslationMap m;
    m["A"] = std::string(SINGLE_LINE_MAX, 'x');
    m["B"] = "next";
    BoxLineBudget budget;
    Substitution sub;

    ASSERT_EQ(BuildSubstitution("A\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes.back(), '\n');
    EXPECT_EQ(sub.lines, 2u) << "the opened line is charged to the opener";

    ASSERT_EQ(BuildSubstitution("\x05" "B", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, std::string("\x05") + "next");
    EXPECT_EQ(sub.box_lines_after, 2u);
}

TEST(BoxBudget, ARerenderedContinuationDrawsExactlyWhatItDrewFirst) {
    // The engine renders one segment many times (typewriter and repaint),
    // always with the same wire.  Each re-render must come out byte for
    // byte the same, or the panel shows the sentence twice -- the second
    // copy joined after the first as if it were the next segment.
    TranslationMap m;
    m["A"] = "She sits with her feet neatly together.";
    m["B"] = "A pair of crutches leans against the seat beside her.";
    BoxLineBudget budget;
    Substitution sub;

    ASSERT_EQ(BuildSubstitution("A\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("\x05" "B\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    const std::string first = sub.bytes;
    const size_t lines_after = sub.box_lines_after;

    for (int repeat = 0; repeat < 3; ++repeat) {
        ASSERT_EQ(BuildSubstitution("\x05" "B\n", m, budget, kDialogue, sub),
                  SubstitutionStatus::Substituted);
        EXPECT_EQ(sub.bytes, first) << "repeat " << repeat;
        EXPECT_EQ(sub.box_lines_after, lines_after) << "repeat " << repeat;
    }
}

TEST(BoxBudget, ARerenderAfterTheBoxClosedStillMatchesItsFirstDraw) {
    // The final segment closes the box, and the engine keeps re-rendering
    // it.  Without the replay the re-render would start the box over and
    // lay the text out from the margin -- a second, differently placed copy.
    TranslationMap m;
    m["A"] = "She sits with her feet neatly together.";
    m["B"] = "A pair of crutches leans against the seat beside her.";
    BoxLineBudget budget;
    Substitution sub;

    ASSERT_EQ(BuildSubstitution("A\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("\x05" "B", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    const std::string first = sub.bytes;
    ASSERT_FALSE(sub.more_follows) << "the box closed on this segment";

    ASSERT_EQ(BuildSubstitution("\x05" "B", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, first);
}

TEST(BoxBudget, ARerenderedUntranslatedSegmentChargesItsLinesOnce) {
    TranslationMap m;
    m["A"] = "opening line";
    BoxLineBudget budget;
    Substitution sub;

    ASSERT_EQ(BuildSubstitution("A\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("\x05" "untranslated\n", m, budget,
                                kDialogue, sub),
              SubstitutionStatus::NotFound);
    const size_t lines_after = sub.box_lines_after;
    ASSERT_EQ(BuildSubstitution("\x05" "untranslated\n", m, budget,
                                kDialogue, sub),
              SubstitutionStatus::NotFound);
    EXPECT_EQ(sub.box_lines_after, lines_after);
}

TEST(BoxBudget, TheHistoryWindowDoesNotFlow) {
    // The backlog redraws each box in one call; a trailing break there
    // belongs to the entry and is put back, not turned into a space.
    TranslationMap m = MapOf("KEY", "the English");
    BoxLineBudget budget;
    Substitution sub;
    ASSERT_EQ(BuildSubstitution("KEY\n", m, budget, kDialogue,
                                HISTORY_WINDOW, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, "the English\n");
    EXPECT_EQ(sub.tail, 1u);
}

TEST(BoxBudget, AValueThatFillsTheBoxAloneIsNotMarked) {
    // Same value, but nothing follows it: a full box is a full box and the
    // player loses nothing.
    TranslationMap m;
    m["A"] = std::string(translator_logic::MAX_LINES * translator_logic::WRAP_AT, 'x');
    BoxLineBudget budget;
    Substitution sub;
    ASSERT_EQ(BuildSubstitution("A", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.lines, MAX_LINES);
    EXPECT_FALSE(sub.truncated);
}

TEST(BoxBudget, ASegmentWithoutAPrefixStartsTheBoxOver) {
    TranslationMap m;
    m["A"] = std::string(2 * translator_logic::WRAP_AT, 'x');
    BoxLineBudget budget;
    Substitution sub;

    ASSERT_EQ(BuildSubstitution("A", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.box_lines_after, 2u);
    // A box that was closed and a segment that opens a new one: the second
    // value gets the whole box back, not the one line the first left.
    ASSERT_EQ(BuildSubstitution("A", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.box_lines_before, 0u);
    EXPECT_EQ(sub.lines, 2u);
    EXPECT_FALSE(sub.truncated);
}

TEST(BoxBudget, ARepaintOfTheWholeBoxCountsFromTheTopAgain) {
    // Whatever redraws a box -- a load, a repaint, the same scene played
    // again -- reissues its opening segment, and the count follows it.
    TranslationMap m;
    m["A"] = "opening line";
    m["B"] = std::string(2 * (translator_logic::WRAP_AT -
                              translator_logic::CHAIN_INDENT), 'x');
    BoxLineBudget budget;
    Substitution sub;

    for (int pass = 0; pass < 3; ++pass) {
        ASSERT_EQ(BuildSubstitution("A\n", m, budget, kDialogue, sub),
                  SubstitutionStatus::Substituted);
        EXPECT_EQ(sub.box_lines_before, 0u) << "pass " << pass;
        ASSERT_EQ(BuildSubstitution("\x05" "B", m, budget, kDialogue, sub),
                  SubstitutionStatus::Substituted);
        EXPECT_EQ(sub.lines, 2u) << "pass " << pass;
        EXPECT_FALSE(sub.truncated) << "pass " << pass;
    }
}

TEST(BoxBudget, AContinuationAfterAClosedBoxStartsOver) {
    // The safe reading of a protocol the engine broke: rather than counting
    // against a box nobody is drawing into any more, start again.  Losing
    // text that would have fitted is worse than the overflow being fixed.
    TranslationMap m;
    m["A"] = std::string(translator_logic::MAX_LINES * translator_logic::WRAP_AT, 'x');
    m["B"] = std::string(2 * (translator_logic::WRAP_AT -
                              translator_logic::CHAIN_INDENT), 'y');
    BoxLineBudget budget;
    Substitution sub;

    ASSERT_EQ(BuildSubstitution("A", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);   // fills it, box closed
    ASSERT_EQ(BuildSubstitution("\x05" "B", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.box_lines_before, 0u);
    EXPECT_EQ(sub.lines, 2u);
    EXPECT_FALSE(sub.dropped);
}

TEST(BoxBudget, TheNamePlateDoesNotSpendTheDialogueBoxLines) {
    // A name is drawn between two dialogue segments of one box.  It is a
    // different box, so it must leave the dialogue count where it was.
    TranslationMap m;
    m["A"] = "opening line";
    m["NAME"] = "Naoya";
    // Exactly two lines of a chained segment, which the engine indents.
    m["B"] = std::string(
        (translator_logic::SINGLE_LINE_MAX - translator_logic::CHAIN_INDENT) +
            (translator_logic::WRAP_AT - translator_logic::CHAIN_INDENT), 'x');
    BoxLineBudget budget;
    Substitution sub;

    ASSERT_EQ(BuildSubstitution("A\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("NAME", m, budget, kNamePlate, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.box_lines_before, 0u) << "the plate has its own count";

    ASSERT_EQ(BuildSubstitution("\x05" "B", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.box_lines_before, 1u) << "the dialogue box kept its line";
    EXPECT_EQ(sub.lines, 2u);
}

TEST(BoxBudget, AnUntranslatedSegmentStillSpendsTheBoxLines) {
    // The engine draws the Japanese for a row the table does not have, and
    // that fills the box exactly as English does.
    TranslationMap m;
    m["B"] = std::string(translator_logic::MAX_LINES * translator_logic::WRAP_AT, 'x');
    BoxLineBudget budget;
    Substitution sub;

    EXPECT_EQ(BuildSubstitution("untranslated\n", m, budget, kDialogue, sub),
              SubstitutionStatus::NotFound);
    EXPECT_EQ(sub.box_lines_after, 1u);

    EXPECT_EQ(BuildSubstitution("\x05" "B", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.box_lines_before, 1u);
    EXPECT_EQ(sub.lines, MAX_LINES - 1);
    EXPECT_TRUE(sub.truncated);
}

TEST(BoxBudget, AnUntranslatedSegmentCountsItsOwnBreaks) {
    TranslationMap m;
    BoxLineBudget budget;
    Substitution sub;
    EXPECT_EQ(BuildSubstitution("line one\nline two\n", m, budget, kDialogue, sub),
              SubstitutionStatus::NotFound);
    EXPECT_EQ(sub.box_lines_after, 2u);
}

TEST(BoxBudget, AnUntranslatedSegmentCountsTheLinesTheEngineWrapsItInto) {
    using translator_logic::BOX_COLUMNS;
    // The engine word-wraps what it draws itself, so a Japanese string with
    // no break in it still takes two lines when it is wider than one.
    // Counting only its 0x0A would leave the box a line short and put the
    // next segment of the chain below the panel.
    TranslationMap m;
    BoxLineBudget budget;
    Substitution sub;

    const std::string one_line(BOX_COLUMNS, 'j');
    ASSERT_EQ(BuildSubstitution((one_line + "\n").c_str(), m, budget,
                                kDialogue, sub),
              SubstitutionStatus::NotFound);
    EXPECT_EQ(sub.box_lines_after, 1u);

    budget.Clear();
    const std::string two_lines(BOX_COLUMNS + 1, 'j');
    ASSERT_EQ(BuildSubstitution((two_lines + "\n").c_str(), m, budget,
                                kDialogue, sub),
              SubstitutionStatus::NotFound);
    EXPECT_EQ(sub.box_lines_after, 2u);
}

TEST(BoxBudget, AnUntranslatedSegmentWrapsOnCharactersWhenItIsJapanese) {
    using translator_logic::BOX_COLUMNS;
    // Every full-width character is a place the engine may break, and each
    // one is two columns wide.
    TranslationMap m;
    BoxLineBudget budget;
    Substitution sub;

    // A line holds BOX_COLUMNS / 2 of them; the next one starts a line.
    std::string fits;
    for (size_t i = 0; i < BOX_COLUMNS / 2; ++i) fits += "\x82\xA0";
    ASSERT_EQ(BuildSubstitution(fits.c_str(), m, budget, kDialogue, sub),
              SubstitutionStatus::NotFound);
    EXPECT_EQ(sub.box_lines_after, 1u);

    budget.Clear();
    ASSERT_EQ(BuildSubstitution((fits + "\x82\xA0").c_str(), m, budget,
                                kDialogue, sub),
              SubstitutionStatus::NotFound);
    EXPECT_EQ(sub.box_lines_after, 2u);
}

TEST(BoxBudget, AnUntranslatedChainedSegmentPaysTheEnginesIndentToo) {
    using translator_logic::BOX_COLUMNS;
    using translator_logic::CHAIN_INDENT;
    TranslationMap m;
    m["A"] = "opening line";
    BoxLineBudget budget;
    Substitution sub;

    ASSERT_EQ(BuildSubstitution("A\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    // Exactly the width a chained line has: one line, not two.
    const std::string fits(BOX_COLUMNS - CHAIN_INDENT, 'j');
    ASSERT_EQ(BuildSubstitution(("\x05" + fits).c_str(), m, budget,
                                kDialogue, sub),
              SubstitutionStatus::NotFound);
    EXPECT_EQ(sub.lines, 1u);
    EXPECT_EQ(sub.box_lines_after, 2u);
}

TEST(BoxBudget, ASegmentOfNothingButABreakStillMovesTheBoxOn) {
    TranslationMap m;
    m["B"] = "text";
    BoxLineBudget budget;
    Substitution sub;
    EXPECT_EQ(BuildSubstitution("\n", m, budget, kDialogue, sub),
              SubstitutionStatus::EmptyKey);
    EXPECT_EQ(sub.box_lines_after, 1u);
    EXPECT_EQ(BuildSubstitution("\x05" "B", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.box_lines_before, 1u);
}

TEST(BoxBudget, ACallThatDrawsNothingAndChainsNothingLeavesTheBoxAlone) {
    // A buffer of control bytes with no break is not a segment of anything:
    // it draws no line and hands the box to nobody.  Treating it as an
    // opening segment would throw away the count of a chain still being
    // drawn, and hand the segments after it lines that are not there.
    TranslationMap m;
    m["A"] = "opening line";
    m["B"] = std::string(2 * translator_logic::WRAP_AT, 'x');
    BoxLineBudget budget;
    Substitution sub;

    ASSERT_EQ(BuildSubstitution("A\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(BuildSubstitution("", m, budget, kDialogue, sub),
              SubstitutionStatus::EmptyKey);
    EXPECT_EQ(sub.lines, 0u);
    EXPECT_EQ(sub.box_lines_after, 0u) << "it reports nothing, having done nothing";

    ASSERT_EQ(BuildSubstitution("\x05" "B", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.box_lines_before, 1u) << "the chain kept the line it had drawn";
}

TEST(BoxBudget, ManyBoxesKeepTheirOwnCounts) {
    // More boxes than the budget tracks at once: the ones still being drawn
    // into keep their counts, and a box that comes back opens itself.
    TranslationMap m;
    m["A"] = "one line";
    m["B"] = std::string(translator_logic::MAX_LINES * translator_logic::WRAP_AT, 'x');
    BoxLineBudget budget;
    Substitution sub;
    for (uintptr_t box = 1; box <= 20; ++box) {
        ASSERT_EQ(BuildSubstitution("A\n", m, budget, box, sub),
                  SubstitutionStatus::Substituted);
        ASSERT_EQ(BuildSubstitution("\x05" "B", m, budget, box, sub),
                  SubstitutionStatus::Substituted);
        EXPECT_EQ(sub.box_lines_before, 1u) << "box " << box;
        EXPECT_EQ(sub.lines, MAX_LINES - 1) << "box " << box;
    }
}

TEST(BoxBudget, AChainedSegmentLeavesRoomForTheEnginesOwnIndent) {
    using translator_logic::CHAIN_INDENT;
    // The engine draws a chained line one column in from the margin, so a
    // segment that continues a box has one column less to fill than the
    // segment that opened it.
    const std::string full(SINGLE_LINE_MAX, 'x');
    TranslationMap m;
    m["A"] = "opening line";
    m["B"] = full;
    BoxLineBudget budget;
    Substitution sub;

    ASSERT_EQ(BuildSubstitution("A\n", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("\x05" "B", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    // One byte of prefix plus at most the narrowed width on the first line.
    const std::string first = sub.bytes.substr(1, sub.bytes.find('\n') - 1);
    EXPECT_LE(first.size() + CHAIN_INDENT, translator_logic::BOX_COLUMNS);

    // The same value in a box of its own uses the full width.
    budget.Clear();
    ASSERT_EQ(BuildSubstitution("B", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, full);
    EXPECT_EQ(sub.lines, 1u);
}

// --- kinsoku: where the engine may break and where it may not ---------
//
// The engine shifts a break when kinsoku forbids the character at a line
// boundary; cases are built long enough to show the cascade.

namespace {

// Full-width CP932 characters.  Only the last three are named by the engine's
// line-breaking tables.
const char kA[]       = "\x82\xA0";  // あ, forbidden nowhere
const char kFullStop[] = "\x81\x42";  // 。, may not start a line
const char kCloser[]  = "\x81\x76";  // 」, may not start a line
const char kOpener[]  = "\x81\x75";  // 「, may not end a line

std::string Repeat(const char* glyph, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) out += glyph;
    return out;
}

// The lines the engine draws for a buffer it renders itself.  Every outcome
// commits, so a miss reports what it charged the box.
size_t EngineLinesFor(const std::string& wire) {
    TranslationMap empty;
    BoxLineBudget budget;
    Substitution sub;
    BuildSubstitution(wire.c_str(), empty, budget, kDialogue, sub);
    return sub.lines;
}

// Full-width characters a line holds: two columns each.
size_t PerLine() { return translator_logic::BOX_COLUMNS / 2; }

}  // namespace

TEST(EngineLines, PlainFullWidthCharactersBreakOnWidthAlone) {
    EXPECT_EQ(EngineLinesFor(Repeat(kA, PerLine())), 1u);
    EXPECT_EQ(EngineLinesFor(Repeat(kA, PerLine() + 1)), 2u);
    EXPECT_EQ(EngineLinesFor(Repeat(kA, 2 * PerLine())), 2u);
}

TEST(EngineLines, PunctuationThatMayNotStartALineTakesItsCharacterWithIt) {
    // 。 would land at the head of the second line, so the character it
    // belongs to goes down with it and every line after shifts by one.  On
    // width alone this is two lines; the engine draws three.
    const std::string text =
        Repeat(kA, PerLine()) + kFullStop + Repeat(kA, PerLine() - 1);
    ASSERT_EQ(text.size() / 2, 2 * PerLine());
    EXPECT_EQ(EngineLinesFor(text), 3u);
}

TEST(EngineLines, ARunOfForbiddenPunctuationIsHeldTogether) {
    // 」。 is two of them, so three characters have to fit or none of them do.
    const std::string text = Repeat(kA, PerLine() - 1) + kCloser + kFullStop +
                             Repeat(kA, PerLine() - 1);
    EXPECT_EQ(EngineLinesFor(text), 3u);
}

TEST(EngineLines, AnOpenerKeepsTheCharacterAfterItCompany) {
    // 「 may not end a line, so it moves down with what it opens.
    const std::string text =
        Repeat(kA, PerLine() - 1) + kOpener + Repeat(kA, PerLine());
    ASSERT_EQ(text.size() / 2, 2 * PerLine());
    EXPECT_EQ(EngineLinesFor(text), 3u);
}

TEST(EngineLines, MarkupCostsNoColumnBecauseTheEngineDrawsNoneOfIt) {
    // The engine consumes "<r かな>" and draws none of it, so charging the box
    // for its bytes would fill the box with text nobody sees and cut the
    // English of the next segment for room that was there all along.
    const std::string tag = std::string("<r ") + "\x82\xA9" "\x82\xC8" + ">";
    const std::string base = Repeat(kA, PerLine());
    EXPECT_EQ(EngineLinesFor(tag + base + "</r>"), 1u);
    EXPECT_GT(tag.size() + base.size() + 4, translator_logic::BOX_COLUMNS);
}

TEST(BoxBudget, TheHistoryDrawsEverythingThePanelDrew) {
    // The backlog redraws values the player has already read in the panel, so a
    // value the panel showed in full and the backlog cut would lose text on a
    // second reading.  Both surfaces are given the same advance and therefore
    // the same number of lines, and a value that fills the panel comes back
    // whole and unmarked on both.
    ASSERT_EQ(MAX_LINES, HISTORY_WINDOW.lines);

    TranslationMap m;
    m["A"] = std::string(MAX_LINES * translator_logic::WRAP_AT, 'x');
    BoxLineBudget budget;
    Substitution sub;

    ASSERT_EQ(BuildSubstitution("A", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.lines, MAX_LINES) << "the panel draws all of it";
    EXPECT_FALSE(sub.truncated);
    const std::string in_the_panel = sub.bytes;

    BoxLineBudget history_budget;
    ASSERT_EQ(BuildSubstitution("A", m, history_budget, kDialogue,
                                HISTORY_WINDOW, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.lines, MAX_LINES);
    EXPECT_FALSE(sub.truncated);
    EXPECT_EQ(sub.bytes, in_the_panel);
}

TEST(BoxBudget, ASurfaceWithFewerLinesStillCutsAtAWordAndMarksIt) {
    // The cut-and-mark backstop is not the backlog's any more, but it is still
    // what every surface falls back on: whatever room a surface turns out to
    // have, a value past it is cut where a word ends and says so on screen.
    const translator_logic::Surface two_lines { 2, 0 };
    TranslationMap m;
    m["A"] = std::string(MAX_LINES * translator_logic::WRAP_AT, 'x');
    BoxLineBudget budget;
    Substitution sub;

    ASSERT_EQ(BuildSubstitution("A", m, budget, kDialogue, two_lines, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.lines, two_lines.lines);
    EXPECT_TRUE(sub.truncated);
    EXPECT_EQ(sub.bytes.substr(sub.bytes.size() - 3), "...");
}

TEST(DrawBudget, NoValueOfAnyShapeGetsPastTheBudgetItIsGiven) {
    // The budget is what stands between a substitution and a heap-corrupting
    // write, so it is tested as a property rather than on one example: every
    // length, prose that has spaces to break at and a single word that has
    // none, alone and inside a chain, and a value of nothing but two-byte
    // characters.
    const translator_logic::Surface surface { MAX_LINES, 176 };
    for (size_t n : { size_t(1), size_t(60), size_t(140), size_t(176),
                      size_t(210), size_t(400), size_t(4000) }) {
        std::string prose;
        while (prose.size() < n) prose += "abcdef ";
        prose.resize(n);
        const std::string word(n, 'x');
        std::string wide;
        while (wide.size() < n) wide += kA;
        wide.resize(n - (n % 2));

        for (const std::string& value : { prose, word, wide }) {
            if (value.empty()) continue;
            TranslationMap m;
            m["A"] = value;
            for (const char* wire : { "A", "\x05" "A",
                                      "A\n", "\x05" "A\n" }) {
                BoxLineBudget budget;
                Substitution sub;
                ASSERT_EQ(BuildSubstitution(wire, m, budget, kDialogue,
                                            surface, sub),
                          SubstitutionStatus::Substituted);
                EXPECT_LE(sub.bytes.size(), surface.max_bytes)
                    << "value of " << value.size() << " bytes on wire '"
                    << wire << "'";
                EXPECT_LE(sub.lines, surface.lines);
            }
        }
    }
}

TEST(DrawBudget, TheCutNeverSplitsATwoByteCharacter) {
    // A cut between a lead byte and its trail byte puts an unrelated glyph, or
    // a stray byte, on screen.  The marker is ASCII, so what has to be checked
    // is the text in front of it.
    std::string wide;
    while (wide.size() < 600) wide += kA;   // fullwidth JP text
    TranslationMap m;
    m["A"] = wide;
    BoxLineBudget budget;
    Substitution sub;
    const translator_logic::Surface surface { MAX_LINES, 176 };
    ASSERT_EQ(BuildSubstitution("A", m, budget, kDialogue, surface, sub),
              SubstitutionStatus::Substituted);
    ASSERT_LE(sub.bytes.size(), surface.max_bytes);

    size_t i = 0;
    size_t last_start = 0;
    while (i < sub.bytes.size()) {
        last_start = i;
        const unsigned char b = static_cast<unsigned char>(sub.bytes[i]);
        i += (translator_logic::IsCp932LeadByte(b) && i + 1 < sub.bytes.size())
             ? 2 : 1;
    }
    EXPECT_EQ(i, sub.bytes.size())
        << "the buffer ends inside a character that starts at " << last_start;
}

TEST(DrawBudget, ABudgetOfZeroIsNoBudgetAtAll) {
    // A caller that does not know how much room the render has says so with a
    // zero, and nothing is cut for it -- charging a value a limit nobody
    // measured is how English gets shortened for no reason.
    TranslationMap m;
    m["A"] = std::string(MAX_LINES * translator_logic::WRAP_AT, 'x');
    BoxLineBudget budget;
    Substitution sub;
    ASSERT_EQ(BuildSubstitution("A", m, budget, kDialogue, sub),
              SubstitutionStatus::Substituted);
    EXPECT_GT(sub.bytes.size(), 176u);
    EXPECT_FALSE(sub.truncated);
}

TEST(DrawBudget, ItScalesWithTheBufferTheEngineMeasured) {
    // The array the engine allocates holds four rectangles for every byte of
    // the buffer IT measured, so the room a substitution has is the source
    // line's length and not a constant.  The margin comes off the top.
    using translator_logic::DrawBudget;
    using translator_logic::RECTS_PER_WIRE_BYTE;
    using translator_logic::RECT_BUDGET_MARGIN;
    EXPECT_EQ(DrawBudget(38, RECTS_PER_WIRE_BYTE),
              38 * RECTS_PER_WIRE_BYTE - RECT_BUDGET_MARGIN);
    EXPECT_EQ(DrawBudget(38, 32u), 38 * 32 - RECT_BUDGET_MARGIN);

    // A short source line keeps most of its budget: a flat margin would be all
    // of it, and a speaker name on four bytes of script would come out as the
    // truncation marker.
    EXPECT_EQ(DrawBudget(4, RECTS_PER_WIRE_BYTE), 14u);   // 16 rectangles
    EXPECT_EQ(DrawBudget(4, 32u), 112u);                  // 128 rectangles
    EXPECT_GE(DrawBudget(1, RECTS_PER_WIRE_BYTE), 1u);
    EXPECT_GE(DrawBudget(0, RECTS_PER_WIRE_BYTE), 1u);

    // The budget is what stands between the longest speaker name the shipped
    // table carries and a heap-corrupting write on the name plate: "Father
    // (Priest)" draws 15 glyphs off four bytes of script, which the engine's
    // own array of 16 rectangles has no room to spare on.
    EXPECT_LT(DrawBudget(4, RECTS_PER_WIRE_BYTE), 15u);
    EXPECT_GT(DrawBudget(4, 32u), 15u);
}

TEST(EngineLines, KinsokuAppliesInsideARunOfHalfWidthCharactersToo) {
    using translator_logic::BOX_COLUMNS;
    // The engine tests one character at a time, so a half-width run that ends
    // in punctuation which may not open a line is broken one character early
    // -- inside the run, where nothing else would break it.  On width alone
    // this is two lines; the engine draws three, and a model that moved the
    // run whole would hand the box a line it does not have.
    const std::string text = std::string(BOX_COLUMNS, 'x') + kFullStop +
                             std::string(BOX_COLUMNS - 2, 'y');
    EXPECT_EQ(EngineLinesFor(text), 3u);

    // Without the punctuation the same widths take two, which is what makes
    // the case above the lookahead's doing and not the width's.
    const std::string plain = std::string(BOX_COLUMNS, 'x') +
                              std::string(BOX_COLUMNS, 'y');
    EXPECT_EQ(EngineLinesFor(plain), 2u);
}

TEST(EngineLines, AHalfWidthRunIsBrokenWhereItStopsFitting) {
    using translator_logic::BOX_COLUMNS;
    // The engine has no notion of a word: a run wider than the line is cut
    // where the line ends, not moved down whole.  It is why the English is
    // wrapped before the engine sees it, and why the count here is the
    // engine's rather than a word-wrapper's.
    EXPECT_EQ(EngineLinesFor(std::string(BOX_COLUMNS, 'x')), 1u);
    EXPECT_EQ(EngineLinesFor(std::string(BOX_COLUMNS + 1, 'x')), 2u);
    EXPECT_EQ(EngineLinesFor(std::string(3 * BOX_COLUMNS, 'x')), 3u);
}

TEST(EngineLines, ABracketTheEngineDrawsItselfStillCostsItsColumn) {
    // No '>' after it, so it is an ordinary character and takes a column.
    const std::string full(translator_logic::BOX_COLUMNS, 'j');
    EXPECT_EQ(EngineLinesFor(full), 1u);
    EXPECT_EQ(EngineLinesFor("<" + full), 2u);
}

// --- The surfaces nobody photographed ----------------------------------
//
// Nineteen font sizes appear in the corpus; only three are screen-measured.
// These pin the derivation for the other sixteen and gate against overflow.

// The derivation has to reproduce every count a screenshot already fixed.
TEST(UnmeasuredSurface, the_derived_geometry_reproduces_the_measured_one) {
    using translator_logic::ColumnsAtPitch;
    using translator_logic::LinesAtPitch;

    // The panel, measured: 71 columns at 14.0 px per half-width glyph, three
    // lines at the engine's own 42 px advance and four at the substituted 30.
    EXPECT_EQ(ColumnsAtPitch(42), translator_logic::BOX_COLUMNS);
    EXPECT_EQ(LinesAtPitch(42), 3u);
    EXPECT_EQ(LinesAtPitch(30), translator_logic::MAX_LINES);

    // The 52 px surface, photographed and measured at about 17.75 px per
    // column: about 56 columns, and two lines rather than four.
    EXPECT_NEAR(static_cast<double>(ColumnsAtPitch(52)), 56.0, 2.0);
    EXPECT_EQ(LinesAtPitch(52), 2u);
    // The 60 px one was never caught on screen; on the same arithmetic it is
    // narrower still and holds the same two lines.
    EXPECT_LT(ColumnsAtPitch(60), ColumnsAtPitch(52));
    EXPECT_EQ(LinesAtPitch(60), 2u);

    // Wider advances hold fewer columns and fewer lines, monotonically.
    for (size_t pitch = 21; pitch < 135; ++pitch) {
        EXPECT_GE(ColumnsAtPitch(pitch), ColumnsAtPitch(pitch + 1)) << pitch;
        EXPECT_GE(LinesAtPitch(pitch), LinesAtPitch(pitch + 1)) << pitch;
    }
    // 90 pt is the largest size the corpus asks for, and it leaves the panel a
    // single line -- nothing may be substituted there.
    EXPECT_EQ(LinesAtPitch(135), 1u);
}

// The backlog's entry is a slot the caller lays out, and the same arithmetic
// decides how much of it the advance uses.
TEST(UnmeasuredSurface, the_backlog_entry_holds_a_fourth_row_at_the_shorter_advance) {
    using translator_logic::HISTORY_BELOW_FIRST_TEXT_ROW_PX;
    using translator_logic::LinesInRoom;

    // The engine's own 40 px advance fills the entry with three text rows: the
    // third ends 40 + 40 + 25 = 105 px down, exactly on the clip.
    EXPECT_EQ(LinesInRoom(HISTORY_BELOW_FIRST_TEXT_ROW_PX, 0x28), 3u);
    // At the substituted advance the fourth row is whole inside the same clip,
    // and the budget the backlog is given matches it.
    EXPECT_EQ(LinesInRoom(HISTORY_BELOW_FIRST_TEXT_ROW_PX, 26),
              translator_logic::HISTORY_MAX_LINES);
    EXPECT_EQ(HISTORY_WINDOW.lines, translator_logic::MAX_LINES)
        << "the backlog must not cut what the panel drew in full";
    // One pixel wider and the fourth row is the one the clip cuts in half --
    // which is what a 30 px advance did on screen before this was measured.
    EXPECT_EQ(LinesInRoom(HISTORY_BELOW_FIRST_TEXT_ROW_PX, 27), 3u);
    EXPECT_EQ(LinesInRoom(HISTORY_BELOW_FIRST_TEXT_ROW_PX, 30), 3u);
    // A fifth row does not fit and is not claimed.
    EXPECT_LT(LinesInRoom(HISTORY_BELOW_FIRST_TEXT_ROW_PX, 26), 5u);
}

TEST(UnmeasuredSurface, only_one_short_line_with_no_break_of_its_own_is_taken) {
    using translator_logic::ColumnsAtPitch;
    using translator_logic::FitsUnmeasuredSurface;
    const size_t pitch = 52;
    const size_t columns = ColumnsAtPitch(pitch);

    EXPECT_TRUE(FitsUnmeasuredSurface(std::string(columns, 'x'), pitch));
    EXPECT_FALSE(FitsUnmeasuredSurface(std::string(columns + 1, 'x'), pitch));

    // The control prefix and the trailing break are the engine's chain
    // protocol, not content, and neither is charged for.
    EXPECT_TRUE(FitsUnmeasuredSurface("\x05" + std::string(columns, 'x') + "\n",
                                      pitch));
    // A break of our own would draw a line this surface may not have.
    EXPECT_FALSE(FitsUnmeasuredSurface("one\ntwo", pitch));
    EXPECT_FALSE(FitsUnmeasuredSurface("", pitch));
    EXPECT_FALSE(FitsUnmeasuredSurface("\x05\n", pitch));

    // A surface with no second line behind the one it would draw takes
    // nothing at all: a width the engine disagrees with by one column would
    // cost the whole value.
    EXPECT_EQ(translator_logic::LinesAtPitch(135), 1u);
    EXPECT_FALSE(FitsUnmeasuredSurface("short", 135));
}

// A value that fits goes through the ordinary substitution path and comes back
// as one unmarked line.  A segment that chains does not: the surface has one
// line to give, so the wrap marks it the moment the wire says another segment
// follows, and the hook then leaves the whole call to the engine.
TEST(UnmeasuredSurface, one_line_is_taken_and_a_chained_segment_is_not) {
    const size_t pitch = 52;
    translator_logic::Surface surface;
    surface.lines = 1;

    TranslationMap map = MapOf("shout", "Fine! I'll turn you down cheerfully!");
    BoxLineBudget budget;
    Substitution sub;
    ASSERT_EQ(BuildSubstitution("shout", map, budget, 1, surface, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.lines, 1u);
    EXPECT_FALSE(sub.truncated);
    EXPECT_FALSE(sub.more_follows);
    EXPECT_TRUE(translator_logic::FitsUnmeasuredSurface(sub.bytes, pitch));

    BoxLineBudget chained;
    Substitution more;
    ASSERT_EQ(BuildSubstitution("shout\n", map, chained, 1, surface, more),
              SubstitutionStatus::Substituted);
    EXPECT_TRUE(more.more_follows);
    EXPECT_TRUE(more.truncated)
        << "one line and another segment coming is a line that loses text";
}

// --- Positioned message boxes (yuzu_2g SET_CURSOR + op140) ------------
//
// yuzu_2g's iMessage*/Message* helpers open boxes with a dummy fullwidth space
// ("\x81\x40"), compute SET_CURSOR (0x146) coordinates from Japanese byte
// length and the stock 42 px row pitch, and emit segments with 0x05 prefixes
// and no trailing 0x0A.

TEST(PositionedBox, DummyFullwidthSpaceOpensPositionedBoxAndDrawsNothing) {
    using translator_logic::DIALOGUE_PANEL;
    TranslationMap m = MapOf("SEG1", "First positioned line.");
    BoxLineBudget budget;
    Substitution sub;
    const int box_rect[4] = { 48, 24, 1083, 151 };
    int cursor_xy[2] = { 48, 24 };

    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, cursor_xy, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_TRUE(sub.bytes.empty());
    EXPECT_EQ(sub.lines, 0u);
    EXPECT_TRUE(sub.more_follows);
    EXPECT_EQ(sub.box_lines_before, 0u);
    EXPECT_EQ(sub.box_lines_after, 0u);
    EXPECT_EQ(cursor_xy[0], 48);
    EXPECT_EQ(cursor_xy[1], 24);
}

TEST(PositionedBox, Screenshot1ThreeSegmentPositionedBoxLayout) {
    using translator_logic::DIALOGUE_PANEL;
    // 03_zypressen_02 [146..148]: iMessageLeftIg(y=0) -> MessageRightIg(y=1)
    // -> MessageCenter(y=2).
    const std::string kJp146 = "JP_146_GAZELLE_EYES";
    const std::string kJp147 = "JP_147_TO_THE_LION";
    const std::string kJp148 = "JP_148_SACK_OF_FLESH";

    TranslationMap m;
    m[kJp146] = "The gazelle's endearing eyes, its beautiful coat--";
    m[kJp147] = "to the lion, none of it matters much.";
    m[kJp148] = "Looks mean nothing to him; a gazelle is only a sack stuffed "
                "with flesh and blood.";

    BoxLineBudget budget;
    Substitution sub;
    const int box_rect[4] = { 48, 24, 1083, 151 };

    // Dummy opener from iMessageLeftIg.
    int c0[2] = { 48, 24 };
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c0, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_TRUE(sub.bytes.empty());

    // [146]: SET_CURSOR(0, 0) -> (48, 24).
    int c1[2] = { 48, 24 };
    ASSERT_EQ(BuildSubstitution(("\x05" + kJp146).c_str(), m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c1, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, "The gazelle's endearing eyes, its beautiful coat--");
    EXPECT_EQ(c1[0], 48);
    EXPECT_EQ(c1[1], 24);
    EXPECT_EQ(sub.lines, 1u);
    EXPECT_EQ(sub.box_lines_after, 1u);
    EXPECT_FALSE(sub.truncated);

    // [147]: SET_CURSOR(1, 18) -> (552, 66), joins the open first line at
    // col 51 (x = 48 + 51 * 14 = 762, y = 24) and wraps onto line 2 (y = 54).
    int c2[2] = { 552, 66 };
    ASSERT_EQ(BuildSubstitution(("\x05" + kJp147).c_str(), m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c2, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, "to the lion, none of\nit matters much.");
    EXPECT_EQ(c2[0], 762);
    EXPECT_EQ(c2[1], 24);
    EXPECT_EQ(sub.lines, 1u);
    EXPECT_EQ(sub.box_lines_before, 1u);
    EXPECT_EQ(sub.box_lines_after, 2u);
    EXPECT_FALSE(sub.truncated);

    // [148]: SET_CURSOR(2, 5) -> (188, 108), joins line 2 at col 17
    // (x = 48 + 17 * 14 = 286, y = 54) and wraps onto line 3 (y = 84).
    int c3[2] = { 188, 108 };
    ASSERT_EQ(BuildSubstitution(("\x05" + kJp148).c_str(), m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c3, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes,
              "Looks mean nothing to him; a gazelle is only a sack\n"
              "stuffed with flesh and blood.");
    EXPECT_EQ(c3[0], 286);
    EXPECT_EQ(c3[1], 54);
    EXPECT_EQ(sub.lines, 1u);
    EXPECT_EQ(sub.box_lines_before, 2u);
    EXPECT_EQ(sub.box_lines_after, 3u);
    EXPECT_FALSE(sub.truncated);
    EXPECT_FALSE(sub.dropped);
}

TEST(PositionedBox, Screenshot2TwoSegmentPositionedBoxLayout) {
    using translator_logic::DIALOGUE_PANEL;
    // 03_zypressen_02 [160..161]: iMessageLeftIg(y=0) -> MessageRight(y=2).
    const std::string kJp160 = "JP_160_CARNIVORE";
    const std::string kJp161 = "JP_161_OWN_SEX";

    TranslationMap m;
    m[kJp160] = "Just as a carnivore devours the flesh of weaker animals as "
                "if by necessity,";
    m[kJp161] = "so too did I love my own sex.";

    BoxLineBudget budget;
    Substitution sub;
    const int box_rect[4] = { 48, 24, 1083, 151 };

    int c0[2] = { 48, 24 };
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c0, box_rect, 30),
              SubstitutionStatus::Substituted);

    // [160]: wraps to 2 lines at (48, 24) and (48, 54), ending at col 10.
    int c1[2] = { 48, 24 };
    ASSERT_EQ(BuildSubstitution(("\x05" + kJp160).c_str(), m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c1, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes,
              "Just as a carnivore devours the flesh of weaker animals as if "
              "by\nnecessity,");
    EXPECT_EQ(c1[0], 48);
    EXPECT_EQ(c1[1], 24);
    EXPECT_EQ(sub.lines, 2u);
    EXPECT_EQ(sub.box_lines_after, 2u);
    EXPECT_FALSE(sub.truncated);

    // [161]: SET_CURSOR(2, 25) -> (748, 108), joins line 2 at col 11
    // (x = 48 + 11 * 14 = 202, y = 54) so the whole sentence fits in 2 lines.
    int c2[2] = { 748, 108 };
    ASSERT_EQ(BuildSubstitution(("\x05" + kJp161).c_str(), m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c2, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, "so too did I love my own sex.");
    EXPECT_EQ(c2[0], 202);
    EXPECT_EQ(c2[1], 54);
    EXPECT_EQ(sub.lines, 0u);
    EXPECT_EQ(sub.box_lines_before, 2u);
    EXPECT_EQ(sub.box_lines_after, 2u);
    EXPECT_FALSE(sub.truncated);
}

TEST(PositionedBox, Screenshot3SingleSegmentCenteredBoxLayout) {
    using translator_logic::DIALOGUE_PANEL;
    // 03_zypressen_02 [194]: iMessageCenter(y=1).
    const std::string kJp194 = "JP_194_RINA_CHILD";

    TranslationMap m;
    m[kJp194] = "It was when both she and I were still small children that I "
                "first laid eyes on Rina.";

    BoxLineBudget budget;
    Substitution sub;
    const int box_rect[4] = { 48, 24, 1083, 151 };

    int c0[2] = { 48, 24 };
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c0, box_rect, 30),
              SubstitutionStatus::Substituted);

    // [194]: SET_CURSOR(1, 8) -> (272, 66), normalized to (48, 24) so it wraps
    // into 2 lines instead of 3 and never clips at bottom_y = 151.
    int c1[2] = { 272, 66 };
    ASSERT_EQ(BuildSubstitution(("\x05" + kJp194).c_str(), m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c1, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes,
              "It was when both she and I were still small children that I "
              "first laid\neyes on Rina.");
    EXPECT_EQ(c1[0], 48);
    EXPECT_EQ(c1[1], 24);
    EXPECT_EQ(sub.lines, 2u);
    EXPECT_EQ(sub.box_lines_after, 2u);
    EXPECT_FALSE(sub.truncated);
}

TEST(PositionedBox, WindowRedrawWithoutControlPrefixReproducesLayout) {
    using translator_logic::DIALOGUE_PANEL;
    // msgwndctrl._bp redraws a positioned box by re-issuing "\x81\x40" and
    // each segment WITHOUT the 0x05 prefix that scrmsg._bp prepended.
    TranslationMap m;
    m["S1"] = "The gazelle's endearing eyes, its beautiful coat--";
    m["S2"] = "to the lion, none of it matters much.";
    m["S3"] = "Looks mean nothing to him; a gazelle is only a sack stuffed "
              "with flesh and blood.";

    BoxLineBudget budget;
    Substitution sub;
    const int box_rect[4] = { 48, 24, 1083, 151 };

    int c0[2] = { 48, 24 };
    int c1[2] = { 48, 24 };
    int c2[2] = { 552, 66 };
    int c3[2] = { 188, 108 };
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c0, box_rect, 30),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("S1", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c1, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(c1[0], 48);
    EXPECT_EQ(c1[1], 24);

    ASSERT_EQ(BuildSubstitution("S2", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c2, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(c2[0], 762);
    EXPECT_EQ(c2[1], 24);

    ASSERT_EQ(BuildSubstitution("S3", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c3, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(c3[0], 286);
    EXPECT_EQ(c3[1], 54);
    EXPECT_EQ(sub.box_lines_after, 3u);
    EXPECT_FALSE(sub.truncated);
}

TEST(PositionedBox, SameRowAndFiveSegmentBoxesJoinOpenLineWithoutDropping) {
    using translator_logic::DIALOGUE_PANEL;
    // 03_zypressen_02 5-call box: two identical "壊せ、" calls on the same
    // script row_y = 2 at different col_x, followed by a 5th segment.
    TranslationMap m;
    m["S1"] = "Such a delusion-like thought reared its head within me,";
    m["S2"] = "urging me simply to--";
    m["S3"] = "destroy her,";
    m["S5"] = "and began driving me onward.";

    BoxLineBudget budget;
    Substitution sub;
    const int box_rect[4] = { 48, 24, 1083, 151 };

    int c0[2] = { 48, 24 };
    int c1[2] = { 48, 24 };
    int c2[2] = { 216, 66 };
    int c3[2] = { 384, 108 };
    int c4[2] = { 496, 108 };  // same row_y = 108, different col_x
    int c5[2] = { 552, 108 };  // same row_y = 108

    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c0, box_rect, 30),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("\x05S1", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c1, box_rect, 30),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("\x05S2", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c2, box_rect, 30),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("\x05S3", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c3, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_GT(c3[0], 48);
    EXPECT_EQ(c3[1], 54);

    // Second "S3" at a different x on the same row joins the open line rather
    // than re-rendering over the first "destroy her,".
    ASSERT_EQ(BuildSubstitution("\x05S3", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c4, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_GT(c4[0], c3[0]);
    EXPECT_EQ(c4[1], 54);
    EXPECT_FALSE(sub.dropped);

    ASSERT_EQ(BuildSubstitution("\x05S5", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c5, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_FALSE(sub.dropped);
    EXPECT_FALSE(sub.truncated);
    EXPECT_EQ(sub.box_lines_after, 2u);
}

TEST(PositionedBox, PremonitionScreenshotsFlowAsContinuousParagraphs) {
    using translator_logic::DIALOGUE_PANEL;
    // Box 81 (04:56) and Box 92 (06:50) in 03_zypressen_02: 3-segment boxes
    // that previously broke mid-sentence onto separate lines.
    TranslationMap m;
    m["B81_1"] = "A boy said, \"What? Bathroom? I'll come too,\"";
    m["B81_2"] = "and then, \"Ah, right, I forgot -- technically Kawachino "
                 "uses a different bathroom than us, huh?\"";
    m["B81_3"] = "and so I punched him.";

    m["B92_1"] = "But we had cleverly kept the truth to ourselves";
    m["B92_2"] = "(the official story became that Tsuyoshi had fallen into a "
                 "sewer),";
    m["B92_3"] = "and the other side, out of their own humiliation, could not "
                 "bring themselves to speak of it either.";

    BoxLineBudget budget;
    Substitution sub;
    const int box_rect[4] = { 48, 24, 1083, 151 };

    // Box 81: flows across 3 lines instead of 4 ragged lines.
    int c0[2] = { 48, 24 }, c1[2] = { 48, 24 }, c2[2] = { 48, 66 },
        c3[2] = { 888, 108 };
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c0, box_rect, 30),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("\x05" "B81_1", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c1, box_rect, 30),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("\x05" "B81_2", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c2, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(c2[0], 48 + 45 * 14);
    EXPECT_EQ(c2[1], 24);
    EXPECT_EQ(sub.bytes,
              "and then, \"Ah, right, I\n"
              "forgot -- technically Kawachino uses a different bathroom than "
              "us,\nhuh?\"");
    ASSERT_EQ(BuildSubstitution("\x05" "B81_3", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c3, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(c3[0], 48 + 6 * 14);
    EXPECT_EQ(c3[1], 84);
    EXPECT_EQ(sub.bytes, "and so I punched him.");
    EXPECT_EQ(sub.box_lines_after, 3u);
    EXPECT_FALSE(sub.truncated);

    // Box 92: flows across 4 lines continuously.
    int d0[2] = { 48, 24 }, d1[2] = { 48, 24 }, d2[2] = { 244, 66 },
        d3[2] = { 328, 108 };
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, d0, box_rect, 30),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("\x05" "B92_1", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, d1, box_rect, 30),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("\x05" "B92_2", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, d2, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(d2[0], 48 + 48 * 14);
    EXPECT_EQ(d2[1], 24);
    ASSERT_EQ(BuildSubstitution("\x05" "B92_3", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, d3, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(d3[0], 48 + 47 * 14);
    EXPECT_EQ(d3[1], 54);
    EXPECT_EQ(sub.box_lines_after, 4u);
    EXPECT_FALSE(sub.truncated);
}

TEST(PositionedBox, LongThreeSegmentBoxesFlowWithoutTruncation) {
    using translator_logic::DIALOGUE_PANEL;
    // Box 6 in 03_zypressen_02: Segment 1 wraps to Line 2 with a 6-char tail
    // ("start,").  Joining Line 2 allows all 3 segments to fit in 4 lines
    // without truncating Segment 3.
    TranslationMap m;
    m["B6_1"] = "As if my thoughts had been heading toward a single point "
                "from the very start,";
    m["B6_2"] = "the blank pure-white paper had, before I knew it, been dyed "
                "pitch-black,";
    m["B6_3"] = "converging upon a single answer.";

    BoxLineBudget budget;
    Substitution sub;
    const int box_rect[4] = { 48, 24, 1083, 151 };

    int c0[2] = { 48, 24 }, c1[2] = { 48, 24 }, c2[2] = { 216, 66 },
        c3[2] = { 664, 108 };
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c0, box_rect, 30),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("\x05" "B6_1", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c1, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_FALSE(sub.truncated);
    ASSERT_EQ(BuildSubstitution("\x05" "B6_2", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c2, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_FALSE(sub.truncated);
    ASSERT_EQ(BuildSubstitution("\x05" "B6_3", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c3, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_FALSE(sub.truncated);
    EXPECT_FALSE(sub.dropped);
    EXPECT_EQ(sub.bytes, "converging upon a single answer.");
    EXPECT_EQ(sub.box_lines_after, 3u);
}

TEST(PositionedBox, BacklogHistoryStripsLeadingFullwidthSpaceAndJoinsSegments) {
    // logwnd._bp concatenates seg[0..k] directly ("\x81\x40" + jp1 + jp2...).
    const std::string kJp1 = "\x83\x4B\x83\x5B\x83\x8B";  // ガゼル
    const std::string kJp2 = "\x83\x89\x83\x43\x83\x49";  // ライオ
    const std::string kJp3 = "\x8A\x4F\x8C\xA9\x82\xC8";  // 外見な

    TranslationMap m;
    m[kJp1] = "The gazelle's endearing eyes, its beautiful coat--";
    m[kJp2] = "to the lion, none of it matters much.";
    m[kJp3] = "Looks mean nothing to him; a gazelle is only a sack stuffed "
              "with flesh and blood.";

    BoxLineBudget budget;
    Substitution sub;

    // Single-segment iMessage* in backlog ("\x81\x40" + kJp1).
    ASSERT_EQ(BuildSubstitution(("\x81\x40" + kJp1).c_str(), m, budget,
                                kDialogue, HISTORY_WINDOW, sub),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.bytes, "The gazelle's endearing eyes, its beautiful coat--");
    EXPECT_FALSE(sub.truncated);

    // Multi-segment iMessage* in backlog ("\x81\x40" + kJp1 + kJp2 + kJp3).
    ASSERT_EQ(BuildSubstitution(("\x81\x40" + kJp1 + kJp2 + kJp3).c_str(),
                                m, budget, kDialogue, HISTORY_WINDOW, sub),
              SubstitutionStatus::Substituted);
    EXPECT_FALSE(sub.truncated);
    EXPECT_LE(sub.lines, HISTORY_WINDOW.lines);
    EXPECT_NE(sub.bytes.find("endearing eyes"), std::string::npos);
    EXPECT_NE(sub.bytes.find("to the lion"), std::string::npos);
    EXPECT_NE(sub.bytes.find("flesh and blood."), std::string::npos);
}

TEST(PositionedBox, NoClearContinuationDoesNotOverwritePreviousSentences) {
    using translator_logic::DIALOGUE_PANEL;
    // 03_zypressen_02 ("Premonition II"): *NC ("No Clear") helpers wait for a
    // click without clearing the message box, emit "\n" on p4=0xEFE958, and
    // then the following iMessage* helper emits "\x81\x40" with cursor_xy
    // still below top_y (e.g. (48, 54) or (48, 84)).  It must continue the
    // open box rather than resetting to (48, 24) and overwriting Line 1.
    TranslationMap m;
    // Screenshot 1 (24:31, calls [281..283]):
    m["NC1_1"] = "But,";
    m["NC1_2"] = "pivoting on that twisted right foot, almost simultaneously "
                 "I swept his legs out with my left,";
    m["NC1_3"] = "and pulled you down on top of myself.";
    // Screenshot 2 (26:13, calls [293..295]):
    m["NC2_1"] = "All I wanted was to not run with anyone, not get tangled up "
                 "with anyone,";
    m["NC2_2"] = "to be alone -- and yet someone was always tangling "
                 "themselves up with me.";
    m["NC2_3"] = "I had never imagined that simply running at my own pace "
                 "could be this hard.";

    BoxLineBudget budget;
    Substitution sub;
    const int box_rect[4] = {48, 24, 1024, 150};
    const uintptr_t kNcNewlineBox = 0xEFE958;

    // --- Screenshot 1: iMessageLeftIg -> MessageLeftNC -> iMessageRight ---
    int d1[2] = {48, 24};
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, d1, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(d1[0], 48);
    EXPECT_EQ(d1[1], 24);
    EXPECT_EQ(sub.box_lines_after, 0u);

    int c1[2] = {48, 66};
    ASSERT_EQ(BuildSubstitution("\x05" "NC1_1", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c1, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(c1[0], 48);
    EXPECT_EQ(c1[1], 24);
    EXPECT_EQ(sub.bytes, "But,");
    EXPECT_EQ(sub.box_lines_after, 1u);

    int c2[2] = {132, 66};
    ASSERT_EQ(BuildSubstitution("\x05" "NC1_2", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c2, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(c2[0], 48 + (4 + 1) * 14);
    EXPECT_EQ(c2[1], 24);
    EXPECT_EQ(sub.box_lines_after, 2u);
    EXPECT_FALSE(sub.truncated);

    // Player clicks after MessageLeftNC: engine emits "\n" on 0xEFE958, then
    // iMessageRight emits "\x81\x40" on kDialogue with cursor_xy=(48, 84).
    ASSERT_EQ(BuildSubstitution("\n", m, budget, kNcNewlineBox,
                                DIALOGUE_PANEL, sub, nullptr, box_rect, 30),
              SubstitutionStatus::EmptyKey);

    int d2[2] = {48, 84};
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, d2, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(d2[0], 48);
    EXPECT_EQ(d2[1], 84) << "must not reset cursor_xy to (48, 24)";
    EXPECT_EQ(sub.box_lines_before, 2u);
    EXPECT_EQ(sub.box_lines_after, 2u);

    int c3[2] = {384, 84};
    ASSERT_EQ(BuildSubstitution("\x05" "NC1_3", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c3, box_rect, 30),
              SubstitutionStatus::Substituted);
    // Joins open Line 2 ("his legs out with my left," = 26 cols) at col 27:
    EXPECT_EQ(c3[0], 48 + (26 + 1) * 14);
    EXPECT_EQ(c3[1], 54);
    EXPECT_EQ(sub.bytes, "and pulled you down on top of myself.");
    EXPECT_EQ(sub.box_lines_after, 2u);
    EXPECT_FALSE(sub.truncated);

    // --- Screenshot 2: iMessageCenterNC -> iMessageCenterNC -> iMessageCenter
    int s2_d1[2] = {48, 24};
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, s2_d1, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(s2_d1[1], 24) << "fresh cleared box at (48, 24) resets to 0";
    EXPECT_EQ(sub.box_lines_after, 0u);

    int s2_c1[2] = {216, 66};
    ASSERT_EQ(BuildSubstitution("\x05" "NC2_1", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, s2_c1, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(s2_c1[0], 48);
    EXPECT_EQ(s2_c1[1], 24);
    EXPECT_EQ(sub.box_lines_after, 2u);
    EXPECT_FALSE(sub.truncated);

    ASSERT_EQ(BuildSubstitution("\n", m, budget, kNcNewlineBox,
                                DIALOGUE_PANEL, sub, nullptr, box_rect, 30),
              SubstitutionStatus::EmptyKey);

    int s2_d2[2] = {48, 84};
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, s2_d2, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(s2_d2[1], 84);
    EXPECT_EQ(sub.box_lines_after, 2u);

    int s2_c2[2] = {132, 108};
    ASSERT_EQ(BuildSubstitution("\x05" "NC2_2", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, s2_c2, box_rect, 30),
              SubstitutionStatus::Substituted);
    // Joins open Line 2 ("anyone," = 7 cols) at col 8:
    EXPECT_EQ(s2_c2[0], 48 + (7 + 1) * 14);
    EXPECT_EQ(s2_c2[1], 54);
    EXPECT_EQ(sub.box_lines_after, 3u);
    EXPECT_FALSE(sub.truncated);

    ASSERT_EQ(BuildSubstitution("\n", m, budget, kNcNewlineBox,
                                DIALOGUE_PANEL, sub, nullptr, box_rect, 30),
              SubstitutionStatus::EmptyKey);

    int s2_d3[2] = {48, 84};
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, s2_d3, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(s2_d3[1], 114);
    EXPECT_EQ(sub.box_lines_after, 3u);

    int s2_c3[2] = {104, 150};
    ASSERT_EQ(BuildSubstitution("\x05" "NC2_3", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, s2_c3, box_rect, 30),
              SubstitutionStatus::Substituted);
    // Joins open Line 3 ("up with me." = 11 cols) at col 12:
    EXPECT_EQ(s2_c3[0], 48 + (11 + 1) * 14);
    EXPECT_EQ(s2_c3[1], 84);
    EXPECT_EQ(sub.box_lines_after, 4u);
    EXPECT_FALSE(sub.truncated);
    EXPECT_FALSE(sub.dropped);
}

TEST(PositionedBox, SaveLoadPreviewResetsMidNoClearBoxWhenNoNewlineFollows) {
    using translator_logic::DIALOGUE_PANEL;
    using translator_logic::Surface;
    // Loading a save created on the segment after *NC (HOOK[161..164] and
    // HOOK[393..396]) renders 1-line save-slot previews on a derived surface
    // (Surface{1, false}) and then re-executes iMessage* ("\x81\x40" at
    // saved cursor_xy=(48, 54)) on a freshly cleared screen WITHOUT the "\n"
    // no-clear click call.  That load must start at (48, 24), whereas
    // cancelling the Save/Load menu and clicking through *NC (which emits
    // "\n") must continue the open box.
    TranslationMap m;
    m["SEG1"] = "I thought that if I couldn't make this girl mine,";
    m["SEG2"] = "then I wanted, rather, to destroy her.";

    BoxLineBudget budget;
    Substitution sub;
    const int box_rect[4] = {48, 24, 1024, 150};
    const Surface kSaveSlotPreview = {1, false};
    const uintptr_t kPreviewBox = 0xEFEC00;
    const uintptr_t kNcNewlineBox = 0xEFE958;

    // 1. Open box with SEG1 (*NC).
    int d1[2] = {48, 24};
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, d1, box_rect, 30),
              SubstitutionStatus::Substituted);
    int c1[2] = {244, 24};
    ASSERT_EQ(BuildSubstitution("\x05" "SEG1", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c1, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.box_lines_after, 1u);

    // 2. Player opens Save/Load menu (renders 1-line preview) and cancels back
    //    to the game, then clicks to advance past *NC (emits "\n").
    ASSERT_EQ(BuildSubstitution("\x81\x40" "SEG2", m, budget, kPreviewBox,
                                kSaveSlotPreview, sub),
              SubstitutionStatus::Substituted);
    ASSERT_EQ(BuildSubstitution("\n", m, budget, kNcNewlineBox,
                                DIALOGUE_PANEL, sub, nullptr, box_rect, 30),
              SubstitutionStatus::EmptyKey);

    int d2[2] = {48, 54};
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, d2, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(d2[1], 54) << "cancelled menu + *NC click continues open box";
    EXPECT_EQ(sub.box_lines_after, 1u);

    int c2[2] = {48, 66};
    ASSERT_EQ(BuildSubstitution("\x05" "SEG2", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c2, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(sub.box_lines_after, 2u);

    // 3. Player opens Load menu (renders 1-line preview) and loads the save
    //    at SEG2: "\x81\x40" arrives at saved cursor_xy=(48, 54) with NO "\n".
    ASSERT_EQ(BuildSubstitution("\x81\x40" "SEG2", m, budget, kPreviewBox,
                                kSaveSlotPreview, sub),
              SubstitutionStatus::Substituted);

    int d_load[2] = {48, 54};
    ASSERT_EQ(BuildSubstitution("\x81\x40", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, d_load, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(d_load[0], 48);
    EXPECT_EQ(d_load[1], 24) << "save load without *NC newline resets to top";
    EXPECT_EQ(sub.box_lines_after, 0u);

    int c_load[2] = {48, 66};
    ASSERT_EQ(BuildSubstitution("\x05" "SEG2", m, budget, kDialogue,
                                DIALOGUE_PANEL, sub, c_load, box_rect, 30),
              SubstitutionStatus::Substituted);
    EXPECT_EQ(c_load[0], 48);
    EXPECT_EQ(c_load[1], 24);
    EXPECT_EQ(sub.box_lines_after, 1u);
}
