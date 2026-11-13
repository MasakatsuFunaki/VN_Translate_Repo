// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT: EnglishToCp932 fold of non-ASCII English to halfwidth ASCII (or report
// of unmappable characters).  Sister file cp932_ut.cpp covers the raw codec.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

using translator_logic::EnglishToCp932;
using translator_logic::ParseTsvBuffer;
using translator_logic::TranslationMap;
using translator_logic::UnmappableChar;
using translator_logic::Utf8ToCp932;

namespace {

constexpr unsigned kAsciiLimit = 0x80;
constexpr unsigned kTwoByteLimit = 0x800;
constexpr unsigned kThreeByteLimit = 0x10000;
constexpr unsigned kBmpMax = 0xFFFF;
constexpr unsigned kSurrogateStart = 0xD800;
constexpr unsigned kSurrogateEnd = 0xDFFF;
constexpr unsigned kMinExpectedBmpNonAsciiChecked = 63000;

constexpr unsigned kUtf8TwoByteLead = 0xC0u;
constexpr unsigned kUtf8ThreeByteLead = 0xE0u;
constexpr unsigned kUtf8FourByteLead = 0xF0u;
constexpr unsigned kUtf8ContByte = 0x80u;
constexpr unsigned kUtf8ContMask = 0x3Fu;
constexpr unsigned kShift6 = 6;
constexpr unsigned kShift12 = 12;
constexpr unsigned kShift18 = 18;

constexpr unsigned kHangulSyllableGa = 0xAC00u;
constexpr unsigned kEmojiGrinningFace = 0x1F600u;

constexpr int kOneRow = 1;
constexpr int kFourRows = 4;
constexpr std::size_t kOneEntry = 1;
constexpr std::size_t kTwoEntries = 2;

// UTF-8 for one code point, for explicit codepoint-sweep tests.
std::string U(unsigned cp) {
    std::string s;
    if (cp < kAsciiLimit) {
        s += static_cast<char>(cp);
        return s;
    }
    if (cp < kTwoByteLimit) {
        s += static_cast<char>(kUtf8TwoByteLead | (cp >> kShift6));
        s += static_cast<char>(kUtf8ContByte | (cp & kUtf8ContMask));
        return s;
    }
    if (cp < kThreeByteLimit) {
        s += static_cast<char>(kUtf8ThreeByteLead | (cp >> kShift12));
        s += static_cast<char>(kUtf8ContByte | ((cp >> kShift6) & kUtf8ContMask));
        s += static_cast<char>(kUtf8ContByte | (cp & kUtf8ContMask));
        return s;
    }
    s += static_cast<char>(kUtf8FourByteLead | (cp >> kShift18));
    s += static_cast<char>(kUtf8ContByte | ((cp >> kShift12) & kUtf8ContMask));
    s += static_cast<char>(kUtf8ContByte | ((cp >> kShift6) & kUtf8ContMask));
    s += static_cast<char>(kUtf8ContByte | (cp & kUtf8ContMask));
    return s;
}

bool IsAscii(const std::string& s) {
    for (char c : s) {
        if (static_cast<unsigned char>(c) >= kAsciiLimit) return false;
    }
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
    EXPECT_EQ(EnglishToCp932("　The wind rose."), "The wind rose.");
    EXPECT_EQ(EnglishToCp932("a　b"), "ab");
}

TEST(EnglishFold, FullwidthAsciiComesBackHalfwidth) {
    EXPECT_EQ(EnglishToCp932("Ａｂ！"), "Ab!");
    EXPECT_EQ(EnglishToCp932("、。"), ",.");
}

TEST(EnglishFold, AFoldedValueIsPureAscii) {
    const std::string worst =
        "　" + U(0x201C) + "Caf" + U(0x00E9) + U(0x2019) + "s " +
        U(0x2014) + " na" + U(0x00EF) + "ve, se" + U(0x00F1) + "or" +
        U(0x2026) + U(0x201D) + "！";
    const std::string folded = EnglishToCp932(worst);
    EXPECT_TRUE(IsAscii(folded)) << folded;
    EXPECT_EQ(folded, "\"Cafe's -- naive, senor...\"!");
}

TEST(EnglishFold, NothingEverFoldsToAnAngleBracket) {
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

// Characters a Japanese-to-English translation emits.

TEST(EnglishFold, MacronVowelsLoseTheirMacrons) {
    EXPECT_EQ(EnglishToCp932("T" + U(0x014D) + "ky" + U(0x014D)), "Tokyo");
    EXPECT_EQ(EnglishToCp932(U(0x014C) + "saka"), "Osaka");
    EXPECT_EQ(EnglishToCp932("j" + U(0x016B) + "d" + U(0x014D)), "judo");
    EXPECT_EQ(EnglishToCp932(U(0x0100) + U(0x0101) + U(0x0112) + U(0x0113) +
                             U(0x012A) + U(0x012B) + U(0x014C) + U(0x014D) +
                             U(0x016A) + U(0x016B)),
              "AaEeIiOoUu");
}

TEST(EnglishFold, TheSymbolsCp932OnlyHasFullwidthFormsOfAreSpelledInAscii) {
    EXPECT_EQ(EnglishToCp932("・"), " ");
    EXPECT_EQ(EnglishToCp932(U(0x00B7)), " ");
    EXPECT_EQ(EnglishToCp932("36" + U(0x00B0)), "36deg");
    EXPECT_EQ(EnglishToCp932("3" + U(0x00D7) + "4"), "3x4");
    EXPECT_EQ(EnglishToCp932("8" + U(0x00F7) + "2"), "8/2");
    EXPECT_EQ(EnglishToCp932(U(0x03B1) + "-" + U(0x03B2)), "alpha-beta");
    EXPECT_EQ(EnglishToCp932(U(0x0391)), "Alpha");
    EXPECT_EQ(EnglishToCp932("a〜b"), "a~b");
    EXPECT_EQ(EnglishToCp932("Rin" + U(0x2020)), "Rin+");
    EXPECT_EQ(EnglishToCp932("well" + U(0x2025)), "well..");
    EXPECT_EQ(EnglishToCp932(U(0x2192)), "->");
    EXPECT_EQ(EnglishToCp932("a" + U(0x2E3A) + "b"), "a--b");
    EXPECT_EQ(EnglishToCp932("「yes」"), "\"yes\"");
    EXPECT_EQ(EnglishToCp932("【x】"), "[x]");
}

TEST(EnglishFold, TheLeftArrowKeepsNoAngleBracket) {
    const std::string folded = EnglishToCp932(U(0x2190));
    EXPECT_EQ(folded.find('<'), std::string::npos) << folded;
    EXPECT_EQ(folded, "--");
}

TEST(EnglishFold, NoCodePointAnywhereOpensATagOnItsOwn) {
    unsigned checked = 0;
    for (unsigned cp = kAsciiLimit; cp <= kBmpMax; ++cp) {
        if (cp >= kSurrogateStart && cp <= kSurrogateEnd) continue;
        ++checked;
        const std::string folded = EnglishToCp932(U(cp));
        const std::size_t open = folded.find('<');
        if (open == std::string::npos) continue;
        EXPECT_EQ(folded.find('>', open + 1), std::string::npos)
            << "U+" << std::hex << cp << " folded to " << folded;
    }
    EXPECT_GT(checked, kMinExpectedBmpNonAsciiChecked);
}

TEST(EnglishFold, TheFullwidthLessThanComesBackHalfwidthLikeItsNeighbours) {
    EXPECT_EQ(EnglishToCp932("＜"), "<");
    EXPECT_EQ(EnglishToCp932("＝"), "=");
    EXPECT_EQ(EnglishToCp932("＜r＞"), "(r>");
}

TEST(EnglishFold, AnOpenerWithAPartnerIsDrawnRoundAndOneWithoutIsLeftAlone) {
    EXPECT_EQ(EnglishToCp932("a < b > c"), "a ( b > c");
    EXPECT_EQ(EnglishToCp932("5 < 6"), "5 < 6");
    EXPECT_EQ(EnglishToCp932("6 > 5"), "6 > 5");
    EXPECT_EQ(EnglishToCp932(U(0x2192) + std::string(" east")), "-> east");
}

TEST(EnglishFold, CharactersThatDrawNothingAreDropped) {
    EXPECT_EQ(EnglishToCp932("a" + U(0x200B) + "b"), "ab");
    EXPECT_EQ(EnglishToCp932("a" + U(0x200D) + "b"), "ab");
    EXPECT_EQ(EnglishToCp932(U(0xFEFF) + "start"), "start");
    EXPECT_EQ(EnglishToCp932("soft" + U(0x00AD) + "hyphen"), "softhyphen");
}

TEST(EnglishFold, TheYenSignDoesNotBecomeABackslash) {
    EXPECT_EQ(EnglishToCp932("1000" + U(0x00A5)), "1000Y");
    EXPECT_EQ(EnglishToCp932(U(0xFFE5) + "500"), "Y500");
}

TEST(EnglishFold, AWideCharacterThatSurvivesIsVisibleToTheCaller) {
    using translator_logic::HasWideCharacter;
    EXPECT_FALSE(HasWideCharacter(EnglishToCp932("plain English --")));
    EXPECT_FALSE(HasWideCharacter(
        EnglishToCp932("T" + U(0x014D) + "ky" + U(0x014D))));
#ifdef _WIN32
    EXPECT_TRUE(HasWideCharacter(Utf8ToCp932("あ")));
#endif
}

#ifdef _WIN32

TEST(EnglishFold, JapaneseLeftInAnEnglishCellStillConverts) {
    std::vector<unsigned> lost;
    const std::string cp932 = EnglishToCp932("あ", &lost);
    EXPECT_EQ(cp932, Utf8ToCp932("あ"));
    EXPECT_TRUE(lost.empty());
}

TEST(EnglishFold, ACharacterWithNoCp932FormIsReportedNotSwallowed) {
    std::vector<unsigned> lost;
    const std::string out = EnglishToCp932(
        "a" + U(kHangulSyllableGa) + "b" + U(kEmojiGrinningFace) + "c", &lost);
    EXPECT_EQ(out, "a?b?c");
    ASSERT_EQ(lost.size(), kTwoEntries);
    EXPECT_EQ(lost[0], kHangulSyllableGa);
    EXPECT_EQ(lost[1], kEmojiGrinningFace);
}

TEST(ParseTsv, TheEnglishValueIsConvertedAndFolded) {
    TranslationMap m;
    const std::string tsv =
        "あ\t　He turned" + U(0x2014) + "slowly" + U(0x2026) + "\n";
    ASSERT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m), kOneRow);
    EXPECT_EQ(m[Utf8ToCp932("あ")], "He turned--slowly...");
}

TEST(ParseTsv, AnUnmappableEnglishCharacterIsReportedWithItsRow) {
    TranslationMap m;
    std::vector<UnmappableChar> lost;
    const std::string tsv = "あ\tgo" + U(kHangulSyllableGa) + "od\n";
    ASSERT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m, &lost), kOneRow);
    EXPECT_EQ(m[Utf8ToCp932("あ")], "go?od");
    ASSERT_EQ(lost.size(), kOneEntry);
    EXPECT_EQ(lost[0].codepoint, kHangulSyllableGa);
    EXPECT_EQ(lost[0].jp_key, "あ");
}

// Engine ruby markup handling.

TEST(EngineMarkup, ABracketPairIsWhatCountsAsMarkup) {
    using translator_logic::CarriesEngineMarkup;
    EXPECT_FALSE(CarriesEngineMarkup("ordinary English prose."));
    EXPECT_TRUE(CarriesEngineMarkup("a <r yes>tag"));
    EXPECT_TRUE(CarriesEngineMarkup("a closer</r>"));
    EXPECT_FALSE(CarriesEngineMarkup("east -> west"));
    EXPECT_FALSE(CarriesEngineMarkup("5 < 6"));
    EXPECT_FALSE(CarriesEngineMarkup("an empty pair <> draws itself"));
}

TEST(EngineMarkup, AWholeTagLosesItsReadingAndKeepsItsBase) {
    using translator_logic::StripEngineMarkup;
    EXPECT_EQ(StripEngineMarkup("<r さくら>桜</r> fell"), "桜 fell");
    EXPECT_EQ(StripEngineMarkup("a <r yes>b</r> c <r no>d</r> e"), "a b c d e");
    EXPECT_EQ(StripEngineMarkup("The 〈x"), "The 〈x");
}

TEST(EngineMarkup, AMalformedFragmentIsRemovedToo) {
    using translator_logic::StripEngineMarkup;
    EXPECT_EQ(StripEngineMarkup("base</>"), "base");
    EXPECT_EQ(StripEngineMarkup("<r reading>base"), "base");
}

TEST(EngineMarkup, AShapeTheNameTableDoesNotKnowGoesTooBecauseTheEngineEatsIt) {
    using translator_logic::StripEngineMarkup;
    EXPECT_EQ(StripEngineMarkup("<rかな>base"), "base");
    EXPECT_EQ(StripEngineMarkup("< かな>base"), "base");
    EXPECT_EQ(StripEngineMarkup("<ｒ かな>base"), "base");
}

TEST(EngineMarkup, AnOpenerWithNoPartnerIsLeftExactlyAsTheEngineDrawsIt) {
    using translator_logic::StripEngineMarkup;
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
    const std::string tsv = "あ\tthe <r さ>cherry</r> tree\n";
    ASSERT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m, &lost, &markup),
              kOneRow);
    EXPECT_EQ(m[Utf8ToCp932("あ")], "the cherry tree");
    ASSERT_EQ(markup.size(), kOneEntry);
    EXPECT_EQ(markup[0], "あ");
}

TEST(ParseTsv, ARowWithoutMarkupIsNotReported) {
    TranslationMap m;
    std::vector<std::string> markup;
    const std::string tsv = "あ\tthe cherry tree\n";
    ASSERT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m, nullptr, &markup),
              kOneRow);
    EXPECT_TRUE(markup.empty());
}

TEST(ParseTsv, NoValueEverReachesTheRendererCarryingAnOpenTag) {
    TranslationMap m;
    const std::string tsv =
        "あ\ta < b\n"
        "い\t<r x>y</r>\n"
        "う\tplain ＜wide\n"
        "え\tone <of these> pairs\n";
    ASSERT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m), kFourRows);
    for (const auto& kv : m) {
        const std::size_t open = kv.second.find('<');
        if (open == std::string::npos) continue;
        EXPECT_EQ(kv.second.find('>', open + 1), std::string::npos) << kv.second;
    }
    EXPECT_EQ(m[Utf8ToCp932("あ")], "a < b");
    EXPECT_EQ(m[Utf8ToCp932("う")], "plain <wide");
    EXPECT_EQ(m[Utf8ToCp932("え")], "one  pairs");
}

TEST(ParseTsv, TheJapaneseKeyKeepsItsIdeographicSpace) {
    TranslationMap m;
    const std::string tsv = "　あ\tHello\n";
    ASSERT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m), kOneRow);
    EXPECT_EQ(m.count(Utf8ToCp932("　あ")), kOneEntry);
}

// Furigana in a buffer the engine draws itself.

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
    EXPECT_EQ(translator_logic::StripRubyForDisplay("5 < 6"), "5 < 6");
}

TEST(Ruby, ATagCarryingATwoByteReadingIsRemovedWhole) {
    const std::string wire = Utf8ToCp932("　<r かな>白紅</r>は");
    const std::string drawn = translator_logic::StripRubyForDisplay(wire);
    EXPECT_EQ(drawn, Utf8ToCp932("　白紅は"));
    EXPECT_FALSE(translator_logic::CarriesRuby(drawn));
}

TEST(Ruby, RemovingItChangesNoColumnTheEngineWouldHaveDrawn) {
    const std::string with = "<r kana>base</r> and more text here";
    EXPECT_EQ(translator_logic::StripRubyForDisplay(with),
              "base and more text here");
}

#endif  // _WIN32
