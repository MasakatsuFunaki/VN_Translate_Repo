// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — TSV parser tests for FAVORITE.
//
// Tests the TSV-format handling in translator_logic: backslash-escape
// decoding (UnescapeInPlace) and the line-by-line parser (ParseTsvBuffer).
// All inputs are synthetic in-memory buffers; no file I/O.
//
// Sister UT file cp932_ut.cpp covers the CP932 byte/codec helpers.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <string>

using translator_logic::TranslationMap;
using translator_logic::ParseTsvBuffer;
using translator_logic::UnescapeInPlace;

TEST(Unescape, BasicEscapes) {
    std::string s = "a\\nb\\tc\\\\d\\re";
    UnescapeInPlace(s);
    EXPECT_EQ(s, "a\nb\tc\\d\re");
}

TEST(Unescape, NoEscapes) {
    std::string s = "plain ascii";
    UnescapeInPlace(s);
    EXPECT_EQ(s, "plain ascii");
}

TEST(Unescape, UnknownEscapeKeepsBackslash) {
    std::string s = "a\\qb";
    UnescapeInPlace(s);
    EXPECT_EQ(s, "a\\qb");
}

TEST(Unescape, TrailingBackslashIsPreserved) {
    std::string s = "trailing\\";
    UnescapeInPlace(s);
    EXPECT_EQ(s, "trailing\\");
}

TEST(ParseTsv, EmptyBuffer) {
    TranslationMap m;
    EXPECT_EQ(ParseTsvBuffer("", 0, m), 0);
    EXPECT_TRUE(m.empty());
}

TEST(ParseTsv, AsciiOnly) {
    TranslationMap m;
    const char tsv[] = "hello\tHello\nworld\tWorld\n";
    int n = ParseTsvBuffer(tsv, sizeof(tsv) - 1, m);
    EXPECT_EQ(n, 2);
    EXPECT_EQ(m["hello"], "Hello");
    EXPECT_EQ(m["world"], "World");
}

TEST(ParseTsv, EscapedNewline) {
    TranslationMap m;
    const char tsv[] = "a\\nb\tA\\nB\n";
    EXPECT_EQ(ParseTsvBuffer(tsv, sizeof(tsv) - 1, m), 1);
    EXPECT_EQ(m["a\nb"], "A\nB");
}

TEST(ParseTsv, CrlfLineEndings) {
    TranslationMap m;
    const char tsv[] = "x\tX\r\ny\tY\r\n";
    EXPECT_EQ(ParseTsvBuffer(tsv, sizeof(tsv) - 1, m), 2);
    EXPECT_EQ(m["x"], "X");
    EXPECT_EQ(m["y"], "Y");
}

TEST(ParseTsv, EmptyOrMalformedLinesIgnored) {
    TranslationMap m;
    const char tsv[] = "good\tValue\nno-tab\nempty-jp\t\n\tempty-en\n";
    int n = ParseTsvBuffer(tsv, sizeof(tsv) - 1, m);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(m["good"], "Value");
}

TEST(NormalizeCp932, StripsMixedAsciiAndFullwidthSpacesBothEnds) {
    // CP932: "　 ？？？ 　" -> 0x81 0x40 ' ' 0x81 0x48 x3 ' ' 0x81 0x40
    const char input[] = "\x81\x40 \x81\x48\x81\x48\x81\x48 \x81\x40";
    const std::string expected = "\x81\x48\x81\x48\x81\x48";
    EXPECT_EQ(translator_logic::NormalizeCp932LookupKey(input, sizeof(input) - 1),
              expected);
}

TEST(NormalizeCp932, UnwrapsCornerBracketsAndInnerSpaces) {
    // CP932: "「…………」" -> 0x81 0x75 + 6x(0x81 0x63) + 0x81 0x76
    const char kagi[] =
        "\x81\x75\x81\x63\x81\x63\x81\x63\x81\x63\x81\x63\x81\x63\x81\x76";
    const std::string expected_dots =
        "\x81\x63\x81\x63\x81\x63\x81\x63\x81\x63\x81\x63";
    EXPECT_EQ(translator_logic::NormalizeCp932LookupKey(kagi, sizeof(kagi) - 1),
              expected_dots);

    // CP932: "『　クロ　』" -> 0x81 0x77 + 0x81 0x40 + 0x83 0x4E 0x83 0x8D + 0x81 0x40 + 0x81 0x78
    const char nijuu[] = "\x81\x77\x81\x40\x83\x4E\x83\x8D\x81\x40\x81\x78";
    const std::string expected_kuro = "\x83\x4E\x83\x8D";
    EXPECT_EQ(translator_logic::NormalizeCp932LookupKey(nijuu, sizeof(nijuu) - 1),
              expected_kuro);
}

TEST(NormalizeCp932, PreservesKanjiWith0x40TrailByte) {
    // CP932: "　占　" -> 0x81 0x40 + 0x90 0x40 ('占') + 0x81 0x40
    const char input[] = "\x81\x40\x90\x40\x81\x40";
    const std::string expected = "\x90\x40";
    EXPECT_EQ(translator_logic::NormalizeCp932LookupKey(input, sizeof(input) - 1),
              expected);
}

TEST(WrapAsciiToColumnGrid, WrapsScreenshotDialogueAtColumn58) {
    const std::string s1 =
        "It's okay. I promise, I promise. I won't ever let you be alone.";
    const std::string w1 = translator_logic::WrapAsciiToColumnGrid(s1, 58);
    ASSERT_GE(w1.size(), 64u);
    EXPECT_EQ(w1.substr(0, 58),
              "It's okay. I promise, I promise. I won't ever let you be  ");
    EXPECT_EQ(w1.substr(58), "alone.");

    const std::string s2 =
        "The world that peeked back at us through the window was vivid and alive.";
    const std::string w2 = translator_logic::WrapAsciiToColumnGrid(s2, 58);
    ASSERT_GE(w2.size(), 72u);
    EXPECT_EQ(w2.substr(0, 58),
              "The world that peeked back at us through the window was   ");
    EXPECT_EQ(w2.substr(58), "vivid and alive.");
}

TEST(WrapAsciiToColumnGrid, LeavesShortStringsUnchangedAndHandlesExactBoundary) {
    EXPECT_EQ(translator_logic::WrapAsciiToColumnGrid("Chiwa", 58), "Chiwa");
    // 58 'a's followed by " next" should consume the separator space so "next" starts at index 58.
    const std::string exact58 = std::string(58, 'a') + " next";
    const std::string wrapped = translator_logic::WrapAsciiToColumnGrid(exact58, 58);
    EXPECT_EQ(wrapped.substr(0, 58), std::string(58, 'a'));
    EXPECT_EQ(wrapped.substr(58), "next");
}

// NOTE: ConvertsUtf8KeyToCp932 was removed for FAVORITE.
// That test asserts ParseTsvBuffer converts UTF-8 input keys to CP932
// in the map — which is CROSS_CHANNEL's architecture. FAVORITE's FVP
// engine does want CP932 keys at runtime (the hook receives CP932
// bytes), but the conversion happens in a SEPARATE re-keying step in
// LoadTranslations (Utf8ToCp932 in src/translator_favorite.cpp:55-71),
// AFTER ParseTsvBuffer returns. ParseTsvBuffer itself only produces
// UTF-8 keys exactly as the TSV stored them. The CP932 codec helpers
// are exercised by the sister cp932_ut.cpp UT file instead.
