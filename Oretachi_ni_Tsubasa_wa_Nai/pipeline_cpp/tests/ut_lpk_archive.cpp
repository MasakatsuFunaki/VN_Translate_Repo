// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "common/util.h"
#include "lpk/lpk_archive.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace {

constexpr std::uint32_t kTestMasterKey0 = 0x11223344U;
constexpr std::uint32_t kTestMasterKey1 = 0x55667788U;
constexpr std::uint8_t kLzssAllLiteralControl = 0xFFU;
constexpr std::uint8_t kLzssFirstLiteralSecondMatchControl = 0x01U;
constexpr std::uint8_t kLzssLowByteMask = 0xFFU;
constexpr std::uint8_t kLzssHighByteShift = 8U;
constexpr std::uint8_t kLzssHighNibbleShift = 4U;
constexpr std::uint8_t kLzssEncodedMatchLenField = 0x03U;
constexpr std::size_t kSyntheticUnpackedLen = 7U;
constexpr std::size_t kSyntheticLiteralLen = 4U;

}  // namespace

TEST(LpkArchiveTest, DeriveLpkKeysIsCaseInsensitiveAndAppliesMasterKeys) {
    const auto upper = oretsuba::lpk::DeriveLpkKeys("SCRIPT");
    const auto lower = oretsuba::lpk::DeriveLpkKeys("script");
    const auto mixed = oretsuba::lpk::DeriveLpkKeys("ScRiPt");

    EXPECT_EQ(upper.file_key, lower.file_key);
    EXPECT_EQ(upper.header_key, lower.header_key);
    EXPECT_EQ(upper.file_key, mixed.file_key);
    EXPECT_EQ(upper.header_key, mixed.header_key);
    EXPECT_NE(upper.file_key, 0U);
    EXPECT_NE(upper.header_key, 0U);

    const auto with_master =
        oretsuba::lpk::DeriveLpkKeys("SCRIPT", kTestMasterKey0, kTestMasterKey1);
    EXPECT_EQ(with_master.file_key, upper.file_key ^ kTestMasterKey0);
    EXPECT_EQ(with_master.header_key, upper.header_key ^ kTestMasterKey1);
}

TEST(LpkArchiveTest, DecompressLzssHandlesLiteralsAndRingMatches) {
    const oretsuba::Bytes literal_stream = {
        kLzssAllLiteralControl, 'S', 'O', 'B', '0'
    };
    const oretsuba::Bytes literal_out =
        oretsuba::lpk::DecompressLzss(literal_stream, kSyntheticLiteralLen);
    ASSERT_EQ(literal_out.size(), kSyntheticLiteralLen);
    EXPECT_EQ(std::memcmp(literal_out.data(), "SOB0", kSyntheticLiteralLen), 0);

    const auto offset_low =
        static_cast<std::uint8_t>(oretsuba::lpk::kInitialRingPos & kLzssLowByteMask);
    const auto offset_high_and_len = static_cast<std::uint8_t>(
        ((oretsuba::lpk::kInitialRingPos >> kLzssHighByteShift) << kLzssHighNibbleShift) |
        kLzssEncodedMatchLenField);
    const oretsuba::Bytes match_stream = {
        kLzssFirstLiteralSecondMatchControl,
        'A',
        offset_low,
        offset_high_and_len,
    };
    const oretsuba::Bytes match_out =
        oretsuba::lpk::DecompressLzss(match_stream, kSyntheticUnpackedLen);
    ASSERT_EQ(match_out.size(), kSyntheticUnpackedLen);
    for (std::uint8_t ch : match_out) {
        EXPECT_EQ(ch, static_cast<std::uint8_t>('A'));
    }
}

TEST(LpkArchiveTest, RejectsInvalidArchiveMagic) {
    const oretsuba::Bytes bad_archive = {'B', 'A', 'D', '!', 0U, 0U, 0U, 0U};
    EXPECT_THROW(
        oretsuba::lpk::LpkArchiveReader(bad_archive, "SCRIPT"),
        std::runtime_error);
}
