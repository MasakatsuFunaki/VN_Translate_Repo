// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// CP932 codec tests covering strict/lossy decoding, encoding round-trips,
// and the five single-byte vendor extensions (0x80, 0xA0, 0xFD-0xFF).
#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>

#include "common/util.h"

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

TEST(Cp932Fuzz, decodes_the_five_special_single_bytes) {
    EXPECT_EQ(dec({0x80}), "\xC2\x80");
    EXPECT_EQ(dec({0xA0}), "\xEF\xA3\xB0");
    EXPECT_EQ(dec({0xFD}), "\xEF\xA3\xB1");
    EXPECT_EQ(dec({0xFE}), "\xEF\xA3\xB2");
    EXPECT_EQ(dec({0xFF}), "\xEF\xA3\xB3");
}

TEST(Cp932Fuzz, special_bytes_survive_inside_a_run) {
    EXPECT_TRUE(valid({0xB5, 0xFF, 0xFF, 0xFF, 0x20, 0x03}));
    EXPECT_EQ(dec({'a', 0xFF, 'b'}), "a\xEF\xA3\xB3" "b");
    EXPECT_EQ(dec({0x82, 0xA0, 0xFF}), "あ\xEF\xA3\xB3");
}

TEST(Cp932Fuzz, a0_does_not_swallow_the_following_byte) {
    EXPECT_EQ(dec({0xA0, 0x41}), "\xEF\xA3\xB0" "A");
}

TEST(Cp932Fuzz, halfwidth_katakana_singles) {
    EXPECT_EQ(dec({0xA1}), "｡");
    EXPECT_EQ(dec({0xDF}), "ﾟ");
}

TEST(Cp932Fuzz, decodes_ordinary_two_byte_characters) {
    EXPECT_EQ(dec({0x82, 0xA0}), "あ");
    EXPECT_EQ(dec({0x81, 0x75}), "「");
    EXPECT_EQ(dec({0x93, 0xFA}), "日");
}

TEST(Cp932Fuzz, rejects_bad_trail_bytes) {
    EXPECT_FALSE(valid({0x86, 0x3A}));
    EXPECT_FALSE(valid({0x82}));
    EXPECT_FALSE(valid({0x81, 0xFD}));
}

TEST(Cp932Fuzz, empty_input_is_the_empty_string) {
    EXPECT_EQ(dec({}), "");
}

TEST(Cp932Fuzz, replace_mode_keeps_special_singles) {
    const Bytes b{0xFF, 0x86, 0x3A};
    const std::string s = cp932_to_utf8_replace(b.data(), b.size());
    EXPECT_EQ(s.rfind("\xEF\xA3\xB3", 0), 0u);
    EXPECT_NE(s.find("\xEF\xBF\xBD"), std::string::npos);
}

TEST(Cp932Fuzz, strict_roundtrip_preserves_ageha_dialogue_and_rejects_unmappable) {
    const std::string sample = "アゲハ\r\n「あ、九条アゲハよ」";
    const auto cp932 = utf8_to_cp932_strict(sample);
    ASSERT_TRUE(cp932.has_value());
    const auto back = cp932_to_utf8_strict(*cp932);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, sample);

    EXPECT_FALSE(utf8_to_cp932_strict("😀").has_value());
}
