// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Script extractor unit tests: control-prefix stripping, sliding CP932 chunk
// scan, choice-option preamble recognition, speaker-dialogue pairing, and
// start-offset rotation.
#include "common/util.h"
#include "extract/extractor.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

using namespace crc;
namespace ex = crc::extract;

namespace {

constexpr std::uint8_t kNullByte = 0x00;
constexpr std::uint8_t kChoiceLeadByte = 0xFF;
constexpr std::uint8_t kChoiceLabelLow = 0x12;
constexpr std::uint8_t kChoiceLabelHigh = 0x34;
constexpr std::uint8_t kSpeakerMarker0 = 0x47;
constexpr std::uint8_t kSpeakerMarker1 = 0x0D;
constexpr std::uint8_t kSpeakerMarker2 = 0x00;
constexpr std::uint8_t kInvalidCp932Lead = 0x86;
constexpr std::uint8_t kInvalidCp932Trail = 0x3A;
constexpr std::uint8_t kOperand59 = 0x59;
constexpr std::uint8_t kOperand45 = 0x45;
constexpr std::uint8_t kOperand63 = 0x63;
constexpr std::uint8_t kOperand01 = 0x01;

constexpr std::size_t kShortBufferBytes = 4u;
constexpr std::size_t kChoiceMinOffset = 6u;
constexpr std::size_t kExpectedSpeakerStart = 3u;
constexpr int kTooLongSpeakerBytes = 31;
constexpr std::size_t kPairedDialogueOffset = 8u;
constexpr std::size_t kExpectedSingleEntry = 1u;
constexpr double kFullDensity = 1.0;
constexpr double kZeroDensity = 0.0;
constexpr double kHalfDensity = 0.5;
constexpr std::size_t kExpectedGameStartOffset = 1891283u;
constexpr std::size_t kSampleEarlyOffset = 10u;
constexpr std::size_t kSampleDeltaOffset = 5u;
constexpr std::size_t kSecondEntryIndex = 2u;

Bytes cp932_of(const std::string& utf8) {
    return utf8_to_cp932_strict(utf8).value();
}

void put(Bytes& b, const Bytes& chunk) {
    b.insert(b.end(), chunk.begin(), chunk.end());
}

void put(Bytes& b, std::initializer_list<std::uint8_t> l) {
    b.insert(b.end(), l);
}

}  // namespace

TEST(Extract, StripControlPrefixKeepsPurePrintablePrefix) {
    const char* const table[] = {
        "※男心を鷲掴む…",
        "Ａ定食は三時間目",
        "（佐倉）（遊紗）",
        "FLOWER'Sのもう片方",
        "「あ」",
        "…始まり",
        "ABCDEF",
        "",
    };
    for (const char* s : table) {
        EXPECT_EQ(ex::strip_control_prefix(s), s) << s;
    }
}

TEST(Extract, StripControlPrefixStripsWhenPrefixHasControlByte) {
    EXPECT_EQ(ex::strip_control_prefix("\x05" "ABC「あ」"), "「あ」");
    EXPECT_EQ(ex::strip_control_prefix("\x18\x02" "あいう"), "あいう");
    EXPECT_EQ(ex::strip_control_prefix("\x01" "abc「お」"), "「お」");
}

TEST(Extract, ExtractStringsSlidesOneByteOnDecodeFailure) {
    Bytes data;
    put(data, {kInvalidCp932Lead, kInvalidCp932Trail, kOperand59, kOperand45,
               kChoiceLeadByte, kChoiceLeadByte, kOperand63, kOperand01});
    put(data, cp932_of("「今日は」"));
    put(data, {kNullByte});

    const auto strings = ex::extract_strings(data, {});
    bool found = false;
    for (const auto& s : strings) {
        if (s.text.find("今日は") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found) << "the 「今日は」 chunk was dropped -- the walker skipped past the NUL";
}

TEST(Extract, ClassifyKeepsTwoCharChoiceOptionBehindPreamble) {
    const Bytes kaeru = cp932_of("帰る");

    Bytes with_preamble;
    put(with_preamble, {kChoiceLeadByte, kChoiceLeadByte, kChoiceLabelLow,
                        kChoiceLabelHigh, kNullByte, kNullByte});
    put(with_preamble, kaeru);
    put(with_preamble, {kNullByte});
    const auto entries =
        ex::classify_and_pair(ex::extract_strings(with_preamble, {}), with_preamble);
    ASSERT_EQ(entries.size(), kExpectedSingleEntry);
    EXPECT_EQ(entries[0].type, "narration");
    EXPECT_EQ(entries[0].text, "帰る");

    Bytes bare;
    put(bare, kaeru);
    put(bare, {kNullByte});
    EXPECT_TRUE(ex::classify_and_pair(ex::extract_strings(bare, {}), bare).empty());
}

TEST(Extract, IsChoiceOptionAtBoundsCheck) {
    const Bytes data(kShortBufferBytes, kChoiceLeadByte);
    for (std::size_t off = 0; off < kChoiceMinOffset; ++off) {
        EXPECT_FALSE(ex::is_choice_option_at(off, data));
    }
}

TEST(Extract, FindSpeakerOffsetsAdvancesByThreeAndBoundsNameLength) {
    const Bytes taichi = cp932_of("太一");

    Bytes data;
    put(data, {kSpeakerMarker0, kSpeakerMarker1, kSpeakerMarker2});
    put(data, taichi);
    put(data, {kNullByte});
    const auto offs = ex::find_speaker_offsets(data);
    ASSERT_EQ(offs.size(), kExpectedSingleEntry);
    EXPECT_EQ(*offs.begin(), kExpectedSpeakerStart);

    Bytes too_long;
    put(too_long, {kSpeakerMarker0, kSpeakerMarker1, kSpeakerMarker2});
    for (int i = 0; i < kTooLongSpeakerBytes; ++i) {
        too_long.push_back('a');
    }
    too_long.push_back(kNullByte);
    EXPECT_TRUE(ex::find_speaker_offsets(too_long).empty());

    const Bytes empty_name{kSpeakerMarker0, kSpeakerMarker1, kSpeakerMarker2, kNullByte};
    EXPECT_TRUE(ex::find_speaker_offsets(empty_name).empty());

    const Bytes unterminated{kSpeakerMarker0, kSpeakerMarker1, kSpeakerMarker2, 'a', 'b'};
    EXPECT_TRUE(ex::find_speaker_offsets(unterminated).empty());
}

TEST(Extract, SpeakerPairsWithNextEntryContainingOpenQuote) {
    const Bytes taichi = cp932_of("太一");

    Bytes data;
    put(data, {kSpeakerMarker0, kSpeakerMarker1, kSpeakerMarker2});
    put(data, taichi);
    put(data, {kNullByte});
    put(data, cp932_of("「今日は」"));
    put(data, {kNullByte});

    const auto speakers = ex::find_speaker_offsets(data);
    const auto entries = ex::classify_and_pair(ex::extract_strings(data, speakers), data);
    ASSERT_EQ(entries.size(), kExpectedSingleEntry);
    EXPECT_EQ(entries[0].type, "dialogue");
    EXPECT_EQ(entries[0].speaker, "太一");
    EXPECT_EQ(entries[0].offset, kPairedDialogueOffset);

    Bytes lonely;
    put(lonely, {kSpeakerMarker0, kSpeakerMarker1, kSpeakerMarker2});
    put(lonely, taichi);
    put(lonely, {kNullByte});
    const auto lonely_speakers = ex::find_speaker_offsets(lonely);
    EXPECT_TRUE(
        ex::classify_and_pair(ex::extract_strings(lonely, lonely_speakers), lonely).empty());
}

TEST(Extract, JapaneseDensityCountsCodepointsAndBonusChars) {
    EXPECT_DOUBLE_EQ(ex::japanese_density("…！？"), kFullDensity);
    EXPECT_DOUBLE_EQ(ex::japanese_density(""), kZeroDensity);
    EXPECT_DOUBLE_EQ(ex::japanese_density("あいうabc"), kHalfDensity);
}

TEST(Extract, IsRealJapaneseIncludesCjkSymbolsExcludesHalfwidthKana) {
    EXPECT_TRUE(ex::is_real_japanese(U'、'));
    EXPECT_TRUE(ex::is_real_japanese(U'「'));
    EXPECT_TRUE(ex::is_real_japanese(U'あ'));
    EXPECT_TRUE(ex::is_real_japanese(U'一'));
    EXPECT_FALSE(ex::is_real_japanese(U'ｱ'));
    EXPECT_FALSE(ex::is_real_japanese(U'ﾝ'));
    EXPECT_FALSE(ex::is_real_japanese(U'Ａ'));
}

TEST(Extract, RotationPutsFirstEntryAtOrAfterGameStartOffset) {
    EXPECT_EQ(ex::GAME_START_OFFSET, kExpectedGameStartOffset);

    std::vector<ex::Entry> entries = {
        {kSampleEarlyOffset, "narration", "", "a"},
        {ex::GAME_START_OFFSET, "narration", "", "b"},
        {ex::GAME_START_OFFSET + kSampleDeltaOffset, "narration", "", "c"},
    };
    EXPECT_EQ(ex::rotate_to_game_start(entries), kExpectedSingleEntry);
    EXPECT_EQ(entries[0].text, "b");
    EXPECT_EQ(entries[1].text, "c");
    EXPECT_EQ(entries[kSecondEntryIndex].text, "a");

    std::vector<ex::Entry> low = {{1, "narration", "", "a"}, {2, "narration", "", "b"}};
    EXPECT_EQ(ex::rotate_to_game_start(low), 0u);
    EXPECT_EQ(low[0].text, "a");
}
