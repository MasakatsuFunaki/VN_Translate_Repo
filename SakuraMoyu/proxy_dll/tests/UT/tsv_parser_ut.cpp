// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for FAVORITE translator_logic: TSV escape decoding, TSV buffer
// parsing, CP932 lookup key normalization, and 58-column grid word wrapping.
#include "translator_logic.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

using translator_logic::ParseTsvBuffer;
using translator_logic::TranslationMap;
using translator_logic::UnescapeInPlace;

namespace {

constexpr UINT kCp932CodePage = 932;
constexpr DWORD kWideConversionFlags = WC_NO_BEST_FIT_CHARS;
constexpr std::size_t kEmptyBufferLen = 0u;
constexpr std::size_t kNullTerminatorLen = 1u;
constexpr int kZeroEntries = 0;
constexpr int kSingleEntry = 1;
constexpr int kTwoEntries = 2;
constexpr std::size_t kFvpGridColumns = translator_logic::kDefaultFvpColumnGridWidth;
constexpr std::size_t kFirstRowStart = 0u;
constexpr std::size_t kMinWrappedAloneSize = 64u;
constexpr std::size_t kMinWrappedVividSize = 72u;

std::string WideToCp932(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    BOOL used_default = FALSE;
    const int needed = WideCharToMultiByte(
        kCp932CodePage,
        kWideConversionFlags,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        &used_default);
    if (needed <= 0 || used_default != FALSE) {
        throw std::runtime_error("WideToCp932 size calculation failed");
    }
    std::string cp932(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        kCp932CodePage,
        kWideConversionFlags,
        wide.data(),
        static_cast<int>(wide.size()),
        cp932.data(),
        needed,
        nullptr,
        &used_default);
    if (written != needed || used_default != FALSE) {
        throw std::runtime_error("WideToCp932 conversion failed");
    }
    return cp932;
}

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
    EXPECT_EQ(ParseTsvBuffer("", kEmptyBufferLen, m), kZeroEntries);
    EXPECT_TRUE(m.empty());
}

TEST(ParseTsv, AsciiOnly) {
    TranslationMap m;
    const char tsv[] = "hello\tHello\nworld\tWorld\n";
    const int n = ParseTsvBuffer(tsv, sizeof(tsv) - kNullTerminatorLen, m);
    EXPECT_EQ(n, kTwoEntries);
    EXPECT_EQ(m["hello"], "Hello");
    EXPECT_EQ(m["world"], "World");
}

TEST(ParseTsv, EscapedNewline) {
    TranslationMap m;
    const char tsv[] = "a\\nb\tA\\nB\n";
    EXPECT_EQ(ParseTsvBuffer(tsv, sizeof(tsv) - kNullTerminatorLen, m), kSingleEntry);
    EXPECT_EQ(m["a\nb"], "A\nB");
}

TEST(ParseTsv, CrlfLineEndings) {
    TranslationMap m;
    const char tsv[] = "x\tX\r\ny\tY\r\n";
    EXPECT_EQ(ParseTsvBuffer(tsv, sizeof(tsv) - kNullTerminatorLen, m), kTwoEntries);
    EXPECT_EQ(m["x"], "X");
    EXPECT_EQ(m["y"], "Y");
}

TEST(ParseTsv, EmptyOrMalformedLinesIgnored) {
    TranslationMap m;
    const char tsv[] = "good\tValue\nno-tab\nempty-jp\t\n\tempty-en\n";
    const int n = ParseTsvBuffer(tsv, sizeof(tsv) - kNullTerminatorLen, m);
    EXPECT_EQ(n, kSingleEntry);
    EXPECT_EQ(m["good"], "Value");
}

TEST(NormalizeCp932, StripsMixedAsciiAndFullwidthSpacesBothEnds) {
    const std::string input = WideToCp932(L"　 ？？？ 　");
    const std::string expected = WideToCp932(L"？？？");
    EXPECT_EQ(translator_logic::NormalizeCp932LookupKey(input.data(), input.size()),
              expected);
}

TEST(NormalizeCp932, UnwrapsCornerBracketsAndInnerSpaces) {
    const std::string kagi = WideToCp932(L"「…………」");
    const std::string expected_dots = WideToCp932(L"…………");
    EXPECT_EQ(translator_logic::NormalizeCp932LookupKey(kagi.data(), kagi.size()),
              expected_dots);

    const std::string nijuu = WideToCp932(L"『　クロ　』");
    const std::string expected_kuro = WideToCp932(L"クロ");
    EXPECT_EQ(translator_logic::NormalizeCp932LookupKey(nijuu.data(), nijuu.size()),
              expected_kuro);
}

TEST(NormalizeCp932, PreservesKanjiWith0x40TrailByte) {
    const std::string input = WideToCp932(L"　占　");
    const std::string expected = WideToCp932(L"占");
    EXPECT_EQ(translator_logic::NormalizeCp932LookupKey(input.data(), input.size()),
              expected);
}

TEST(WrapAsciiToColumnGrid, WrapsScreenshotDialogueAtColumn58) {
    const std::string s1 =
        "It's okay. I promise, I promise. I won't ever let you be alone.";
    const std::string w1 =
        translator_logic::WrapAsciiToColumnGrid(s1, kFvpGridColumns);
    ASSERT_GE(w1.size(), kMinWrappedAloneSize);
    EXPECT_EQ(w1.substr(kFirstRowStart, kFvpGridColumns),
              "It's okay. I promise, I promise. I won't ever let you be  ");
    EXPECT_EQ(w1.substr(kFvpGridColumns), "alone.");

    const std::string s2 =
        "The world that peeked back at us through the window was vivid and alive.";
    const std::string w2 =
        translator_logic::WrapAsciiToColumnGrid(s2, kFvpGridColumns);
    ASSERT_GE(w2.size(), kMinWrappedVividSize);
    EXPECT_EQ(w2.substr(kFirstRowStart, kFvpGridColumns),
              "The world that peeked back at us through the window was   ");
    EXPECT_EQ(w2.substr(kFvpGridColumns), "vivid and alive.");
}

TEST(WrapAsciiToColumnGrid, LeavesShortStringsUnchangedAndHandlesExactBoundary) {
    EXPECT_EQ(translator_logic::WrapAsciiToColumnGrid("Chiwa", kFvpGridColumns),
              "Chiwa");
    const std::string exact58 = std::string(kFvpGridColumns, 'a') + " next";
    const std::string wrapped =
        translator_logic::WrapAsciiToColumnGrid(exact58, kFvpGridColumns);
    EXPECT_EQ(wrapped.substr(kFirstRowStart, kFvpGridColumns),
              std::string(kFvpGridColumns, 'a'));
    EXPECT_EQ(wrapped.substr(kFvpGridColumns), "next");
}
