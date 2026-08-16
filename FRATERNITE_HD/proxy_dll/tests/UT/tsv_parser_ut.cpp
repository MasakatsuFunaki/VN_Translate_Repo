// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — TSV parser tests for FRATERNITE_HD.
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
using translator_logic::ReconstructEnglish;
using translator_logic::EnglishLine;

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

// --- ReconstructEnglish (overlay full-line renderer) ---------------------
//
// Byte strings use explicit lengths so adjacent \x escapes can't merge and
// no embedded null trims them. Speaker name = 2 CP932 glyphs; quote =
// open-kakko (0x81 0x75) + 1 glyph + close-kakko (0x81 0x76).

TEST(ReconstructEnglish, SpeakerAndQuoteSplitsSpeakerFromBody) {
    TranslationMap m;
    std::string spk("\x91\xE5\x92\x71", 4);            // 2-glyph speaker
    std::string quote("\x81\x75\x82\xA0\x81\x76", 6);  // "<glyph>"
    m[spk]   = "Daichi";
    m[quote] = "\"Ah\"";
    EnglishLine el = ReconstructEnglish(spk + quote, m);
    EXPECT_TRUE(el.valid);
    EXPECT_EQ(el.speaker, "Daichi");
    EXPECT_EQ(el.speaker_slots, 2);   // hook suppresses BODY slots from here
    EXPECT_EQ(el.body, "\"Ah\"");
}

TEST(ReconstructEnglish, NarrationHasNoSpeaker) {
    TranslationMap m;
    std::string jp("\x82\xA0\x82\xA2", 4);
    m[jp] = "Hello there.";
    EnglishLine el = ReconstructEnglish(jp, m);
    EXPECT_TRUE(el.valid);
    EXPECT_TRUE(el.speaker.empty());
    EXPECT_EQ(el.speaker_slots, 0);
    EXPECT_EQ(el.body, "Hello there.");
}

TEST(ReconstructEnglish, NoMatchIsInvalid) {
    TranslationMap m;
    m[std::string("\x82\xA0\x82\xA2", 4)] = "x";
    EnglishLine el = ReconstructEnglish(std::string("\xE0\x40\xE0\x41", 4), m);
    EXPECT_FALSE(el.valid);   // caller falls through to engine JP, not a blank
    EXPECT_TRUE(el.body.empty());
}

// NOTE: ConvertsUtf8KeyToCp932 was removed for FRATERNITE_HD.
// That test asserts ParseTsvBuffer converts UTF-8 input keys to CP932
// in the map — which is CROSS_CHANNEL's architecture (UTF-8 TSV on
// disk, parser converts at load). FRATERNITE_HD's pipeline writes the
// TSV in CP932 directly (see the table builder:
// "Format: <jp_cp932>\t<en_cp932>\n") and stores keys verbatim, so
// adding the conversion here would corrupt the already-CP932 keys.
