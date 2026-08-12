// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — CP932 codec / lead-byte tests for CROSS_CHANNEL.
//
// Tests the CP932-specific helpers in translator_logic: lead-byte
// classification (IsCp932LeadByte) and the byte-buffer scan that decides
// whether a string is JP at all (HasJPLeadByte). Pure functions, no I/O.

#include "translator_logic.h"

#include <gtest/gtest.h>

using translator_logic::IsCp932LeadByte;
using translator_logic::HasJPLeadByte;

TEST(Cp932, LeadByteRange) {
    EXPECT_TRUE (IsCp932LeadByte(0x81));
    EXPECT_TRUE (IsCp932LeadByte(0x9F));
    EXPECT_TRUE (IsCp932LeadByte(0xE0));
    EXPECT_TRUE (IsCp932LeadByte(0xFC));
    EXPECT_FALSE(IsCp932LeadByte(0x80));
    EXPECT_FALSE(IsCp932LeadByte(0xA0));
    EXPECT_FALSE(IsCp932LeadByte(0xDF));
    EXPECT_FALSE(IsCp932LeadByte(0x7F));
}

TEST(Cp932, LeadByteRangeBoundaries) {
    EXPECT_FALSE(IsCp932LeadByte(0x00));
    EXPECT_FALSE(IsCp932LeadByte(0xFF));
    EXPECT_FALSE(IsCp932LeadByte(0xFD));
}

TEST(HasJpLeadByte, AsciiStringIsRejected) {
    const char ascii[] = "Hello world";
    EXPECT_FALSE(HasJPLeadByte(ascii, sizeof(ascii) - 1));
}

TEST(HasJpLeadByte, JapaneseStringIsAccepted) {
    const char japanese[] = "\x82\xa0\x82\xa2";
    EXPECT_TRUE(HasJPLeadByte(japanese, sizeof(japanese) - 1));
}

TEST(HasJpLeadByte, EmptyBufferIsRejected) {
    EXPECT_FALSE(HasJPLeadByte("", 0));
}

TEST(HasJpLeadByte, SingleLeadByteWithNoTrailIsRejected) {
    const char dangling[1] = { static_cast<char>(0x82) };
    EXPECT_FALSE(HasJPLeadByte(dangling, 1));
}

TEST(HasJpLeadByte, JapaneseAfterAsciiPrefix) {
    const char mixed[] = "abc\x82\xa0";
    EXPECT_TRUE(HasJPLeadByte(mixed, sizeof(mixed) - 1));
}
