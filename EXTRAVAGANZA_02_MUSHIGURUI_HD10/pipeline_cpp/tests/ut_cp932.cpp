// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CP932 codec tests.
//
// CP932 maps 0x80, 0xA0 and 0xFD-0xFF as single-byte characters (U+0080 and
// Private Use Area U+F8F0-U+F8F3) rather than two-byte lead bytes.  Around 13%
// of extracted BLACKCyc script strings contain at least one of them, so strict
// decoding accepts all five to preserve string array indices while still
// rejecting malformed multi-byte sequences.
#include "common/util.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>

using namespace mgi;

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

std::string utf8_from_bytes(std::initializer_list<std::uint8_t> raw) {
    return std::string(reinterpret_cast<const char*>(raw.begin()), raw.size());
}

}  // namespace

// CP932 single-byte mappings outside standard Shift-JIS: C1 control U+0080 and
// Private Use Area U+F8F0-U+F8F3.
TEST(Cp932, decodes_special_single_bytes) {
    EXPECT_EQ(dec({0x80u}), utf8_from_bytes({0xC2u, 0x80u}));          // U+0080
    EXPECT_EQ(dec({0xA0u}), utf8_from_bytes({0xEFu, 0xA3u, 0xB0u}));   // U+F8F0
    EXPECT_EQ(dec({0xFDu}), utf8_from_bytes({0xEFu, 0xA3u, 0xB1u}));   // U+F8F1
    EXPECT_EQ(dec({0xFEu}), utf8_from_bytes({0xEFu, 0xA3u, 0xB2u}));   // U+F8F2
    EXPECT_EQ(dec({0xFFu}), utf8_from_bytes({0xEFu, 0xA3u, 0xB3u}));   // U+F8F3
}

// Special single bytes inside runs of ordinary single-byte and two-byte text.
TEST(Cp932, special_bytes_survive_inside_a_run) {
    // 'B5 FF FF FF 20 03' -- taken from mushi.spt offset 6492.
    EXPECT_TRUE(valid({0xB5u, 0xFFu, 0xFFu, 0xFFu, 0x20u, 0x03u}));
    const std::string pua_f8f3 = utf8_from_bytes({0xEFu, 0xA3u, 0xB3u});
    EXPECT_EQ(dec({'a', 0xFFu, 'b'}), "a" + pua_f8f3 + "b");
    EXPECT_EQ(dec({0x82u, 0xA0u, 0xFFu}), "あ" + pua_f8f3);
}

// 0xA0 must be read as a SINGLE byte, not as a lead byte swallowing the next
// one -- getting this wrong shifts every subsequent character.
TEST(Cp932, a0_does_not_swallow_the_following_byte) {
    EXPECT_EQ(dec({0xA0u, 0x41u}), utf8_from_bytes({0xEFu, 0xA3u, 0xB0u}) + "A");
}

TEST(Cp932, halfwidth_katakana_singles) {
    EXPECT_EQ(dec({0xA1u}), "｡");  // U+FF61
    EXPECT_EQ(dec({0xDFu}), "ﾟ");  // U+FF9F
}

TEST(Cp932, decodes_ordinary_two_byte_characters) {
    EXPECT_EQ(dec({0x82u, 0xA0u}), "あ");
    EXPECT_EQ(dec({0x81u, 0x75u}), "「");
    EXPECT_EQ(dec({0x93u, 0xFAu}), "日");
}

// Still strict everywhere else: a lead byte with an out-of-range trail is an
// error, not something to paper over.
TEST(Cp932, rejects_bad_trail_bytes) {
    EXPECT_FALSE(valid({0x86u, 0x3Au}));  // 0x3A is not a valid trail
    EXPECT_FALSE(valid({0x82u}));         // truncated lead
    EXPECT_FALSE(valid({0x81u, 0xFDu}));  // trail out of range
}

TEST(Cp932, empty_input_is_the_empty_string) {
    EXPECT_EQ(dec({}), "");
}

// Lossy decode: the special singles decode, so they must NOT come back as
// U+FFFD -- only genuinely invalid bytes do.
TEST(Cp932, replace_mode_keeps_special_singles) {
    const Bytes b{0xFFu, 0x86u, 0x3Au};
    const std::string s = cp932_to_utf8_replace(b.data(), b.size());
    const std::string ufffd = utf8_from_bytes({0xEFu, 0xBFu, 0xBDu});
    EXPECT_EQ(s.rfind(utf8_from_bytes({0xEFu, 0xA3u, 0xB3u}), 0), 0u)
        << "0xFF should decode, not be replaced";
    EXPECT_NE(s.find(ufffd), std::string::npos) << "0x86 0x3A should be replaced";
}
