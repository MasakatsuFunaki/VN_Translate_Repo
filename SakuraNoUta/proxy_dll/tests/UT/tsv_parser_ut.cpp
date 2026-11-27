// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT: TSV unescape and parser tests.  All inputs are synthetic in-memory
// buffers.  Sister file cp932_ut.cpp covers the raw codec helpers.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

using translator_logic::ParseTsvBuffer;
using translator_logic::TranslationMap;
using translator_logic::UnescapeInPlace;
using translator_logic::Utf8ToCp932;

namespace {

constexpr int kExpectedEmptyRows = 0;
constexpr int kExpectedOneRow = 1;
constexpr int kExpectedTwoRows = 2;
constexpr std::size_t kPresentKeyCount = 1;

}  // namespace

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
    EXPECT_EQ(ParseTsvBuffer("", 0, m), kExpectedEmptyRows);
    EXPECT_TRUE(m.empty());
}

TEST(ParseTsv, AsciiOnly) {
    TranslationMap m;
    const std::string tsv = "hello\tHello\nworld\tWorld\n";
    const int n = ParseTsvBuffer(tsv.data(), tsv.size(), m);
    EXPECT_EQ(n, kExpectedTwoRows);
    EXPECT_EQ(m["hello"], "Hello");
    EXPECT_EQ(m["world"], "World");
}

TEST(ParseTsv, EscapedNewline) {
    TranslationMap m;
    const std::string tsv = "a\\nb\tA\\nB\n";
    EXPECT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m), kExpectedOneRow);
    EXPECT_EQ(m["a\nb"], "A\nB");
}

TEST(ParseTsv, CrlfLineEndings) {
    TranslationMap m;
    const std::string tsv = "x\tX\r\ny\tY\r\n";
    EXPECT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m), kExpectedTwoRows);
    EXPECT_EQ(m["x"], "X");
    EXPECT_EQ(m["y"], "Y");
}

TEST(ParseTsv, EmptyOrMalformedLinesIgnored) {
    TranslationMap m;
    const std::string tsv = "good\tValue\nno-tab\nempty-jp\t\n\tempty-en\n";
    const int n = ParseTsvBuffer(tsv.data(), tsv.size(), m);
    EXPECT_EQ(n, kExpectedOneRow);
    EXPECT_EQ(m["good"], "Value");
}

TEST(ParseTsv, ConvertsUtf8KeyToCp932) {
    TranslationMap m;
    const std::string tsv = "あ\thello\n";
    const int n = ParseTsvBuffer(tsv.data(), tsv.size(), m);
    EXPECT_EQ(n, kExpectedOneRow);
    const std::string cp932_a = Utf8ToCp932("あ");
    EXPECT_EQ(m.count(cp932_a), kPresentKeyCount);
    EXPECT_EQ(m[cp932_a], "hello");
}

TEST(BgiWireDirective, RecognizesAndStripsTrailingBreakAmpersandOnly) {
    using translator_logic::HasBgiWireDirectiveSuffix;
    using translator_logic::StripBgiWireDirectiveSuffix;
    using translator_logic::StripLeakedTrailingDirective;

    EXPECT_FALSE(HasBgiWireDirectiveSuffix(""));
    EXPECT_FALSE(HasBgiWireDirectiveSuffix("&"));
    EXPECT_FALSE(HasBgiWireDirectiveSuffix("a&"));
    EXPECT_FALSE(HasBgiWireDirectiveSuffix("a\n"));
    EXPECT_FALSE(HasBgiWireDirectiveSuffix("a & b"));
    EXPECT_FALSE(HasBgiWireDirectiveSuffix("foo &"));
    EXPECT_TRUE(HasBgiWireDirectiveSuffix("\n&"));
    EXPECT_TRUE(HasBgiWireDirectiveSuffix("line\n&"));
    EXPECT_TRUE(HasBgiWireDirectiveSuffix("line\r&"));
    EXPECT_TRUE(HasBgiWireDirectiveSuffix("line\r\n&"));

    const std::string cp932_jp = Utf8ToCp932("あ");
    EXPECT_FALSE(HasBgiWireDirectiveSuffix(cp932_jp));
    EXPECT_TRUE(HasBgiWireDirectiveSuffix(cp932_jp + "\n&"));
    EXPECT_EQ(StripBgiWireDirectiveSuffix(cp932_jp + "\n&"), cp932_jp + "\n");

    EXPECT_EQ(StripBgiWireDirectiveSuffix("line\n&"), "line\n");
    EXPECT_EQ(StripBgiWireDirectiveSuffix("line\r&"), "line\r");
    EXPECT_EQ(StripBgiWireDirectiveSuffix("line\r\n&"), "line\r\n");
    EXPECT_EQ(StripBgiWireDirectiveSuffix("Rock & Roll"), "Rock & Roll");
    EXPECT_EQ(StripBgiWireDirectiveSuffix("Rock & Roll\n&"), "Rock & Roll\n");

    // StripLeakedTrailingDirective boundary & guard-mutation pins:
    EXPECT_EQ(StripLeakedTrailingDirective(""), "");
    EXPECT_EQ(StripLeakedTrailingDirective("&"), "");
    EXPECT_EQ(StripLeakedTrailingDirective("foo&"), "foo&");
    EXPECT_EQ(StripLeakedTrailingDirective("AT&T"), "AT&T");
    EXPECT_EQ(StripLeakedTrailingDirective("foo &"), "foo");
    EXPECT_EQ(StripLeakedTrailingDirective("foo\t&"), "foo");
    EXPECT_EQ(StripLeakedTrailingDirective("foo\r\n&"), "foo");
    EXPECT_EQ(StripLeakedTrailingDirective("foo\n&\n"), "foo");
    EXPECT_EQ(StripLeakedTrailingDirective("Watching me like that,\n&"),
              "Watching me like that,");
    EXPECT_EQ(StripLeakedTrailingDirective("Watching me like that, &"),
              "Watching me like that,");
    EXPECT_EQ(StripLeakedTrailingDirective("Watching me like that,"),
              "Watching me like that,");
}

TEST(ParseTsv, NormalizesBgiAmpersandKeyAndStripsLeakedValueDirective) {
    TranslationMap m;
    const std::string tsv =
        "　そんな俺を見て、\\n&\t　Watching me like that, &\n"
        "　まだ日は高い。\\n&\t　The sun was still high.\\n&\n"
        "ロック＆ロール\tTom & Jerry\n"
        "通常\tfoo &\n"
        "同一\\n&\t同一\\n&\n"
        "空になる\\n&\t &\n";
    // 5 rows kept; "空になる\n&\t &\n" becomes empty after stripping and is skipped.
    ASSERT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m), 5);

    const std::string raw_key = Utf8ToCp932("　そんな俺を見て、\n&");
    const std::string wire_key = Utf8ToCp932("　そんな俺を見て、\n");
    EXPECT_EQ(m.count(raw_key), 0u);
    ASSERT_EQ(m.count(wire_key), kPresentKeyCount);
    EXPECT_EQ(m[wire_key], "Watching me like that,");

    const std::string wire_key2 = Utf8ToCp932("　まだ日は高い。\n");
    ASSERT_EQ(m.count(wire_key2), kPresentKeyCount);
    EXPECT_EQ(m[wire_key2], "The sun was still high.");

    const std::string prose_key = Utf8ToCp932("ロック＆ロール");
    ASSERT_EQ(m.count(prose_key), kPresentKeyCount);
    EXPECT_EQ(m[prose_key], "Tom & Jerry");

    // A non-\n& key must never have a legitimate trailing " &" value stripped.
    const std::string plain_key = Utf8ToCp932("通常");
    ASSERT_EQ(m.count(plain_key), kPresentKeyCount);
    EXPECT_EQ(m[plain_key], "foo &");

    // Identity "\n&" row drops '&' on both key and value (trailing '\n' stripped from value).
    const std::string id_key = Utf8ToCp932("同一\n");
    EXPECT_EQ(m.count(Utf8ToCp932("同一\n&")), 0u);
    ASSERT_EQ(m.count(id_key), kPresentKeyCount);
    EXPECT_EQ(m[id_key], Utf8ToCp932("同一"));

    EXPECT_EQ(m.count(Utf8ToCp932("空になる\n")), 0u);
    EXPECT_EQ(m.count(Utf8ToCp932("空になる\n&")), 0u);
}
