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

// NOTE: ConvertsUtf8KeyToCp932 was removed for amakano2pe.
// That test asserts ParseTsvBuffer converts UTF-8 input keys to CP932
// in the map — which is CROSS_CHANNEL's architecture. amakano2pe's
// engine (CatSystem2) hands the hook UTF-8 bytes directly (the hooked
// converter FUN_0063a9b0 internally calls MultiByteToWideChar with
// CP_UTF8), so the map keys must stay UTF-8 to match runtime lookup.
// See src/translator.cpp:14-17 and src/translator_logic.h:9-13.
