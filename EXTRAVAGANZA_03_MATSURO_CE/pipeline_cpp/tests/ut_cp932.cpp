// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CP932 codec tests.
//
// Verifies strict and replacement decoding of single-byte CP932 extensions
// (0x80, 0xA0, 0xFD-0xFF), half-width katakana, ordinary two-byte characters,
// and out-of-range trail bytes.
#include "common/util.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>

using namespace exm;

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

TEST(Cp932, decodes_the_five_special_single_bytes) {
    EXPECT_EQ(dec({0x80}), "\xC2\x80");          // U+0080
    EXPECT_EQ(dec({0xA0}), "\xEF\xA3\xB0");      // U+F8F0
    EXPECT_EQ(dec({0xFD}), "\xEF\xA3\xB1");      // U+F8F1
    EXPECT_EQ(dec({0xFE}), "\xEF\xA3\xB2");      // U+F8F2
    EXPECT_EQ(dec({0xFF}), "\xEF\xA3\xB3");      // U+F8F3
}

TEST(Cp932, special_bytes_survive_inside_a_run) {
    EXPECT_TRUE(valid({0xB5, 0xFF, 0xFF, 0xFF, 0x20, 0x03}));
    EXPECT_EQ(dec({'a', 0xFF, 'b'}), "a\xEF\xA3\xB3" "b");
    EXPECT_EQ(dec({0x82, 0xA0, 0xFF}), "あ\xEF\xA3\xB3");
}

TEST(Cp932, a0_does_not_swallow_the_following_byte) {
    EXPECT_EQ(dec({0xA0, 0x41}), "\xEF\xA3\xB0" "A");
}

TEST(Cp932, halfwidth_katakana_singles) {
    EXPECT_EQ(dec({0xA1}), "｡");
    EXPECT_EQ(dec({0xDF}), "ﾟ");
}

TEST(Cp932, decodes_ordinary_two_byte_characters) {
    EXPECT_EQ(dec({0x82, 0xA0}), "あ");
    EXPECT_EQ(dec({0x81, 0x75}), "「");
    EXPECT_EQ(dec({0x93, 0xFA}), "日");
}

TEST(Cp932, rejects_bad_trail_bytes) {
    EXPECT_FALSE(valid({0x86, 0x3A}));
    EXPECT_FALSE(valid({0x82}));
    EXPECT_FALSE(valid({0x81, 0xFD}));
}

TEST(Cp932, empty_input_is_the_empty_string) {
    EXPECT_EQ(dec({}), "");
}

TEST(Cp932, replace_mode_keeps_special_singles) {
    const Bytes b{0xFF, 0x86, 0x3A};
    const std::string s = cp932_to_utf8_replace(b.data(), b.size());
    EXPECT_EQ(s.rfind("\xEF\xA3\xB3", 0), 0u) << "0xFF should decode, not be replaced";
    EXPECT_NE(s.find("\xEF\xBF\xBD"), std::string::npos) << "0x86 0x3A should be replaced";
}
