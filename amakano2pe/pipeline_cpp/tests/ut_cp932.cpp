// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CP932 codec tests.
//
// These exist because an earlier decoder asserted that 0x80, 0xA0 and 0xFD-0xFF
// "are never valid lead bytes" and rejected them.  CP932 maps all five as
// single characters, and ~13% of the BLACKCyc games' extracted strings contain
// at least one of them -- so that decoder silently DROPPED those strings and
// every downstream array index shifted.
#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>

using namespace ama;

namespace {

constexpr std::uint8_t kCp932Special80 = 0x80;
constexpr std::uint8_t kCp932SpecialA0 = 0xA0;
constexpr std::uint8_t kCp932SpecialFd = 0xFD;
constexpr std::uint8_t kCp932SpecialFe = 0xFE;
constexpr std::uint8_t kCp932SpecialFf = 0xFF;

constexpr std::uint8_t kCp932HalfwidthStop = 0xA1;
constexpr std::uint8_t kCp932HalfwidthKatakanaU = 0xB5;
constexpr std::uint8_t kCp932HalfwidthSemiVoiced = 0xDF;

constexpr std::uint8_t kAsciiUpperA = 0x41;
constexpr std::uint8_t kAsciiSpace = 0x20;
constexpr std::uint8_t kAsciiEtx = 0x03;

constexpr std::uint8_t kCp932Lead81 = 0x81;
constexpr std::uint8_t kCp932Lead82 = 0x82;
constexpr std::uint8_t kCp932Lead86 = 0x86;
constexpr std::uint8_t kCp932InvalidTrail3A = 0x3A;
constexpr std::uint8_t kCp932InvalidTrailFd = 0xFD;

constexpr std::uint8_t kUtf8LeadC2 = 0xC2;
constexpr std::uint8_t kUtf8Trail80 = 0x80;

constexpr std::size_t kSingleByteLen = 1;
constexpr std::size_t kTwoByteLen = 2;

std::string dec(const Bytes& b) {
    const auto s = cp932_to_utf8_strict(b);
    return s ? *s : std::string("<INVALID>");
}

std::string dec_bytes(std::initializer_list<std::uint8_t> bytes) {
    return dec(Bytes(bytes));
}

bool valid_bytes(std::initializer_list<std::uint8_t> bytes) {
    const Bytes b(bytes);
    return cp932_to_utf8_strict(b).has_value();
}

}  // namespace

// The five bytes the original structure test wrongly rejected.
TEST(Cp932, decodes_cp932_special_single_bytes) {
    const std::string utf8_u0080{
        static_cast<char>(kUtf8LeadC2),
        static_cast<char>(kUtf8Trail80),
    };
    EXPECT_EQ(dec_bytes({kCp932Special80}), utf8_u0080);
    EXPECT_EQ(dec_bytes({kCp932SpecialA0}), "\uF8F0");
    EXPECT_EQ(dec_bytes({kCp932SpecialFd}), "\uF8F1");
    EXPECT_EQ(dec_bytes({kCp932SpecialFe}), "\uF8F2");
    EXPECT_EQ(dec_bytes({kCp932SpecialFf}), "\uF8F3");
}

// The exact shape that was being dropped: a special byte inside a run of
// otherwise ordinary bytes.
TEST(Cp932, special_bytes_survive_inside_a_run) {
    EXPECT_TRUE(valid_bytes({
        kCp932HalfwidthKatakanaU,
        kCp932SpecialFf,
        kCp932SpecialFf,
        kCp932SpecialFf,
        kAsciiSpace,
        kAsciiEtx,
    }));
    EXPECT_EQ(dec_bytes({'a', kCp932SpecialFf, 'b'}), "a\uF8F3b");

    Bytes hiragana_plus_special = utf8_to_cp932_replace("あ");
    hiragana_plus_special.push_back(kCp932SpecialFf);
    EXPECT_EQ(dec(hiragana_plus_special), "あ\uF8F3");
}

// 0xA0 must be read as a SINGLE byte, not as a lead byte swallowing the next
// one -- getting this wrong shifts every subsequent character.
TEST(Cp932, a0_does_not_swallow_the_following_byte) {
    EXPECT_EQ(dec_bytes({kCp932SpecialA0, kAsciiUpperA}), "\uF8F0A");
}

TEST(Cp932, halfwidth_katakana_singles) {
    EXPECT_EQ(dec_bytes({kCp932HalfwidthStop}), "｡");
    EXPECT_EQ(dec_bytes({kCp932HalfwidthSemiVoiced}), "ﾟ");
}

TEST(Cp932, decodes_ordinary_two_byte_characters) {
    for (const char* literal_jp : {"あ", "「", "日", "ちとせ"}) {
        const Bytes cp932 = utf8_to_cp932_replace(literal_jp);
        ASSERT_FALSE(cp932.empty());
        EXPECT_EQ(dec(cp932), literal_jp);
    }
}

// Still strict where it counts: a lead byte with an out-of-range trail is an
// error, not something to paper over.
TEST(Cp932, rejects_bad_trail_bytes) {
    const char kTruncatedLead[] = {static_cast<char>(kCp932Lead81), '\0'};
    const char kTruncatedHiraganaLead[] = {static_cast<char>(kCp932Lead82), '\0'};
    const char kBadTrail[] = {
        static_cast<char>(kCp932Lead86),
        static_cast<char>(kCp932InvalidTrail3A),
        '\0',
    };
    const char kOutOfRangeTrail[] = {
        static_cast<char>(kCp932Lead81),
        static_cast<char>(kCp932InvalidTrailFd),
        '\0',
    };

    EXPECT_FALSE(cp932_to_utf8_strict(
        reinterpret_cast<const std::uint8_t*>(kTruncatedLead), kSingleByteLen).has_value());
    EXPECT_FALSE(cp932_to_utf8_strict(
        reinterpret_cast<const std::uint8_t*>(kTruncatedHiraganaLead), kSingleByteLen).has_value());
    EXPECT_FALSE(cp932_to_utf8_strict(
        reinterpret_cast<const std::uint8_t*>(kBadTrail), kTwoByteLen).has_value());
    EXPECT_FALSE(cp932_to_utf8_strict(
        reinterpret_cast<const std::uint8_t*>(kOutOfRangeTrail), kTwoByteLen).has_value());
}

TEST(Cp932, empty_input_is_the_empty_string) {
    EXPECT_EQ(dec_bytes({}), "");
}

// Replace mode: the special singles do decode, so they must NOT come back as
// U+FFFD.
TEST(Cp932, replace_mode_keeps_special_singles) {
    const Bytes b{kCp932SpecialFf, kCp932Lead86, kCp932InvalidTrail3A};
    const std::string s = cp932_to_utf8_replace(b.data(), b.size());
    EXPECT_EQ(s.rfind("\uF8F3", 0), 0u) << "0xFF should decode, not be replaced";
    EXPECT_NE(s.find("\uFFFD"), std::string::npos) << "0x86 0x3A should be replaced";
}
