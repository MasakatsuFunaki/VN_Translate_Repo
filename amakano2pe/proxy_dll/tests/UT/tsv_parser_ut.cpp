// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — TSV parser tests for amakano2pe.
//
// Tests the TSV-format handling in translator_logic: backslash-escape
// decoding (UnescapeInPlace) and the line-by-line parser (ParseTsvBuffer).
// All inputs are synthetic in-memory buffers; no file I/O.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using translator_logic::ParseTsvBuffer;
using translator_logic::TranslationMap;
using translator_logic::UnescapeInPlace;

constexpr int kZeroEntries = 0;
constexpr int kOneEntry = 1;
constexpr int kTwoEntries = 2;

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
    EXPECT_EQ(ParseTsvBuffer("", 0, m), kZeroEntries);
    EXPECT_TRUE(m.empty());
}

TEST(ParseTsv, AsciiOnly) {
    TranslationMap m;
    constexpr std::string_view kTsv = "hello\tHello\nworld\tWorld\n";
    const int n = ParseTsvBuffer(kTsv.data(), kTsv.size(), m);
    EXPECT_EQ(n, kTwoEntries);
    EXPECT_EQ(m["hello"], "Hello");
    EXPECT_EQ(m["world"], "World");
}

TEST(ParseTsv, EscapedNewline) {
    TranslationMap m;
    constexpr std::string_view kTsv = "a\\nb\tA\\nB\n";
    EXPECT_EQ(ParseTsvBuffer(kTsv.data(), kTsv.size(), m), kOneEntry);
    EXPECT_EQ(m["a\nb"], "A\nB");
}

TEST(ParseTsv, CrlfLineEndings) {
    TranslationMap m;
    constexpr std::string_view kTsv = "x\tX\r\ny\tY\r\n";
    EXPECT_EQ(ParseTsvBuffer(kTsv.data(), kTsv.size(), m), kTwoEntries);
    EXPECT_EQ(m["x"], "X");
    EXPECT_EQ(m["y"], "Y");
}

TEST(ParseTsv, EmptyOrMalformedLinesIgnored) {
    TranslationMap m;
    constexpr std::string_view kTsv =
        "good\tValue\nno-tab\nempty-jp\t\n\tempty-en\n";
    const int n = ParseTsvBuffer(kTsv.data(), kTsv.size(), m);
    EXPECT_EQ(n, kOneEntry);
    EXPECT_EQ(m["good"], "Value");
}

// CatSystem2 passes UTF-8 bytes directly into FUN_0063a9b0 (which converts
// via MultiByteToWideChar(CP_UTF8, ...)), so ParseTsvBuffer preserves UTF-8
// keys verbatim without CP932 re-encoding. See src/translator.cpp and
// src/translator_logic.h.
