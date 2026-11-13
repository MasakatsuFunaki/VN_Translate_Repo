// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Synthetic in-memory unit tests for TSV escape decoding (UnescapeInPlace)
// and line-by-line CP932 TSV parsing (ParseTsvBuffer) in translator_logic.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

constexpr int kExpectedZeroRows = 0;
constexpr int kExpectedSingleRow = 1;
constexpr int kExpectedTwoRows = 2;
constexpr std::size_t kAsciiEscapeLetterCount = 3;
constexpr std::array<std::string_view, kAsciiEscapeLetterCount> kAsciiEscapeLetters = {{
    "r",
    "n",
    "t",
}};

std::string Utf8ToCp932(std::string_view utf8) {
    const std::wstring wide = std::filesystem::u8path(utf8).wstring();
    std::string cp932;
    EXPECT_TRUE(translator_logic::EncodeWideToCp932(wide, cp932));
    return cp932;
}

}  // namespace

using translator_logic::ParseTsvBuffer;
using translator_logic::TranslationMap;
using translator_logic::TranslationTable;
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

TEST(Unescape, PreservesCp932TrailByteBackslash) {
    const std::string raw_bars = Utf8ToCp932("声は――――");
    ASSERT_FALSE(raw_bars.empty());
    ASSERT_EQ(raw_bars.back(), '\\');
    const std::string expected_bars_hd10 = Utf8ToCp932("声は――――\r\n");
    const std::string expected_bars_ce = raw_bars;

    const std::string escaped_bars_key = raw_bars + "\\r\\n";
    std::string unescaped_bars = escaped_bars_key;
    UnescapeInPlace(unescaped_bars);
    EXPECT_EQ(unescaped_bars, expected_bars_hd10);

    const std::string raw_impossible = Utf8ToCp932("不可能");
    ASSERT_FALSE(raw_impossible.empty());
    ASSERT_EQ(raw_impossible.back(), '\\');
    const std::string escaped_midline_key =
        raw_impossible + "\\r\\n" + Utf8ToCp932("だった。") + "\\r\\n";
    const std::string expected_midline_hd10 = Utf8ToCp932("不可能\r\nだった。\r\n");
    const std::string expected_midline_ce = Utf8ToCp932("不可能\r\nだった。");

    std::string unescaped_midline = escaped_midline_key;
    UnescapeInPlace(unescaped_midline);
    EXPECT_EQ(unescaped_midline, expected_midline_hd10);

    const std::string synthetic_tsv =
        escaped_bars_key + "\tThe voice he heard was --\\r\\n\r\n" +
        escaped_midline_key + "\tIt was plainly impossible.\\r\\n\r\n";

    TranslationTable table;
    ASSERT_EQ(table.LoadFromTsvBuffer(synthetic_tsv.data(), synthetic_tsv.size()),
              kExpectedTwoRows);

    std::string out_bars_hd10;
    ASSERT_TRUE(table.Lookup(expected_bars_hd10.c_str(), false, out_bars_hd10));
    EXPECT_EQ(out_bars_hd10, "The voice he heard was --\r\n");

    std::string out_bars_ce;
    ASSERT_TRUE(table.Lookup(expected_bars_ce.c_str(), false, out_bars_ce));
    EXPECT_EQ(out_bars_ce, out_bars_hd10);

    std::string out_midline_hd10;
    ASSERT_TRUE(table.Lookup(expected_midline_hd10.c_str(), false, out_midline_hd10));
    EXPECT_EQ(out_midline_hd10, "It was plainly impossible.\r\n");

    std::string out_midline_ce;
    ASSERT_TRUE(table.Lookup(expected_midline_ce.c_str(), false, out_midline_ce));
    EXPECT_EQ(out_midline_ce, out_midline_hd10);

    const std::string kanji_hyo = Utf8ToCp932("表");
    ASSERT_FALSE(kanji_hyo.empty());
    ASSERT_EQ(kanji_hyo.back(), '\\');

    for (const std::string_view suffix : kAsciiEscapeLetters) {
        std::string candidate = kanji_hyo + std::string(suffix);
        const std::string expected = candidate;
        UnescapeInPlace(candidate);
        EXPECT_EQ(candidate, expected);
    }

    std::string kanji_escaped_backslash = kanji_hyo + "\\\\";
    UnescapeInPlace(kanji_escaped_backslash);
    EXPECT_EQ(kanji_escaped_backslash, kanji_hyo + "\\");
}

TEST(ParseTsv, EmptyBuffer) {
    TranslationMap m;
    EXPECT_EQ(ParseTsvBuffer("", 0, m), kExpectedZeroRows);
    EXPECT_TRUE(m.empty());
}

TEST(ParseTsv, AsciiOnly) {
    TranslationMap m;
    constexpr std::string_view kTsv = "hello\tHello\nworld\tWorld\n";
    const int n = ParseTsvBuffer(kTsv.data(), kTsv.size(), m);
    EXPECT_EQ(n, kExpectedTwoRows);
    EXPECT_EQ(m["hello"], "Hello");
    EXPECT_EQ(m["world"], "World");
}

TEST(ParseTsv, EscapedNewline) {
    TranslationMap m;
    constexpr std::string_view kTsv = "a\\nb\tA\\nB\n";
    EXPECT_EQ(ParseTsvBuffer(kTsv.data(), kTsv.size(), m), kExpectedSingleRow);
    EXPECT_EQ(m["a\nb"], "A\nB");
}

TEST(ParseTsv, CrlfLineEndings) {
    TranslationMap m;
    constexpr std::string_view kTsv = "x\tX\r\ny\tY\r\n";
    EXPECT_EQ(ParseTsvBuffer(kTsv.data(), kTsv.size(), m), kExpectedTwoRows);
    EXPECT_EQ(m["x"], "X");
    EXPECT_EQ(m["y"], "Y");
}

TEST(ParseTsv, EmptyOrMalformedLinesIgnored) {
    TranslationMap m;
    constexpr std::string_view kTsv = "good\tValue\nno-tab\nempty-jp\t\n\tempty-en\n";
    const int n = ParseTsvBuffer(kTsv.data(), kTsv.size(), m);
    EXPECT_EQ(n, kExpectedSingleRow);
    EXPECT_EQ(m["good"], "Value");
}
