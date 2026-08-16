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
//
// The expectations below are the CP932 mappings for those bytes; a differential
// sweep of the whole single-byte range against a reference CP932 table is what
// catches a regression here.
#include <gtest/gtest.h>

#include <string>

#include "common/util.h"

using namespace ama;

namespace {

std::string dec(std::initializer_list<std::uint8_t> bytes) {
    const Bytes b(bytes);
    auto s = cp932_to_utf8_strict(b);
    return s ? *s : std::string("<INVALID>");
}

bool valid(std::initializer_list<std::uint8_t> bytes) {
    const Bytes b(bytes);
    return cp932_to_utf8_strict(b).has_value();
}

}  // namespace

// The five bytes the original structure test wrongly rejected.
TEST(Cp932, decodes_cp932_special_single_bytes) {
    EXPECT_EQ(dec({0x80}), "\xC2\x80");          // U+0080
    EXPECT_EQ(dec({0xA0}), "\xEF\xA3\xB0");      // U+F8F0
    EXPECT_EQ(dec({0xFD}), "\xEF\xA3\xB1");      // U+F8F1
    EXPECT_EQ(dec({0xFE}), "\xEF\xA3\xB2");      // U+F8F2
    EXPECT_EQ(dec({0xFF}), "\xEF\xA3\xB3");      // U+F8F3
}

// The exact shape that was being dropped: a special byte inside a run of
// otherwise ordinary bytes.
TEST(Cp932, special_bytes_survive_inside_a_run) {
    // 'B5 FF FF FF 20 03' -- taken from mushi.spt offset 6492.
    EXPECT_TRUE(valid({0xB5, 0xFF, 0xFF, 0xFF, 0x20, 0x03}));
    EXPECT_EQ(dec({'a', 0xFF, 'b'}), "a\xEF\xA3\xB3" "b");
    // ...and a special byte adjacent to a real two-byte character.
    EXPECT_EQ(dec({0x82, 0xA0, 0xFF}), "\xE3\x81\x82\xEF\xA3\xB3");
}

// 0xA0 must be read as a SINGLE byte, not as a lead byte swallowing the next
// one -- getting this wrong shifts every subsequent character.
TEST(Cp932, a0_does_not_swallow_the_following_byte) {
    EXPECT_EQ(dec({0xA0, 0x41}), "\xEF\xA3\xB0" "A");
}

TEST(Cp932, halfwidth_katakana_singles) {
    EXPECT_EQ(dec({0xA1}), "\xEF\xBD\xA1");  // U+FF61
    EXPECT_EQ(dec({0xDF}), "\xEF\xBE\x9F");  // U+FF9F
}

TEST(Cp932, decodes_ordinary_two_byte_characters) {
    EXPECT_EQ(dec({0x82, 0xA0}), "\xE3\x81\x82");              // あ
    EXPECT_EQ(dec({0x81, 0x75}), "\xE3\x80\x8C");              // 「
    EXPECT_EQ(dec({0x93, 0xFA}), "\xE6\x97\xA5");              // 日
}

// Still strict where it counts: a lead byte with an out-of-range trail is an
// error, not something to paper over.
TEST(Cp932, rejects_bad_trail_bytes) {
    EXPECT_FALSE(valid({0x86, 0x3A}));  // 0x3A is not a valid trail
    EXPECT_FALSE(valid({0x82}));        // truncated lead
    EXPECT_FALSE(valid({0x81, 0xFD}));  // trail out of range
}

TEST(Cp932, empty_input_is_the_empty_string) {
    EXPECT_EQ(dec({}), "");
}

// Replace mode: the special singles do decode, so they must NOT come back as
// U+FFFD.
TEST(Cp932, replace_mode_keeps_special_singles) {
    const Bytes b{0xFF, 0x86, 0x3A};
    const std::string s = cp932_to_utf8_replace(b.data(), b.size());
    EXPECT_EQ(s.rfind("\xEF\xA3\xB3", 0), 0u) << "0xFF should decode, not be replaced";
    EXPECT_NE(s.find("\xEF\xBF\xBD"), std::string::npos) << "0x86 0x3A should be replaced";
}
