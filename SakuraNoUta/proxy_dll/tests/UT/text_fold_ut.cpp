// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT -- EnglishToCp932: fold non-ASCII English to halfwidth ASCII (or report
// unmappable characters).  Sister file cp932_ut.cpp covers the raw codec.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using translator_logic::EnglishToCp932;
using translator_logic::ParseTsvBuffer;
using translator_logic::TranslationMap;
using translator_logic::UnmappableChar;

namespace {

// UTF-8 for one code point, so the tests read as characters rather than
// as byte soup.
std::string U(unsigned cp) {
    std::string s;
    if (cp < 0x80) { s += static_cast<char>(cp); return s; }
    if (cp < 0x800) {
        s += static_cast<char>(0xC0u | (cp >> 6));
        s += static_cast<char>(0x80u | (cp & 0x3Fu));
        return s;
    }
    if (cp < 0x10000) {
        s += static_cast<char>(0xE0u | (cp >> 12));
        s += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        s += static_cast<char>(0x80u | (cp & 0x3Fu));
        return s;
    }
    s += static_cast<char>(0xF0u | (cp >> 18));
    s += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
    s += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
    s += static_cast<char>(0x80u | (cp & 0x3Fu));
    return s;
}

bool IsAscii(const std::string& s) {
    for (char c : s)
        if (static_cast<unsigned char>(c) >= 0x80) return false;
    return true;
}

}  // namespace

TEST(EnglishFold, AsciiIsUntouched) {
    const std::string s = "He said \"no\" -- and meant it; 100% of the time.";
    EXPECT_EQ(EnglishToCp932(s), s);
}

TEST(EnglishFold, TheEmDashBecomesTwoHyphens) {
    EXPECT_EQ(EnglishToCp932("a" + U(0x2014) + "b"), "a--b");
}

TEST(EnglishFold, TheEnDashBecomesOneHyphen) {
    EXPECT_EQ(EnglishToCp932("1" + U(0x2013) + "9"), "1-9");
}

TEST(EnglishFold, CurlySingleQuotesBecomeApostrophes) {
    EXPECT_EQ(EnglishToCp932(U(0x2018) + "yes" + U(0x2019)), "'yes'");
    EXPECT_EQ(EnglishToCp932("Naoya" + U(0x2019) + "s"), "Naoya's");
}

TEST(EnglishFold, CurlyDoubleQuotesBecomeStraightQuotes) {
    EXPECT_EQ(EnglishToCp932(U(0x201C) + "Rin" + U(0x201D)), "\"Rin\"");
}

TEST(EnglishFold, TheEllipsisBecomesThreeDots) {
    EXPECT_EQ(EnglishToCp932("well" + U(0x2026)), "well...");
}

TEST(EnglishFold, TheNoBreakSpaceBecomesAnOrdinarySpace) {
    EXPECT_EQ(EnglishToCp932("a" + U(0x00A0) + "b"), "a b");
}

TEST(EnglishFold, AccentedLatinLettersLoseTheirAccents) {
    EXPECT_EQ(EnglishToCp932(U(0x00E9) + "clair"), "eclair");
    EXPECT_EQ(EnglishToCp932("na" + U(0x00EF) + "ve"), "naive");
    EXPECT_EQ(EnglishToCp932("Gr" + U(0x00FC) + U(0x00DF) + "e"), "Grusse");
    EXPECT_EQ(EnglishToCp932(U(0x00C6) + "on"), "AEon");
    EXPECT_EQ(EnglishToCp932("h" + U(0x0153) + "uvre"), "hoeuvre");
}

TEST(EnglishFold, TheIdeographicSpaceIsDropped) {
    // The Japanese source indents a paragraph with U+3000; English does not,
    // and the fullwidth blank it converts to is a two-column gap.
    EXPECT_EQ(EnglishToCp932(U(0x3000) + "The wind rose."), "The wind rose.");
    EXPECT_EQ(EnglishToCp932("a" + U(0x3000) + "b"), "ab");
}

TEST(EnglishFold, FullwidthAsciiComesBackHalfwidth) {
    EXPECT_EQ(EnglishToCp932(U(0xFF21) + U(0xFF42) + U(0xFF01)), "Ab!");
    EXPECT_EQ(EnglishToCp932(U(0x3001) + U(0x3002)), ",.");
}

TEST(EnglishFold, AFoldedValueIsPureAscii) {
    // Everything the folds cover has to leave nothing wide behind, because
    // the wrapper budgets in bytes and the box in columns.
    const std::string worst =
        U(0x3000) + U(0x201C) + "Caf" + U(0x00E9) + U(0x2019) + "s " +
        U(0x2014) + " na" + U(0x00EF) + "ve, se" + U(0x00F1) + "or" +
        U(0x2026) + U(0x201D) + U(0xFF01);
    const std::string folded = EnglishToCp932(worst);
    EXPECT_TRUE(IsAscii(folded)) << folded;
    EXPECT_EQ(folded, "\"Cafe's -- naive, senor...\"!");
}

TEST(EnglishFold, NothingEverFoldsToAnAngleBracket) {
    // The guillemets are the characters that would tempt a fold into one.
    EXPECT_EQ(EnglishToCp932(U(0x00AB) + "x" + U(0x00BB)), "\"x\"");
    EXPECT_EQ(EnglishToCp932(U(0x2039) + "x" + U(0x203A)), "'x'");
}

TEST(EnglishFold, AnEmptyValueStaysEmpty) {
    std::vector<unsigned> lost;
    EXPECT_EQ(EnglishToCp932(std::string(), &lost), "");
    EXPECT_TRUE(lost.empty());
}

TEST(EnglishFold, NothingIsReportedWhenEverythingHasAForm) {
    std::vector<unsigned> lost;
    EnglishToCp932("plain ASCII, an " + U(0x2014) + " and a " + U(0x2026),
                   &lost);
    EXPECT_TRUE(lost.empty());
}

// --- the characters a Japanese-to-English translation actually emits ---

TEST(EnglishFold, MacronVowelsLoseTheirMacrons) {
    // Romanized Japanese spells every long vowel with a macron, which makes
    // these the likeliest non-ASCII letters in the whole corpus.
    EXPECT_EQ(EnglishToCp932("T" + U(0x014D) + "ky" + U(0x014D)), "Tokyo");
    EXPECT_EQ(EnglishToCp932(U(0x014C) + "saka"), "Osaka");
    EXPECT_EQ(EnglishToCp932("j" + U(0x016B) + "d" + U(0x014D)), "judo");
    EXPECT_EQ(EnglishToCp932(U(0x0100) + U(0x0101) + U(0x0112) + U(0x0113) +
                             U(0x012A) + U(0x012B) + U(0x014C) + U(0x014D) +
                             U(0x016A) + U(0x016B)),
              "AaEeIiOoUu");
}

TEST(EnglishFold, TheSymbolsCp932OnlyHasFullwidthFormsOfAreSpelledInAscii) {
    // Each of these has a CP932 form, so nothing reports them; without a
    // fold they would draw as a two-column CJK glyph inside English prose.
    EXPECT_EQ(EnglishToCp932(U(0x30FB)), " ");    // katakana middle dot
    EXPECT_EQ(EnglishToCp932(U(0x00B7)), " ");    // middle dot
    EXPECT_EQ(EnglishToCp932("36" + U(0x00B0)), "36deg");
    EXPECT_EQ(EnglishToCp932("3" + U(0x00D7) + "4"), "3x4");
    EXPECT_EQ(EnglishToCp932("8" + U(0x00F7) + "2"), "8/2");
    EXPECT_EQ(EnglishToCp932(U(0x03B1) + "-" + U(0x03B2)), "alpha-beta");
    EXPECT_EQ(EnglishToCp932(U(0x0391)), "Alpha");
    EXPECT_EQ(EnglishToCp932("a" + U(0x301C) + "b"), "a~b");
    EXPECT_EQ(EnglishToCp932("Rin" + U(0x2020)), "Rin+");
    EXPECT_EQ(EnglishToCp932("well" + U(0x2025)), "well..");
    EXPECT_EQ(EnglishToCp932(U(0x2192)), "->");
    EXPECT_EQ(EnglishToCp932("a" + U(0x2E3A) + "b"), "a--b");
    EXPECT_EQ(EnglishToCp932(U(0x300C) + "yes" + U(0x300D)), "\"yes\"");
    EXPECT_EQ(EnglishToCp932(U(0x3010) + "x" + U(0x3011)), "[x]");
}

TEST(EnglishFold, TheLeftArrowKeepsNoAngleBracket) {
    // On its own the head would be drawn, but a '>' later in the same line
    // would pair with it and the engine would eat the words between them.
    const std::string folded = EnglishToCp932(U(0x2190));
    EXPECT_EQ(folded.find('<'), std::string::npos) << folded;
    EXPECT_EQ(folded, "--");
}

TEST(EnglishFold, NoCodePointAnywhereOpensATagOnItsOwn) {
    // Sweep every BMP code point: no fold may produce both '<' and '>' in
    // the same output, or the engine would eat the text between them.
    unsigned checked = 0;
    for (unsigned cp = 0x80; cp <= 0xFFFF; ++cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;  // surrogate halves
        ++checked;
        const std::string folded = EnglishToCp932(U(cp));
        const size_t open = folded.find('<');
        if (open == std::string::npos) continue;
        EXPECT_EQ(folded.find('>', open + 1), std::string::npos)
            << "U+" << std::hex << cp << " folded to " << folded;
    }
    EXPECT_GT(checked, 63000u);
}

TEST(EnglishFold, TheFullwidthLessThanComesBackHalfwidthLikeItsNeighbours) {
    // U+FF01..U+FF5E folds by subtracting 0xFEE0, this one included: a
    // fullwidth bracket in English prose is a two-column CJK glyph, and the
    // halfwidth one it becomes is only a hazard when a '>' follows it.
    EXPECT_EQ(EnglishToCp932(U(0xFF1C)), "<");
    EXPECT_EQ(EnglishToCp932(U(0xFF1D)), "=");
    // With a partner it becomes a tag, so the pair is broken up.
    EXPECT_EQ(EnglishToCp932(U(0xFF1C) + "r" + U(0xFF1E)), "(r>");
}

TEST(EnglishFold, AnOpenerWithAPartnerIsDrawnRoundAndOneWithoutIsLeftAlone) {
    // The engine consumes everything between '<' and the next '>', so a pair
    // costs the text inside it; a lone '<' is drawn as itself and costs
    // nothing, which is what lets a comparison keep its sign.
    EXPECT_EQ(EnglishToCp932("a < b > c"), "a ( b > c");
    EXPECT_EQ(EnglishToCp932("5 < 6"), "5 < 6");
    EXPECT_EQ(EnglishToCp932("6 > 5"), "6 > 5");
    EXPECT_EQ(EnglishToCp932(U(0x2192) + std::string(" east")), "-> east");
}

TEST(EnglishFold, CharactersThatDrawNothingAreDropped) {
    // They occupy no column but they do occupy bytes, and the wrapper
    // budgets in bytes.
    EXPECT_EQ(EnglishToCp932("a" + U(0x200B) + "b"), "ab");
    EXPECT_EQ(EnglishToCp932("a" + U(0x200D) + "b"), "ab");
    EXPECT_EQ(EnglishToCp932(U(0xFEFF) + "start"), "start");
    EXPECT_EQ(EnglishToCp932("soft" + U(0x00AD) + "hyphen"), "softhyphen");
}

TEST(EnglishFold, TheYenSignDoesNotBecomeABackslash) {
    // CP932 draws 0x5C as the yen sign, so letting U+00A5 through the codec
    // would put a backslash in the English and an escape in the log.
    EXPECT_EQ(EnglishToCp932("1000" + U(0x00A5)), "1000Y");
    EXPECT_EQ(EnglishToCp932(U(0xFFE5) + "500"), "Y500");
}

TEST(EnglishFold, AWideCharacterThatSurvivesIsVisibleToTheCaller) {
    using translator_logic::HasWideCharacter;
    EXPECT_FALSE(HasWideCharacter(EnglishToCp932("plain English --")));
    EXPECT_FALSE(HasWideCharacter(EnglishToCp932("T" + U(0x014D) + "ky" +
                                                 U(0x014D))));
    EXPECT_TRUE(HasWideCharacter(std::string("\x82\xA0")));
}

#ifdef _WIN32

TEST(EnglishFold, JapaneseLeftInAnEnglishCellStillConverts) {
    // Not every value is prose: a name row may legitimately be Japanese.
    // Those go through the codec instead of being folded away.
    std::vector<unsigned> lost;
    const std::string cp932 = EnglishToCp932(U(0x3042), &lost);  // hiragana A
    EXPECT_EQ(cp932, std::string("\x82\xA0"));
    EXPECT_TRUE(lost.empty());
}

TEST(EnglishFold, ACharacterWithNoCp932FormIsReportedNotSwallowed) {
    std::vector<unsigned> lost;
    // Hangul and an emoji: neither exists in CP932 under any fold.
    const std::string out =
        EnglishToCp932("a" + U(0xAC00) + "b" + U(0x1F600) + "c", &lost);
    EXPECT_EQ(out, "a?b?c");
    ASSERT_EQ(lost.size(), 2u);
    EXPECT_EQ(lost[0], 0xAC00u);
    EXPECT_EQ(lost[1], 0x1F600u);
}

TEST(ParseTsv, TheEnglishValueIsConvertedAndFolded) {
    TranslationMap m;
    const std::string tsv =
        U(0x3042) + "\t" + U(0x3000) + "He turned" + U(0x2014) + "slowly" +
        U(0x2026) + "\n";
    ASSERT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m), 1);
    EXPECT_EQ(m[std::string("\x82\xA0")], "He turned--slowly...");
}

TEST(ParseTsv, AnUnmappableEnglishCharacterIsReportedWithItsRow) {
    TranslationMap m;
    std::vector<UnmappableChar> lost;
    const std::string tsv = U(0x3042) + "\tgo" + U(0xAC00) + "od\n";
    ASSERT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m, &lost), 1);
    EXPECT_EQ(m[std::string("\x82\xA0")], "go?od");
    ASSERT_EQ(lost.size(), 1u);
    EXPECT_EQ(lost[0].codepoint, 0xAC00u);
    EXPECT_EQ(lost[0].jp_key, U(0x3042));  // the row, still readable
}

// --- ruby markup ------------------------------------------------------

TEST(EngineMarkup, ABracketPairIsWhatCountsAsMarkup) {
    using translator_logic::CarriesEngineMarkup;
    EXPECT_FALSE(CarriesEngineMarkup("ordinary English prose."));
    EXPECT_TRUE(CarriesEngineMarkup("a <r yes>tag"));
    EXPECT_TRUE(CarriesEngineMarkup("a closer</r>"));
    // A '>' on its own opens nothing; the arrow is spelled with one.
    EXPECT_FALSE(CarriesEngineMarkup("east -> west"));
    // A '<' the engine would draw rather than read is not markup.
    EXPECT_FALSE(CarriesEngineMarkup("5 < 6"));
    EXPECT_FALSE(CarriesEngineMarkup("an empty pair <> draws itself"));
}

TEST(EngineMarkup, AWholeTagLosesItsReadingAndKeepsItsBase) {
    using translator_logic::StripEngineMarkup;
    // The tag as the script writes it: a kana reading, then the kanji.
    const std::string reading = U(0x3055) + U(0x304F) + U(0x3089);
    const std::string base = U(0x685C);
    EXPECT_EQ(StripEngineMarkup("<r " + reading + ">" + base + "</r> fell"),
              base + " fell");
    EXPECT_EQ(StripEngineMarkup("a <r yes>b</r> c <r no>d</r> e"), "a b c d e");
    // The CJK angle brackets are different characters and are left alone
    // here; the fold turns them into quotation marks later.
    EXPECT_EQ(StripEngineMarkup("The " + U(0x3008) + "x"),
              "The " + U(0x3008) + "x");
}

TEST(EngineMarkup, AMalformedFragmentIsRemovedToo) {
    using translator_logic::StripEngineMarkup;
    // The corpus carries both of these shapes, and the model has already
    // produced one of them once.
    EXPECT_EQ(StripEngineMarkup("base</>"), "base");
    EXPECT_EQ(StripEngineMarkup("<r reading>base"), "base");
}

TEST(EngineMarkup, AShapeTheNameTableDoesNotKnowGoesTooBecauseTheEngineEatsIt) {
    using translator_logic::StripEngineMarkup;
    // The engine matches the tag name by prefix against fourteen names and
    // skips over a tag it cannot name, so the reading is lost either way.
    // These are the shapes the corpus writes that no name matches.
    const std::string kana = U(0x304B) + U(0x306A);
    EXPECT_EQ(StripEngineMarkup("<r" + kana + ">base"), "base");
    EXPECT_EQ(StripEngineMarkup("< " + kana + ">base"), "base");
    EXPECT_EQ(StripEngineMarkup("<" + U(0xFF52) + " " + kana + ">base"), "base");
}

TEST(EngineMarkup, AnOpenerWithNoPartnerIsLeftExactlyAsTheEngineDrawsIt) {
    using translator_logic::StripEngineMarkup;
    // No '>' after it, so the engine draws the bracket; rewriting it here
    // would make the table and the screen disagree.
    EXPECT_EQ(StripEngineMarkup("a < b"), "a < b");
    EXPECT_EQ(StripEngineMarkup("<unclosed"), "<unclosed");
    EXPECT_EQ(StripEngineMarkup("3 > 2"), "3 > 2") << "'>' opens nothing";
    EXPECT_EQ(StripEngineMarkup("an empty pair <> stays"),
              "an empty pair <> stays");
}

TEST(EngineMarkup, TextWithoutABracketIsReturnedUnchanged) {
    using translator_logic::StripEngineMarkup;
    const std::string plain = "The pale crimson dancing in the wind -- 1985.";
    EXPECT_EQ(StripEngineMarkup(plain), plain);
}

TEST(ParseTsv, ARowWhoseEnglishCarriesMarkupIsStrippedAndNamed) {
    TranslationMap m;
    std::vector<UnmappableChar> lost;
    std::vector<std::string> markup;
    const std::string tsv =
        U(0x3042) + "\tthe <r " + U(0x3055) + ">cherry</r> tree\n";
    ASSERT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m, &lost, &markup), 1);
    EXPECT_EQ(m[std::string("\x82\xA0")], "the cherry tree");
    ASSERT_EQ(markup.size(), 1u);
    EXPECT_EQ(markup[0], U(0x3042));  // the row, still readable
}

TEST(ParseTsv, ARowWithoutMarkupIsNotReported) {
    TranslationMap m;
    std::vector<std::string> markup;
    const std::string tsv = U(0x3042) + "\tthe cherry tree\n";
    ASSERT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m, nullptr, &markup), 1);
    EXPECT_TRUE(markup.empty());
}

TEST(ParseTsv, NoValueEverReachesTheRendererCarryingAnOpenTag) {
    // The load is the last gate before the engine, so it has to hold even
    // for a bracket no strip recognised.  A lone '<' is not a tag and stays:
    // the engine draws it, and rewriting it would only lose a comparison.
    TranslationMap m;
    const std::string tsv =
        U(0x3042) + "\ta < b\n" + U(0x3044) + "\t<r x>y</r>\n" +
        U(0x3046) + "\tplain " + U(0xFF1C) + "wide\n" +
        U(0x3048) + "\tone <of these> pairs\n";
    ASSERT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m), 4);
    for (const auto& kv : m) {
        const size_t open = kv.second.find('<');
        if (open == std::string::npos) continue;
        EXPECT_EQ(kv.second.find('>', open + 1), std::string::npos) << kv.second;
    }
    EXPECT_EQ(m[std::string("\x82\xA0")], "a < b");
    EXPECT_EQ(m[std::string("\x82\xA4")], "plain <wide");
    // A pair is markup wherever it came from: the strip takes it out before
    // the folds ever see it.
    EXPECT_EQ(m[std::string("\x82\xA6")], "one  pairs");
}

TEST(ParseTsv, TheJapaneseKeyKeepsItsIdeographicSpace) {
    // The key has to match the wire bytes exactly, so the fold that drops
    // U+3000 from English must not touch it: CP932 82 40 is that character.
    TranslationMap m;
    const std::string tsv = U(0x3000) + U(0x3042) + "\tHello\n";
    ASSERT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m), 1);
    EXPECT_EQ(m.count(std::string("\x81\x40\x82\xA0")), 1u);
}

// --- Furigana in a buffer the engine draws itself ----------------------
//
// Ruby readings overlap other lines at the four-line pitch, so they are
// stripped before the engine sees the buffer.

TEST(Ruby, AReadingIsRecognisedInBothSpellings) {
    EXPECT_TRUE(translator_logic::CarriesRuby("<r kana>base</r>"));
    EXPECT_TRUE(translator_logic::CarriesRuby("<ruby kana>base</ruby>"));
    EXPECT_TRUE(translator_logic::CarriesRuby("before </r> after"));
    EXPECT_FALSE(translator_logic::CarriesRuby("plain text"));
    EXPECT_FALSE(translator_logic::CarriesRuby("<b>bold</b>"));
    EXPECT_FALSE(translator_logic::CarriesRuby("<c 12>colour</c>"));
}

TEST(Ruby, TheReadingGoesAndTheBaseTextStays) {
    EXPECT_EQ(translator_logic::StripRubyForDisplay("<r kana>base</r>"), "base");
    EXPECT_EQ(translator_logic::StripRubyForDisplay("a<ruby k>b</r>c"), "abc");
}

TEST(Ruby, EveryOtherTagIsLeftForTheEngineToDraw) {
    const std::string styled = "<b>bold</b> and <c 3>colour</c>";
    EXPECT_EQ(translator_logic::StripRubyForDisplay(styled), styled);
    // A '<' with no '>' after it is drawn as itself, so it survives too.
    EXPECT_EQ(translator_logic::StripRubyForDisplay("5 < 6"), "5 < 6");
}

TEST(Ruby, ATagCarryingATwoByteReadingIsRemovedWhole) {
    // The reading rides in the tag's argument, which is CP932 -- and neither
    // '<' nor '>' can be a trail byte, so the run is found by scanning bytes.
    const std::string wire = "\x81\x40<r \x82\xA9\x82\xC8>\x94\x92\x8D\x67</r>\x82\xCD";
    const std::string drawn = translator_logic::StripRubyForDisplay(wire);
    EXPECT_EQ(drawn, "\x81\x40\x94\x92\x8D\x67\x82\xCD");
    EXPECT_FALSE(translator_logic::CarriesRuby(drawn));
}

TEST(Ruby, RemovingItChangesNoColumnTheEngineWouldHaveDrawn) {
    // The engine consumes a tag and draws none of it, so taking it out cannot
    // move a line break -- which is what lets the buffer be rewritten without
    // recounting the box.
    const std::string with = "<r kana>base</r> and more text here";
    EXPECT_EQ(translator_logic::StripRubyForDisplay(with),
              "base and more text here");
}

#endif  // _WIN32
