// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// TSV parser unit tests for CROSS_CHANNEL: backslash-escape decoding
// (UnescapeInPlace) and line-by-line parsing (ParseTsvBuffer) on synthetic
// in-memory buffers.
#include "translator_logic.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

using translator_logic::ParseTsvBuffer;
using translator_logic::TranslationMap;
using translator_logic::UnescapeInPlace;

namespace {

constexpr int kSingleParsedRow = 1;
constexpr int kTwoParsedRows = 2;

#ifdef _WIN32
constexpr UINT kCodePageCp932 = 932;

std::string to_cp932(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    BOOL used_default = FALSE;
    const int needed = ::WideCharToMultiByte(
        kCodePageCp932, WC_NO_BEST_FIT_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, &used_default);
    if (needed <= 0 || used_default != FALSE) {
        throw std::runtime_error("WideCharToMultiByte(932) failed");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(
        kCodePageCp932, WC_NO_BEST_FIT_CHARS, wide.data(),
        static_cast<int>(wide.size()), out.data(), needed, nullptr, &used_default);
    if (written != needed || used_default != FALSE) {
        throw std::runtime_error("WideCharToMultiByte(932) conversion mismatch");
    }
    return out;
}
#endif

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
    EXPECT_EQ(ParseTsvBuffer("", 0, m), 0);
    EXPECT_TRUE(m.empty());
}

TEST(ParseTsv, AsciiOnly) {
    TranslationMap m;
    const std::string tsv = "hello\tHello\nworld\tWorld\n";
    const int n = ParseTsvBuffer(tsv.data(), tsv.size(), m);
    EXPECT_EQ(n, kTwoParsedRows);
    EXPECT_EQ(m["hello"], "Hello");
    EXPECT_EQ(m["world"], "World");
}

TEST(ParseTsv, EscapedNewline) {
    TranslationMap m;
    const std::string tsv = "a\\nb\tA\\nB\n";
    EXPECT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m), kSingleParsedRow);
    EXPECT_EQ(m["a\nb"], "A\nB");
}

TEST(ParseTsv, CrlfLineEndings) {
    TranslationMap m;
    const std::string tsv = "x\tX\r\ny\tY\r\n";
    EXPECT_EQ(ParseTsvBuffer(tsv.data(), tsv.size(), m), kTwoParsedRows);
    EXPECT_EQ(m["x"], "X");
    EXPECT_EQ(m["y"], "Y");
}

TEST(ParseTsv, EmptyOrMalformedLinesIgnored) {
    TranslationMap m;
    const std::string tsv = "good\tValue\nno-tab\nempty-jp\t\n\tempty-en\n";
    const int n = ParseTsvBuffer(tsv.data(), tsv.size(), m);
    EXPECT_EQ(n, kSingleParsedRow);
    EXPECT_EQ(m["good"], "Value");
}

TEST(ParseTsv, ConvertsUtf8KeyToCp932) {
    TranslationMap m;
    const std::string tsv = "あ\thello\n";
    const int n = ParseTsvBuffer(tsv.data(), tsv.size(), m);
    EXPECT_EQ(n, kSingleParsedRow);
#ifdef _WIN32
    const std::string cp932_a = to_cp932(L"あ");
    EXPECT_EQ(m.count(cp932_a), 1u);
    EXPECT_EQ(m[cp932_a], "hello");
#else
    EXPECT_EQ(m["あ"], "hello");
#endif
}
