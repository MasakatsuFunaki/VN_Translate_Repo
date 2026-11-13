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
