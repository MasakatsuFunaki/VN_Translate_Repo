// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CP932 codec unit tests.
//
// CP932 maps 0x80, 0xA0 and 0xFD-0xFF to single characters rather than
// rejecting them as invalid Shift-JIS lead bytes.  These cases pin the exact
// single-byte, double-byte, and malformed-trail behavior.
#include "common/util.h"

#include <cstdint>
#include <initializer_list>
#include <string>

#include <gtest/gtest.h>

namespace {

using namespace snt;

constexpr std::uint8_t kCp932Byte80 = 0x80U;
constexpr std::uint8_t kCp932ByteA0 = 0xA0U;
constexpr std::uint8_t kCp932ByteA1 = 0xA1U;
constexpr std::uint8_t kCp932ByteB5 = 0xB5U;
constexpr std::uint8_t kCp932ByteDf = 0xDFU;
constexpr std::uint8_t kCp932ByteFd = 0xFDU;
constexpr std::uint8_t kCp932ByteFe = 0xFEU;
constexpr std::uint8_t kCp932ByteFf = 0xFFU;
constexpr std::uint8_t kAsciiSpace = 0x20U;
constexpr std::uint8_t kAsciiEtx = 0x03U;
constexpr std::uint8_t kAsciiUpperA = 0x41U;
constexpr std::uint8_t kBadLead86 = 0x86U;
constexpr std::uint8_t kBadTrail3A = 0x3AU;
constexpr std::uint8_t kTruncatedLead82 = 0x82U;
constexpr std::uint8_t kLead81 = 0x81U;
constexpr std::uint8_t kOutOfRangeTrailFd = 0xFDU;
constexpr std::uint8_t kUtf8Control80Bytes[] = {0xC2U, 0x80U};

std::string utf8_control_0080() {
    return std::string(
        reinterpret_cast<const char*>(kUtf8Control80Bytes),
        sizeof(kUtf8Control80Bytes));
}

std::string dec_bytes(const Bytes& b) {
    const auto s = cp932_to_utf8_strict(b);
    return s ? *s : std::string("<INVALID>");
}

std::string dec(std::initializer_list<std::uint8_t> bytes) {
    return dec_bytes(Bytes(bytes));
}

bool valid(std::initializer_list<std::uint8_t> bytes) {
    const Bytes b(bytes);
    return cp932_to_utf8_strict(b).has_value();
}

}  // namespace

TEST(Cp932, decodes_special_single_bytes) {
    EXPECT_EQ(dec({kCp932Byte80}), utf8_control_0080());
    EXPECT_EQ(dec({kCp932ByteA0}), "\uF8F0");
    EXPECT_EQ(dec({kCp932ByteFd}), "\uF8F1");
    EXPECT_EQ(dec({kCp932ByteFe}), "\uF8F2");
    EXPECT_EQ(dec({kCp932ByteFf}), "\uF8F3");
}

TEST(Cp932, special_bytes_survive_inside_a_run) {
    EXPECT_TRUE(valid(
        {kCp932ByteB5, kCp932ByteFf, kCp932ByteFf, kCp932ByteFf, kAsciiSpace, kAsciiEtx}));
    EXPECT_EQ(dec({'a', kCp932ByteFf, 'b'}), "a\uF8F3b");

    Bytes hiragana_a_plus_ff = utf8_to_cp932_replace("あ");
    hiragana_a_plus_ff.push_back(kCp932ByteFf);
    EXPECT_EQ(dec_bytes(hiragana_a_plus_ff), "あ\uF8F3");
}

TEST(Cp932, a0_does_not_swallow_the_following_byte) {
    EXPECT_EQ(dec({kCp932ByteA0, kAsciiUpperA}), "\uF8F0A");
}

TEST(Cp932, halfwidth_katakana_singles) {
    EXPECT_EQ(dec({kCp932ByteA1}), "｡");
    EXPECT_EQ(dec({kCp932ByteDf}), "ﾟ");
}

TEST(Cp932, decodes_ordinary_two_byte_characters) {
    for (const char* jp : {"あ", "「", "日", "直哉", "「おはよう」"}) {
        const Bytes encoded = utf8_to_cp932_replace(jp);
        ASSERT_FALSE(encoded.empty());
        EXPECT_EQ(dec_bytes(encoded), jp);
    }
}

TEST(Cp932, rejects_bad_trail_bytes) {
    EXPECT_FALSE(valid({kBadLead86, kBadTrail3A}));
    EXPECT_FALSE(valid({kTruncatedLead82}));
    EXPECT_FALSE(valid({kLead81, kOutOfRangeTrailFd}));
}

TEST(Cp932, empty_input_is_the_empty_string) {
    EXPECT_EQ(dec({}), "");
}

TEST(Cp932, replace_mode_keeps_special_singles) {
    const Bytes b{kCp932ByteFf, kBadLead86, kBadTrail3A};
    const std::string s = cp932_to_utf8_replace(b.data(), b.size());
    EXPECT_EQ(s.rfind("\uF8F3", 0), 0U) << "0xFF should decode, not be replaced";
    EXPECT_NE(s.find("\uFFFD"), std::string::npos) << "0x86 0x3A should be replaced";
}
