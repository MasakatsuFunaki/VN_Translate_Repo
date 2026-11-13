// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CP932 codec unit tests.
//
// These exist because a naive decoder asserts that 0x80, 0xA0 and 0xFD-0xFF
// "are never valid lead bytes" and rejects them.  CP932 decodes all five as
// single characters, and ~13% of the BLACKCyc games' extracted strings contain
// at least one of them -- so such a decoder silently DROPS those strings and
// every downstream array index shifts.
#include "common/util.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>

using namespace fav;

namespace {

constexpr std::array<std::uint8_t, 2> kUtf8Control0080 = {0xC2u, 0x80u};
constexpr std::uint8_t kCp932Byte80 = 0x80u;
constexpr std::uint8_t kCp932ByteA0 = 0xA0u;
constexpr std::uint8_t kCp932ByteA1 = 0xA1u;
constexpr std::uint8_t kCp932ByteB5 = 0xB5u;
constexpr std::uint8_t kCp932ByteDF = 0xDFu;
constexpr std::uint8_t kCp932ByteFD = 0xFDu;
constexpr std::uint8_t kCp932ByteFE = 0xFEu;
constexpr std::uint8_t kCp932ByteFF = 0xFFu;
constexpr std::uint8_t kCp932Space = 0x20u;
constexpr std::uint8_t kCp932Etx = 0x03u;
constexpr std::uint8_t kCp932AsciiA = 0x41u;
constexpr std::uint8_t kCp932HiraganaALead = 0x82u;
constexpr std::uint8_t kCp932HiraganaATrail = 0xA0u;
constexpr std::uint8_t kCp932InvalidLead = 0x86u;
constexpr std::uint8_t kCp932InvalidTrail = 0x3Au;
constexpr std::uint8_t kCp932PunctLead = 0x81u;
constexpr std::size_t kPrefixMatchStart = 0u;

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

// The five bytes a bare structure test wrongly rejects.
TEST(Cp932, decodes_special_single_bytes) {
    const std::string expected_0080(
        reinterpret_cast<const char*>(kUtf8Control0080.data()),
        kUtf8Control0080.size());
    EXPECT_EQ(dec({kCp932Byte80}), expected_0080);
    EXPECT_EQ(dec({kCp932ByteA0}), "\uF8F0");
    EXPECT_EQ(dec({kCp932ByteFD}), "\uF8F1");
    EXPECT_EQ(dec({kCp932ByteFE}), "\uF8F2");
    EXPECT_EQ(dec({kCp932ByteFF}), "\uF8F3");
}

// The exact shape that was being dropped: a special byte inside a run of
// otherwise ordinary bytes.
TEST(Cp932, special_bytes_survive_inside_a_run) {
    EXPECT_TRUE(valid({kCp932ByteB5, kCp932ByteFF, kCp932ByteFF, kCp932ByteFF,
                       kCp932Space, kCp932Etx}));
    EXPECT_EQ(dec({'a', kCp932ByteFF, 'b'}), "a\uF8F3b");
    EXPECT_EQ(dec({kCp932HiraganaALead, kCp932HiraganaATrail, kCp932ByteFF}),
              "あ\uF8F3");
}

// 0xA0 must be read as a SINGLE byte, not as a lead byte swallowing the next
// one -- getting this wrong shifts every subsequent character.
TEST(Cp932, a0_does_not_swallow_the_following_byte) {
    EXPECT_EQ(dec({kCp932ByteA0, kCp932AsciiA}), "\uF8F0A");
}

TEST(Cp932, halfwidth_katakana_singles) {
    EXPECT_EQ(dec({kCp932ByteA1}), "｡");
    EXPECT_EQ(dec({kCp932ByteDF}), "ﾟ");
}

TEST(Cp932, decodes_ordinary_two_byte_characters) {
    EXPECT_EQ(dec_bytes(utf8_to_cp932_replace("あ")), "あ");
    EXPECT_EQ(dec_bytes(utf8_to_cp932_replace("「")), "「");
    EXPECT_EQ(dec_bytes(utf8_to_cp932_replace("日")), "日");
    EXPECT_EQ(dec_bytes(utf8_to_cp932_replace("さくら、もゆ。")), "さくら、もゆ。");
}

// Still strict everywhere else: a lead byte with an out-of-range trail is an
// error, not something to paper over.
TEST(Cp932, rejects_bad_trail_bytes) {
    EXPECT_FALSE(valid({kCp932InvalidLead, kCp932InvalidTrail}));
    EXPECT_FALSE(valid({kCp932HiraganaALead}));
    EXPECT_FALSE(valid({kCp932PunctLead, kCp932ByteFD}));
}

TEST(Cp932, empty_input_is_the_empty_string) {
    EXPECT_EQ(dec({}), "");
}

// Replacement mode: the special singles do decode, so they must NOT come back
// as U+FFFD.
TEST(Cp932, replace_mode_keeps_special_singles) {
    const Bytes b{kCp932ByteFF, kCp932InvalidLead, kCp932InvalidTrail};
    const std::string s = cp932_to_utf8_replace(b.data(), b.size());
    EXPECT_EQ(s.rfind("\uF8F3", kPrefixMatchStart), kPrefixMatchStart)
        << "0xFF should decode, not be replaced";
    EXPECT_NE(s.find("\uFFFD"), std::string::npos) << "0x86 0x3A should be replaced";
}
