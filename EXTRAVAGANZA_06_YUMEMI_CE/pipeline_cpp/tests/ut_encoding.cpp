// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include <gtest/gtest.h>

#include "common/util.h"

namespace exm {
namespace {

TEST(EncodingContract, Cp932Utf8StrictRoundTrip) {
    const std::string original = "夢美\r\n「やめてください……お願いですから！」";
    const auto cp932 = utf8_to_cp932_strict(original);
    ASSERT_TRUE(cp932.has_value());
    ASSERT_FALSE(cp932->empty());

    const auto roundtrip = cp932_to_utf8_strict(*cp932);
    ASSERT_TRUE(roundtrip.has_value());
    EXPECT_EQ(*roundtrip, original);
    EXPECT_GT(jp_char_count(*roundtrip), 0);
}

TEST(EncodingContract, StrictRejectsUnencodableUnicodeAndTruncatedLeadByte) {
    const std::string emoji = "Yumemi 🌸";
    EXPECT_FALSE(utf8_to_cp932_strict(emoji).has_value());

    const Bytes truncated_lead = {0x82};
    EXPECT_FALSE(cp932_to_utf8_strict(truncated_lead).has_value());
}

}  // namespace
}  // namespace exm
