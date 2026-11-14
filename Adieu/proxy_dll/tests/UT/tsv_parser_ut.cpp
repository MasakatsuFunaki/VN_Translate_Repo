// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for UTF-8 <-> UTF-16 codec validation, TSV escape decoding,
// TranslationTable buffer parsing, and SiglusEngine runtime bypass predicates.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace {

using translator_logic::ShouldBypassTranslation;
using translator_logic::TranslationTable;
using translator_logic::UnescapeInPlace;
using translator_logic::Utf16ToUtf8;
using translator_logic::Utf8ToUtf16;

constexpr std::size_t kZeroEntries = 0u;
constexpr std::size_t kSingleEntry = 1u;
constexpr std::size_t kTwoEntries = 2u;
constexpr std::size_t kThreeEntries = 3u;

}  // namespace

TEST(Unescape, DecodesNarrowAndWideEscapeSequences) {
    std::string narrow = "a\\nb\\tc\\\\d\\re";
    UnescapeInPlace(narrow);
    EXPECT_EQ(narrow, "a\nb\tc\\d\re");

    std::wstring wide = L"一行目\\n二行目\\t値\\\\末\\r終";
    UnescapeInPlace(wide);
    EXPECT_EQ(wide, L"一行目\n二行目\t値\\末\r終");
}

TEST(Unescape, NoEscapesLeavesTextUnchanged) {
    std::string narrow = "plain text";
    UnescapeInPlace(narrow);
    EXPECT_EQ(narrow, "plain text");

    std::wstring wide = L"さよならを教えて";
    UnescapeInPlace(wide);
    EXPECT_EQ(wide, L"さよならを教えて");
}

TEST(Unescape, PreservesUnknownEscapePairsAndTrailingBackslash) {
    std::string narrow = "a\\qb\\";
    UnescapeInPlace(narrow);
    EXPECT_EQ(narrow, "a\\qb\\");

    std::wstring wide = L"テスト\\z末尾\\";
    UnescapeInPlace(wide);
    EXPECT_EQ(wide, L"テスト\\z末尾\\");
}

TEST(ParseTsv, EmptyAndWhitespaceBuffersYieldZeroEntries) {
    TranslationTable table;
    EXPECT_EQ(table.ParseTsvBytes(""), kZeroEntries);
    EXPECT_TRUE(table.Empty());
    EXPECT_EQ(table.Size(), kZeroEntries);

    EXPECT_EQ(table.ParseTsvBytes("\xEF\xBB\xBF"), kZeroEntries);
    EXPECT_EQ(table.ParseTsvBytes("\r\n\n\r\n"), kZeroEntries);
    EXPECT_TRUE(table.Empty());
}

TEST(ParseTsv, StripsUtf8BomAndParsesCrlfLfAndUnterminatedFinalRow) {
    TranslationTable table;
    const std::string tsv =
        "\xEF\xBB\xBF"
        "人見\tHitomi\r\n"
        "広介\tKousuke\n"
        "睦　月\tMutsuki";

    EXPECT_EQ(table.ParseTsvBytes(tsv), kThreeEntries);
    EXPECT_EQ(table.Size(), kThreeEntries);
    EXPECT_FALSE(table.Empty());

    const std::wstring* hitomi = table.Find(L"人見");
    ASSERT_NE(hitomi, nullptr);
    EXPECT_EQ(*hitomi, L"Hitomi");

    const std::wstring* kousuke = table.Find(L"広介");
    ASSERT_NE(kousuke, nullptr);
    EXPECT_EQ(*kousuke, L"Kousuke");

    const std::wstring* mutsuki = table.Find(L"睦　月");
    ASSERT_NE(mutsuki, nullptr);
    EXPECT_EQ(*mutsuki, L"Mutsuki");
}

TEST(ParseTsv, UnescapesMultilineJapaneseAndEnglishColumns) {
    TranslationTable table;
    const std::string tsv =
        "　夢だ。\\n　目が覚めた。\tIt was a dream.\\nI woke up.\n";

    ASSERT_EQ(table.ParseTsvBytes(tsv), kSingleEntry);
    const std::wstring* en = table.Find(L"　夢だ。\n　目が覚めた。");
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(*en, L"It was a dream.\nI woke up.");
}

TEST(ParseTsv, SkipsMalformedEmptyAndIllFormedUtf8RowsWhileKeepingValidRows) {
    TranslationTable table;
    std::string tsv;
    tsv += "保健室\tInfirmary\n";
    tsv += "row_without_tab_delimiter\n";
    tsv += "空の値\t\n";
    tsv += "\tEmptyKey\n";
    tsv += "\xE3\x81\tTruncatedUtf8Key\n";
    tsv += "不正値\t\xC0\xAF\n";
    tsv += "サロゲート\t\xED\xA0\x80\n";
    tsv += "図書室\tLibrary\r\n";

    EXPECT_EQ(table.ParseTsvBytes(tsv), kTwoEntries);
    EXPECT_EQ(table.Size(), kTwoEntries);

    const std::wstring* infirmary = table.Find(L"保健室");
    ASSERT_NE(infirmary, nullptr);
    EXPECT_EQ(*infirmary, L"Infirmary");

    const std::wstring* library = table.Find(L"図書室");
    ASSERT_NE(library, nullptr);
    EXPECT_EQ(*library, L"Library");

    EXPECT_EQ(table.Find(L"空の値"), nullptr);
    EXPECT_EQ(table.Find(L"不正値"), nullptr);
    EXPECT_EQ(table.Find(L"サロゲート"), nullptr);
}

TEST(ParseTsv, EnforcesFirstWinsSemanticsOnDuplicateKeys) {
    TranslationTable table;
    const std::string tsv =
        "教室\tClassroom\n"
        "教室\tHomeroom\n";

    EXPECT_EQ(table.ParseTsvBytes(tsv), kSingleEntry);
    EXPECT_EQ(table.Size(), kSingleEntry);

    const std::wstring* en = table.Find(L"教室");
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(*en, L"Classroom");
}

TEST(TsvParser, StrictUtf8Utf16CodecRoundTripsAndRejectsInvalidSequences) {
    // BMP Japanese + supplemental-plane cherry blossom U+1F338 (UTF-8 F0 9F 8C B8).
    const std::string valid_utf8 = "さよなら\xF0\x9F\x8C\xB8";
    std::wstring wide;
    ASSERT_TRUE(Utf8ToUtf16(valid_utf8, wide));
    ASSERT_EQ(wide.size(), 6u);
    EXPECT_EQ(wide.substr(0u, 4u), L"さよなら");
    EXPECT_EQ(wide[4], static_cast<wchar_t>(0xD83Cu));
    EXPECT_EQ(wide[5], static_cast<wchar_t>(0xDF38u));

    std::string roundtrip_utf8;
    ASSERT_TRUE(Utf16ToUtf8(wide, roundtrip_utf8));
    EXPECT_EQ(roundtrip_utf8, valid_utf8);

    // Malformed UTF-8 inputs must return false and clear `wide`.
    EXPECT_FALSE(Utf8ToUtf16(std::string_view("\xC0\xAF", 2u), wide));
    EXPECT_TRUE(wide.empty());
    EXPECT_FALSE(Utf8ToUtf16(std::string_view("\xE3\x81", 2u), wide));
    EXPECT_TRUE(wide.empty());
    EXPECT_FALSE(Utf8ToUtf16(std::string_view("\xED\xA0\x80", 3u), wide));
    EXPECT_TRUE(wide.empty());

    // Unpaired UTF-16 surrogates must return false and clear `roundtrip_utf8`.
    const wchar_t lone_high[] = {static_cast<wchar_t>(0xD800u), L'\0'};
    const wchar_t lone_low[] = {static_cast<wchar_t>(0xDC00u), L'\0'};
    EXPECT_FALSE(Utf16ToUtf8(std::wstring_view(lone_high, 1u), roundtrip_utf8));
    EXPECT_TRUE(roundtrip_utf8.empty());
    EXPECT_FALSE(Utf16ToUtf8(std::wstring_view(lone_low, 1u), roundtrip_utf8));
    EXPECT_TRUE(roundtrip_utf8.empty());
}

TEST(TsvParser, ShouldBypassTranslationRespectsAsciiAndNamaeMacroBoundaries) {
    struct BypassCase {
        std::wstring_view input;
        bool expected_bypass;
        const char* reason;
    };

    constexpr BypassCase kCases[] = {
        {L"", true, "empty string"},
        {L"seen020", true, "ASCII scene identifier"},
        {L"MENU01", true, "ASCII g00 asset identifier"},
        {L"bg_sch01", true, "ASCII background token"},
        {L"\x7F", true, "upper ASCII boundary U+007F"},
        {L"\u0080", false, "lowest non-ASCII code point U+0080"},
        {L"＊Ａ", true, "protagonist surname macro key"},
        {L"＊Ｂ", true, "protagonist given-name macro key"},
        {L"＊Ｃ", false, "adjacent fullwidth token outside #NAMAE pair"},
        {L"＊", false, "lone fullwidth asterisk"},
        {L"＊Ａ ", false, "macro key with trailing space"},
        {L"人見", false, "default protagonist surname"},
        {L"広介", false, "default protagonist given name"},
        {L"睦　月", false, "spaced heroine speaker plate"},
        {L"　夢だ。", false, "ideographic-space indented narration"},
    };

    for (const auto& tc : kCases) {
        EXPECT_EQ(ShouldBypassTranslation(tc.input), tc.expected_bypass)
            << "case: " << tc.reason;
    }
}

TEST(TsvParser, CopyAndMoveOperationsPreserveValueSemantics) {
    TranslationTable original;
    ASSERT_EQ(original.ParseTsvBytes("屋上\tRooftop\n中庭\tCourtyard\n"),
              kTwoEntries);

    TranslationTable copied(original);
    original.Clear();
    EXPECT_TRUE(original.Empty());
    EXPECT_EQ(copied.Size(), kTwoEntries);
    ASSERT_NE(copied.Find(L"屋上"), nullptr);
    EXPECT_EQ(*copied.Find(L"屋上"), L"Rooftop");

    TranslationTable moved(std::move(copied));
    EXPECT_EQ(moved.Size(), kTwoEntries);
    ASSERT_NE(moved.Find(L"中庭"), nullptr);
    EXPECT_EQ(*moved.Find(L"中庭"), L"Courtyard");
}
