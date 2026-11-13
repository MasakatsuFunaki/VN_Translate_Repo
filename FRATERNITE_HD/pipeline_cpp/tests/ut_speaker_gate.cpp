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
#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

using namespace frat;

namespace {

constexpr std::size_t kPadWidthLeft = 8;
constexpr std::size_t kPadWidthRight = 10;
constexpr long long kSampleLargeCount = 32893;
constexpr long long kSampleFiveThousand = 5000;
constexpr long long kSampleThreeDigits = 999;
constexpr long long kSampleOneMillion = 1000000;
constexpr std::size_t kSubstrCountThree = 3;
constexpr std::size_t kSubstrStartTwo = 2;
constexpr std::size_t kExpectedFiveCodepoints = 5u;

}  // namespace

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
    EXPECT_EQ(quote_repr("　"), "'\\u3000'");

    // "'愛'" is 3 codepoints (8 bytes) -- a byte-counted pad would prepend 0.
    EXPECT_EQ(pad_left_cp(quote_repr("愛"), kPadWidthLeft), "     '愛'");
    EXPECT_EQ(pad_right_cp(quote_repr("Daichi"), kPadWidthRight), "'Daichi'  ");
    // Longer than the field -> returned unchanged, never truncated.
    EXPECT_EQ(pad_left_cp(quote_repr("Middle-aged Man"), kPadWidthLeft),
              "'Middle-aged Man'");
}

TEST(SpeakerGate, CommaGroupsThousands) {
    EXPECT_EQ(comma(kSampleLargeCount), "32,893");
    EXPECT_EQ(comma(kSampleFiveThousand), "5,000");
    EXPECT_EQ(comma(kSampleThreeDigits), "999");
    EXPECT_EQ(comma(kSampleOneMillion), "1,000,000");
}

TEST(SpeakerGate, CpSubstrCountsCodepoints) {
    EXPECT_EQ(cp_substr("あいうえお", 0, kSubstrCountThree), "あいう");
    EXPECT_EQ(cp_substr("あいうえお", kSubstrStartTwo), "うえお");
    EXPECT_EQ(char_len("あいうえお"), kExpectedFiveCodepoints);
}
