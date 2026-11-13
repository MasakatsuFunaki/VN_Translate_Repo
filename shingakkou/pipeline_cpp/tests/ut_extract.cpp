// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Bytecode extractor tests: the marker walk, the pre-byte classification order
// and the speaker split whose length bound is the single most expensive thing
// in this port to get wrong.
#include "cache/cache_core.h"
#include "common/util.h"
#include "extract/extractor.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace shin;
namespace ex = shin::extract;

namespace {

constexpr char32_t kBmpCodepointLimit = 0x10000u;
constexpr char32_t kLowByteMask = 0xFFu;
constexpr int kUtf16HighByteShift = 8;
constexpr std::uint8_t kMarkerByte0 = 0xFFu;
constexpr std::uint8_t kMarkerByte1 = 0x01u;
constexpr std::uint8_t kMarkerByte2 = 0x80u;
constexpr std::uint8_t kUtf16NullByte = 0x00u;
constexpr std::uint8_t kPreByteChoice = 0x01u;
constexpr std::uint8_t kPreByteDialogue = 0x36u;
constexpr std::uint8_t kPreByteAltDialogue = 0x07u;
constexpr std::uint8_t kPreByteIgnored0 = 0x00u;
constexpr std::uint8_t kPreByteIgnored2 = 0x02u;
constexpr std::uint8_t kPreByteIgnored4 = 0x04u;
constexpr int kMaxSpeakerCodepoints = 20;
constexpr std::size_t kSingleExtractedCount = 1u;
constexpr std::size_t kTwoExtractedCount = 2u;
constexpr std::size_t kFirstRecordMarkerOffset = 1u;
constexpr std::size_t kPreBytePrefixBytes = 1u;
constexpr std::size_t kMinRecordBytes = 4u;
constexpr std::size_t kIdx0 = 0u;
constexpr std::size_t kIdx1 = 1u;

// UTF-8 source literal -> the UTF-16LE bytes the engine actually stores.
void append_utf16le(Bytes& out, const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        ASSERT_LT(cp, kBmpCodepointLimit) << "test fixtures are BMP-only";
        out.push_back(static_cast<std::uint8_t>(cp & kLowByteMask));
        out.push_back(static_cast<std::uint8_t>(cp >> kUtf16HighByteShift));
    }
}

// <pre_byte> FF 01 80 <UTF-16LE> 00 00
void append_record(Bytes& out, std::uint8_t pre_byte, const std::string& utf8) {
    out.push_back(pre_byte);
    out.push_back(kMarkerByte0);
    out.push_back(kMarkerByte1);
    out.push_back(kMarkerByte2);
    append_utf16le(out, utf8);
    out.push_back(kUtf16NullByte);
    out.push_back(kUtf16NullByte);
}

std::vector<ex::Str> one(std::uint8_t pre_byte, const std::string& utf8) {
    Bytes data;
    append_record(data, pre_byte, utf8);
    return ex::extract_strings_from_script(data);
}

}  // namespace

// THE 311-LINE REGRESSION.  The speaker bound is on codepoints; this name is
// 10 codepoints but 30 bytes, so a byte-length test files it as narration with
// an empty speaker and every one of its TSV keys changes shape.
TEST(Extract, speaker_split_accepts_long_japanese_names) {
    const auto s = one(kPreByteDialogue, "ミスター・マコーリー\\n「x」");
    ASSERT_EQ(s.size(), kSingleExtractedCount);
    EXPECT_EQ(s[kIdx0].type, "dialogue");
    EXPECT_EQ(s[kIdx0].speaker, "ミスター・マコーリー");
    EXPECT_EQ(s[kIdx0].text, "「x」");
}

TEST(Extract, speaker_split_boundary_is_twenty_codepoints) {
    // Exactly 20 -> still a speaker.
    std::string name20;
    for (int i = 0; i < kMaxSpeakerCodepoints; ++i) {
        name20 += "あ";
    }
    const auto ok = one(kPreByteDialogue, name20 + "\\n「x」");
    ASSERT_EQ(ok.size(), kSingleExtractedCount);
    EXPECT_EQ(ok[kIdx0].type, "dialogue");
    EXPECT_EQ(ok[kIdx0].speaker, name20);

    // 21 -> falls through to narration, and the WHOLE string is the content.
    const std::string name21 = name20 + "あ";
    const auto bad = one(kPreByteDialogue, name21 + "\\n「x」");
    ASSERT_EQ(bad.size(), kSingleExtractedCount);
    EXPECT_EQ(bad[kIdx0].type, "narration");
    EXPECT_EQ(bad[kIdx0].speaker, "");
    EXPECT_EQ(bad[kIdx0].text, name21 + "\\n「x」");
}

// A leading literal backslash-n skips the speaker split entirely, then has
// exactly two characters removed.  A doubled prefix must keep the second --
// that is what makes the TSV key '\n' + text round-trip.
TEST(Extract, narration_strips_only_one_leading_backslash_n) {
    const auto s = one(kPreByteDialogue, "\\n\\nあいう");
    ASSERT_EQ(s.size(), kSingleExtractedCount);
    EXPECT_EQ(s[kIdx0].type, "narration");
    EXPECT_EQ(s[kIdx0].text, "\\nあいう");
}

TEST(Extract, prebyte_classification_order) {
    // 0x01 + Japanese -> choice, and it never reaches the 0x36/0x07 gate.
    const auto choice = one(kPreByteChoice, "屋上");
    ASSERT_EQ(choice.size(), kSingleExtractedCount);
    EXPECT_EQ(choice[kIdx0].type, "choice");
    EXPECT_EQ(choice[kIdx0].speaker, "");

    EXPECT_TRUE(one(kPreByteChoice, "ascii only").empty());
    EXPECT_EQ(one(kPreByteDialogue, "あいう").size(), kSingleExtractedCount);
    EXPECT_EQ(one(kPreByteAltDialogue, "あいう").size(), kSingleExtractedCount);
    for (const std::uint8_t pre : {kPreByteIgnored0, kPreByteIgnored2, kPreByteIgnored4}) {
        EXPECT_TRUE(one(pre, "あいう").empty());
    }
}

// The two Japanese predicates are deliberately different functions; using
// either one in the other's place changes what gets extracted or translated.
TEST(Extract, has_japanese_is_wider_than_the_translate_predicate) {
    EXPECT_TRUE(ex::has_japanese("？？？"));                     // fullwidth
    EXPECT_FALSE(translate::has_real_japanese("？？？"));
    EXPECT_FALSE(ex::has_japanese("㐀"));                         // U+3400 CJK Ext-A
    EXPECT_TRUE(translate::has_real_japanese("㐀"));
}

// Pins `i = pos + 2` (unconditional, before the filters) and the `pos + 1 < n`
// inner bound: an off-by-one here loses every second record or spins forever.
TEST(Extract, walk_advances_past_the_terminator) {
    Bytes data;
    append_record(data, kPreByteDialogue, "\\nいち");
    const std::size_t second_marker = data.size() + kPreBytePrefixBytes;
    append_record(data, kPreByteDialogue, "\\nに");
    const auto s = ex::extract_strings_from_script(data);
    ASSERT_EQ(s.size(), kTwoExtractedCount);
    EXPECT_EQ(s[kIdx0].offset, kFirstRecordMarkerOffset);
    EXPECT_EQ(s[kIdx1].offset, second_marker);
    EXPECT_EQ(s[kIdx0].text, "いち");
    EXPECT_EQ(s[kIdx1].text, "に");
}

TEST(Extract, short_buffer_does_not_underflow) {
    for (std::size_t n = 0; n < kMinRecordBytes; ++n) {
        EXPECT_TRUE(ex::extract_strings_from_script(Bytes(n, kMarkerByte0)).empty());
    }
}
