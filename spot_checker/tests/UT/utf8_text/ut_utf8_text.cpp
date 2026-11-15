// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "common/utf8_text.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace spot_checker {
namespace {

constexpr char32_t kAsciiNull = 0x00U;
constexpr char32_t kAsciiMax = 0x7FU;
constexpr char32_t kTwoByteMin = 0x80U;
constexpr char32_t kTwoByteMax = 0x7FFU;
constexpr char32_t kThreeByteMin = 0x800U;
constexpr char32_t kSurrogateLowNeighbor = 0xD7FFU;
constexpr char32_t kSurrogateMin = 0xD800U;
constexpr char32_t kSurrogateMax = 0xDFFFU;
constexpr char32_t kSurrogateHighNeighbor = 0xE000U;
constexpr char32_t kFourByteMin = 0x10000U;
constexpr char32_t kAstralShikaruKanji = 0x20B9FU;  // '𠮟'
constexpr char32_t kAboveMaxUnicode = 0x110000U;

constexpr wchar_t kShikaruHighSurrogate = 0xD842;
constexpr wchar_t kShikaruLowSurrogate = 0xDF9F;
constexpr wchar_t kUnpairedHighMin = 0xD800;
constexpr wchar_t kUnpairedHighMax = 0xDBFF;
constexpr wchar_t kUnpairedLowMin = 0xDC00;

TEST(Utf8Text, DecodesAndAppendsValidRfc3629BoundaryScalarsAndInteriorNul) {
    struct ValidCase {
        const char* label;
        const char* bytes;
        std::size_t len;
        char32_t expected_cp;
    };
    constexpr ValidCase kCases[] = {
        {"ascii_nul_0x00", "\0", 1U, kAsciiNull},
        {"ascii_max_0x7F", "\x7F", 1U, kAsciiMax},
        {"two_byte_min_0x80", "\xC2\x80", 2U, kTwoByteMin},
        {"two_byte_max_0x7FF", "\xDF\xBF", 2U, kTwoByteMax},
        {"three_byte_min_0x800", "\xE0\xA0\x80", 3U, kThreeByteMin},
        {"below_surrogate_0xD7FF", "\xED\x9F\xBF", 3U, kSurrogateLowNeighbor},
        {"above_surrogate_0xE000", "\xEE\x80\x80", 3U, kSurrogateHighNeighbor},
        {"literal_replacement_0xFFFD", "\xEF\xBF\xBD", 3U, kUnicodeReplacementChar},
        {"four_byte_min_0x10000", "\xF0\x90\x80\x80", 4U, kFourByteMin},
        {"astral_kanji_20B9F", "𠮟", 4U, kAstralShikaruKanji},
        {"max_unicode_10FFFF", "\xF4\x8F\xBF\xBF", 4U, kMaxUnicodeCodepoint},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        const std::string_view sv(tc.bytes, tc.len);
        std::size_t idx = 0U;
        EXPECT_EQ(utf8_next(sv, idx), tc.expected_cp);
        EXPECT_EQ(idx, tc.len);
        EXPECT_EQ(utf8_next(sv, idx), U'\0');
        EXPECT_EQ(idx, tc.len);

        std::string encoded;
        append_utf8(encoded, tc.expected_cp);
        EXPECT_EQ(encoded, sv);
    }

    const std::string_view with_nul("A\0語", 5U);
    EXPECT_EQ(utf8_codepoint_count(with_nul), 3U);
    std::size_t pos = 0U;
    EXPECT_EQ(utf8_next(with_nul, pos), U'A');
    EXPECT_EQ(utf8_next(with_nul, pos), U'\0');
    EXPECT_EQ(utf8_next(with_nul, pos), U'語');
    EXPECT_EQ(pos, 5U);

    std::string invalid_encoded;
    append_utf8(invalid_encoded, kSurrogateMin);
    append_utf8(invalid_encoded, kSurrogateMax);
    append_utf8(invalid_encoded, kAboveMaxUnicode);
    EXPECT_EQ(invalid_encoded, "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST(Utf8Text, RejectsIllFormedAndTruncatedSequencesWithoutConsumingFollowingAscii) {
    struct IllFormedBeforeAsciiCase {
        const char* label;
        std::string_view bad_prefix;
        std::string_view ascii_tail;
    };
    constexpr IllFormedBeforeAsciiCase kCases[] = {
        {"lone_cont_0x80", "\x80", "\"%s"},
        {"lone_cont_0xBF", "\xBF", "\"%s"},
        {"overlong_2b_0xC0", "\xC0\x80", "\"%s"},
        {"overlong_2b_0xC1", "\xC1\xBF", "\"%s"},
        {"overlong_3b_0xE0_80", "\xE0\x80\x80", "\"%s"},
        {"overlong_3b_0xE0_9F", "\xE0\x9F\xBF", "\"%s"},
        {"surrogate_min_D800", "\xED\xA0\x80", "\"%s"},
        {"surrogate_max_DFFF", "\xED\xBF\xBF", "\"%s"},
        {"overlong_4b_0xF0_8F", "\xF0\x8F\xBF\xBF", "\"%s"},
        {"above_max_0xF4_90", "\xF4\x90\x80\x80", "\"%s"},
        {"invalid_lead_0xF5", "\xF5", "\"%s"},
        {"invalid_lead_0xFF", "\xFF", "\"%s"},
        {"truncated_2b_before_quote", "\xC2", "\"%s"},
        {"truncated_2b_before_percent", "\xC2", "%s"},
        {"truncated_3b_before_quote", "\xE3\x81", "\"%s"},
        {"truncated_3b_before_percent", "\xE3\x81", "%s"},
        {"truncated_4b_before_quote", "\xF0\xA0\xAE", "\"%s"},
        {"truncated_4b_before_percent", "\xF0\xA0\xAE", "%s"},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        const std::string combined =
            std::string(tc.bad_prefix) + std::string(tc.ascii_tail);
        std::size_t idx = 0U;
        while (idx < tc.bad_prefix.size()) {
            const std::size_t prev = idx;
            EXPECT_EQ(utf8_next(combined, idx), kUnicodeReplacementChar);
            ASSERT_GT(idx, prev);
        }
        ASSERT_EQ(idx, tc.bad_prefix.size());

        std::string recovered_tail;
        while (idx < combined.size()) {
            const std::size_t prev = idx;
            const char32_t cp = utf8_next(combined, idx);
            ASSERT_GT(idx, prev);
            ASSERT_LT(cp, 0x80U);
            recovered_tail.push_back(static_cast<char>(cp));
        }
        EXPECT_EQ(recovered_tail, tc.ascii_tail);
    }

    constexpr std::string_view kTruncatedAtEnd[] = {
        "\xC2",
        "\xE3\x81",
        "\xF0\xA0\xAE",
    };
    for (const std::string_view truncated : kTruncatedAtEnd) {
        std::size_t idx = 0U;
        EXPECT_EQ(utf8_next(truncated, idx), kUnicodeReplacementChar);
        EXPECT_EQ(idx, truncated.size());
    }
}

TEST(Utf8Text, ConvertsWideAndUtf8AcrossBmpSurrogatePairsAndUnpairedSurrogates) {
    EXPECT_EQ(wide_to_utf8(L"和倉 賢一"), "和倉 賢一");
    EXPECT_EQ(utf8_to_wide("和倉 賢一"), L"和倉 賢一");

    const std::wstring astral_wide{kShikaruHighSurrogate, kShikaruLowSurrogate};
    EXPECT_EQ(wide_to_utf8(astral_wide), "𠮟");
    EXPECT_EQ(utf8_to_wide("𠮟"), astral_wide);

    const std::wstring high_before_ascii{kUnpairedHighMin, L'A'};
    EXPECT_EQ(wide_to_utf8(high_before_ascii), "\xEF\xBF\xBD" "A");

    const std::wstring high_at_end{kUnpairedHighMax};
    EXPECT_EQ(wide_to_utf8(high_at_end), "\xEF\xBF\xBD");

    const std::wstring low_before_ascii{kUnpairedLowMin, L'Z'};
    EXPECT_EQ(wide_to_utf8(low_before_ascii), "\xEF\xBF\xBD" "Z");

    const std::wstring high_then_valid_pair{
        kUnpairedHighMin, kShikaruHighSurrogate, kShikaruLowSurrogate};
    EXPECT_EQ(wide_to_utf8(high_then_valid_pair), "\xEF\xBF\xBD" "𠮟");

    const std::wstring expected_recovered{
        static_cast<wchar_t>(kUnicodeReplacementChar), L'"', L'o', L'k'};
    EXPECT_EQ(utf8_to_wide("\xE3\x81" "\"ok"), expected_recovered);

    const std::wstring_view wide_nul(L"A\0B", 3U);
    const std::string_view utf8_nul("A\0B", 3U);
    EXPECT_EQ(wide_to_utf8(wide_nul), utf8_nul);
    EXPECT_EQ(utf8_to_wide(utf8_nul), wide_nul);
}

TEST(Utf8Text, ClassifiesJapaneseScriptsAndPunctuationBoundaries) {
    struct CpCase {
        char32_t cp;
        bool hiragana;
        bool katakana;
        bool lexical_katakana;
        bool kanji;
    };
    constexpr CpCase kCpCases[] = {
        {0x3004U, false, false, false, false},
        {U'々', false, false, false, true},
        {U'〆', false, false, false, false},
        {0x303FU, false, false, false, false},
        {0x3040U, true, false, false, false},
        {0x309FU, true, false, false, false},
        {U'゠', false, true, false, false},
        {U'ァ', false, true, true, false},
        {U'・', false, true, false, false},
        {U'ー', false, true, false, false},
        {0x30FFU, false, true, true, false},
        {0x3100U, false, false, false, false},
        {U'ㇰ', false, true, true, false},
        {0x33FFU, false, false, false, false},
        {0x3400U, false, false, false, true},
        {0x4DBFU, false, false, false, true},
        {0x4DC0U, false, false, false, false},
        {U'一', false, false, false, true},
        {0x9FFFU, false, false, false, true},
        {0xF900U, false, false, false, true},
        {0xFF65U, false, false, false, false},
        {U'ｦ', false, true, true, false},
        {U'ｰ', false, true, false, false},
        {U'ｱ', false, true, true, false},
        {0xFF9FU, false, true, true, false},
        {0xFFA0U, false, false, false, false},
        {0x1FFFFU, false, false, false, false},
        {0x20000U, false, false, false, true},
        {kAstralShikaruKanji, false, false, false, true},
        {0x2A6DFU, false, false, false, true},
        {0x2A6E0U, false, false, false, false},
    };

    for (const auto& tc : kCpCases) {
        SCOPED_TRACE(static_cast<std::uint32_t>(tc.cp));
        EXPECT_EQ(is_hiragana(tc.cp), tc.hiragana);
        EXPECT_EQ(is_katakana(tc.cp), tc.katakana);
        EXPECT_EQ(is_lexical_katakana(tc.cp), tc.lexical_katakana);
        EXPECT_EQ(is_kanji(tc.cp), tc.kanji);
    }

    struct StringClassCase {
        const char* label;
        std::string_view text;
        bool any_jp;
        bool real_jp;
        bool pure_silence;
        bool cjk_punct;
    };
    constexpr StringClassCase kStrCases[] = {
        {"cjk_ellipsis_quotes", "「……」", true, false, true, true},
        {"katakana_dots_only", "「・・・」", true, false, true, true},
        {"prolongation_only", "「ー」", true, false, true, true},
        {"ascii_dots", "...", false, false, true, false},
        {"ascii_word_and_dots", "Wait...", false, false, false, false},
        {"iteration_mark_compound", "人々", true, true, false, false},
        {"iteration_mark_alone", "々", true, true, false, false},
        {"halfwidth_katakana_sfx", "ｶﾞﾀｯ", true, true, false, false},
        {"astral_kanji", "𠮟", true, true, false, false},
        {"horizontal_bar_2015", "Wait\u2015now", false, false, false, true},
        {"box_drawing_2500", "Wait\u2500now", false, false, false, true},
        {"wave_dash_301C", "Ah\u301C", true, false, false, true},
        {"fullwidth_tilde_FF5E", "Ah\uFF5E", true, false, false, true},
        {"halfwidth_cjk_stop_FF61", "End\uFF61", true, false, false, true},
        {"macronized_english", "tōi café", false, false, false, false},
    };

    for (const auto& tc : kStrCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(has_japanese(tc.text), tc.any_jp);
        EXPECT_EQ(has_real_japanese(tc.text), tc.real_jp);
        EXPECT_EQ(is_pure_silence_or_punctuation(tc.text), tc.pure_silence);
        EXPECT_EQ(has_cjk_or_fullwidth_punctuation(tc.text), tc.cjk_punct);
    }
}

TEST(Utf8Text, EnforcesUtf8WordBoundariesAndSanitizesPunctuation) {
    EXPECT_EQ(trim_unicode("\uFEFF\u3000 \tHello\u3000World\r\n\u3000"),
              "Hello\u3000World");
    EXPECT_EQ(trim_unicode("\uFEFF\u3000 \r\n"), "");
    EXPECT_EQ(to_lower_ascii("ÉCLAIR 東京 AZ09"), "Éclair 東京 az09");

    struct WordMatchCase {
        const char* label;
        std::string_view haystack;
        std::string_view needle;
        bool expected;
    };
    constexpr WordMatchCase kWordCases[] = {
        {"macron_prevents_trailing_i_match", "That place is tōi.", "i", false},
        {"macron_prevents_leading_t_match", "tōi", "t", false},
        {"accent_prevents_prefix_match", "A cliché ending", "clich", false},
        {"quoted_pronoun_i_matches", "He said, 'I see.'", "i", true},
        {"case_insensitive_end_boundary", "Walk with HER.", "her", true},
        {"substring_inside_word_rejected", "HEROIN", "her", false},
        {"empty_needle_rejected", "Hello world", "", false},
        {"needle_longer_than_haystack", "Hi", "High", false},
    };
    for (const auto& tc : kWordCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(contains_word_ascii_ci(tc.haystack, tc.needle), tc.expected);
    }

    const std::string raw =
        "\u201CWait\u2014no\u2013really\u2015stop\u2500now\u2026\u2025\u201D"
        "\u3000\u2018It\u2019s\u301Cfine\uFF5E\u2019";
    const std::string sanitized = sanitize_ascii_punctuation(raw);
    EXPECT_EQ(sanitized,
              "\"Wait--no--really--stop--now......\" 'It's~fine~'");
    EXPECT_FALSE(has_cjk_or_fullwidth_punctuation(sanitized));
}

}  // namespace
}  // namespace spot_checker
