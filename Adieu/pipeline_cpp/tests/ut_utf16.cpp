// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UTF-16LE replacement-mode decoding and direct code-unit UTF-8 encoding
// (rejecting lone surrogates rather than emitting WTF-8).
#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

using namespace adi;

namespace {

constexpr const char* kFffd = "\xEF\xBF\xBD";
constexpr const char* kNoncharFfff = "\xEF\xBF\xBF";
constexpr const char* kNoncharFffe = "\xEF\xBF\xBE";
constexpr const char* kFirstSmpScalar = "\xF0\x90\x80\x80";

constexpr std::uint8_t kZeroByte = 0x00U;
constexpr std::uint8_t kHighSurrogateHi = 0xD8U;
constexpr std::uint8_t kLowSurrogateHi = 0xDCU;
constexpr std::uint8_t kAsciiAByte = 0x41U;
constexpr std::uint8_t kAsciiBByte = 0x42U;
constexpr std::uint8_t kByteFf = 0xFFU;
constexpr std::uint8_t kByteFe = 0xFEU;
constexpr std::uint8_t kHiraALo = 0x42U;
constexpr std::uint8_t kHiraAHi = 0x30U;

constexpr std::uint16_t kUnitAsciiA = 0x0041U;
constexpr std::uint16_t kUnitHiraA = 0x3042U;
constexpr std::uint16_t kUnitNoncharFfff = 0xFFFFU;
constexpr std::uint16_t kUnitHighSurrogate = 0xD800U;
constexpr std::uint16_t kUnitLowSurrogate = 0xDC00U;

constexpr std::size_t kZeroCodepoints = 0;
constexpr std::size_t kThreeCodepoints = 3;
constexpr std::size_t kNulPaddedLen = 4;
constexpr std::size_t kSingleCharCount = 1;

constexpr std::uint8_t kUtf8HiraAB0 = 0xE3U;
constexpr std::uint8_t kUtf8HiraAB1 = 0x81U;
constexpr std::uint8_t kUtf8HiraAB2 = 0x82U;

std::string dec(std::initializer_list<std::uint8_t> b) {
    std::vector<std::uint8_t> v(b);
    return utf16le_decode_replace(v.data(), v.size());
}

}  // namespace

// Surrogate and noncharacter probe vectors, plus the odd-byte case:
// one U+FFFD per invalid code unit, resyncing at the next unit.
TEST(Utf16, Utf16ReplaceEmitsOneFffdPerInvalidUnit) {
    EXPECT_EQ(dec({kZeroByte, kHighSurrogateHi}), kFffd);
    EXPECT_EQ(dec({kZeroByte, kLowSurrogateHi}), kFffd);
    EXPECT_EQ(dec({kZeroByte, kHighSurrogateHi, kZeroByte, kLowSurrogateHi}),
              kFirstSmpScalar);
    EXPECT_EQ(dec({kZeroByte, kHighSurrogateHi, kAsciiAByte, kZeroByte}),
              std::string(kFffd) + "A");
    EXPECT_EQ(dec({kZeroByte, kLowSurrogateHi, kZeroByte, kHighSurrogateHi}),
              std::string(kFffd) + kFffd);
    EXPECT_EQ(dec({kByteFf, kByteFf}), kNoncharFfff);
    EXPECT_EQ(dec({kByteFe, kByteFf}), kNoncharFffe);
    EXPECT_EQ(dec({kZeroByte, kHighSurrogateHi, kZeroByte, kHighSurrogateHi,
                   kZeroByte, kLowSurrogateHi}),
              std::string(kFffd) + kFirstSmpScalar);
    EXPECT_EQ(dec({kAsciiAByte, kZeroByte, kAsciiBByte}), std::string("A") + kFffd);
    EXPECT_EQ(dec({}), "");
}

TEST(Utf16, PlainAsciiAndJapaneseRoundTrip) {
    EXPECT_EQ(dec({kAsciiAByte, kZeroByte, kAsciiBByte, kZeroByte}), "AB");
    EXPECT_EQ(dec({kHiraALo, kHiraAHi}), "あ");
}

TEST(Utf16, UnitsToUtf8DirectThrowsOnLoneSurrogate) {
    const std::uint16_t ok[] = {kUnitAsciiA, kUnitHiraA, kUnitNoncharFfff};
    EXPECT_EQ(units_to_utf8_direct(ok, std::size(ok)),
              std::string("Aあ") + kNoncharFfff);

    const std::uint16_t pair[] = {kUnitHighSurrogate, kUnitLowSurrogate};
    EXPECT_THROW(units_to_utf8_direct(pair, std::size(pair)), LoneSurrogateError);
    const std::uint16_t low[] = {kUnitLowSurrogate};
    EXPECT_THROW(units_to_utf8_direct(low, std::size(low)), LoneSurrogateError);
}

TEST(Utf16, CharLenCountsCodepoints) {
    EXPECT_EQ(char_len("あいう"), kThreeCodepoints);
    EXPECT_EQ(char_len("日本語"), kThreeCodepoints);
    EXPECT_EQ(char_len("abc"), kThreeCodepoints);
    EXPECT_EQ(char_len(""), kZeroCodepoints);
}

TEST(Utf16, TrimRightChars) {
    EXPECT_EQ(
        trim_right_chars(std::string("ab\0\0", kNulPaddedLen),
                         std::string(kSingleCharCount, '\0')),
        "ab");
    EXPECT_EQ(trim_right_chars("ab  ", std::string(kSingleCharCount, '\0')), "ab  ");
    EXPECT_EQ(trim_right_chars("ab\n\n", "\n"), "ab");
    EXPECT_EQ(trim_right("ab　"), "ab");
}

TEST(Utf16, Utf8DecodeReplace) {
    const std::uint8_t good[] = {'a', kUtf8HiraAB0, kUtf8HiraAB1, kUtf8HiraAB2};
    EXPECT_EQ(utf8_decode_replace(good, std::size(good)), "aあ");
    const std::uint8_t bad[] = {'a', kByteFf, 'b'};
    EXPECT_EQ(utf8_decode_replace(bad, std::size(bad)), std::string("a") + kFffd + "b");
    const std::uint8_t truncated[] = {kUtf8HiraAB0, kUtf8HiraAB1};
    EXPECT_EQ(utf8_decode_replace(truncated, std::size(truncated)), kFffd);
}

TEST(Utf16, Utf8ToCp932StrictRejectsInvalidUtf8AndPreservesEmpty) {
    constexpr std::uint8_t kCp932HiraALead = 0x82U;
    constexpr std::uint8_t kCp932HiraATrail = 0xA0U;

    const auto empty_encoded = utf8_to_cp932_strict("");
    ASSERT_TRUE(empty_encoded.has_value());
    EXPECT_TRUE(empty_encoded->empty());

    const auto valid_encoded = utf8_to_cp932_strict("あ");
    ASSERT_TRUE(valid_encoded.has_value());
    EXPECT_EQ(*valid_encoded, (Bytes{kCp932HiraALead, kCp932HiraATrail}));

    EXPECT_FALSE(utf8_to_cp932_strict("\xFF").has_value());
    EXPECT_FALSE(utf8_to_cp932_strict("\xE3\x81").has_value());
}
