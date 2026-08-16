// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The formatting primitives the speaker gate's report is built from.  Its
// [2/5] block right-aligns a quoted JP name in 8 and left-aligns the resolved
// EN name in 10, so the quoting rules and the codepoint-counted padding both
// have to be exact or the columns collapse.
#include <string>

#include <gtest/gtest.h>

#include "common/util.h"

using namespace frat;

TEST(SpeakerGate, QuotedFormAndCodepointPadding) {
    EXPECT_EQ(quote_repr("大智"), "'大智'");
    EXPECT_EQ(quote_repr("Daichi"), "'Daichi'");
    // Double quotes when the string has an apostrophe and no double quote, so
    // "Yuka's Mother" needs no escape.
    EXPECT_EQ(quote_repr("Yuka's Mother"), "\"Yuka's Mother\"");
    EXPECT_EQ(quote_repr("say \"hi\""), "'say \"hi\"'");
    EXPECT_EQ(quote_repr("both ' and \""), "'both \\' and \"'");
    EXPECT_EQ(quote_repr("a\\b"), "'a\\\\b'");
    EXPECT_EQ(quote_repr("a\r\nb"), "'a\\r\\nb'");
    // U+3000 is not printable, so it is escaped rather than shown as a blank.
    EXPECT_EQ(quote_repr("\xE3\x80\x80"), "'\\u3000'");

    // "'愛'" is 3 codepoints (8 bytes) -- a byte-counted pad would prepend 0.
    EXPECT_EQ(pad_left_cp(quote_repr("愛"), 8), "     '愛'");
    EXPECT_EQ(pad_right_cp(quote_repr("Daichi"), 10), "'Daichi'  ");
    // Longer than the field -> returned unchanged, never truncated.
    EXPECT_EQ(pad_left_cp(quote_repr("Middle-aged Man"), 8), "'Middle-aged Man'");
}

TEST(SpeakerGate, CommaGroupsThousands) {
    EXPECT_EQ(comma(32893), "32,893");
    EXPECT_EQ(comma(5000), "5,000");
    EXPECT_EQ(comma(999), "999");
    EXPECT_EQ(comma(1000000), "1,000,000");
}

TEST(SpeakerGate, CpSubstrCountsCodepoints) {
    EXPECT_EQ(cp_substr("あいうえお", 0, 3), "あいう");
    EXPECT_EQ(cp_substr("あいうえお", 2), "うえお");
    EXPECT_EQ(char_len("あいうえお"), 5u);
}
