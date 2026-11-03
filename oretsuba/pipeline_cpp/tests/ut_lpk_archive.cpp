// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "apps/paths.h"
#include "common/util.h"
#include "lpk/lpk_archive.h"

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kTestMasterKey0 = 0x11223344U;
constexpr std::uint32_t kTestMasterKey1 = 0x55667788U;
constexpr std::uint8_t kLzssAllLiteralControl = 0xFFU;
constexpr std::uint8_t kLzssFirstLiteralSecondMatchControl = 0x01U;
constexpr std::uint8_t kLzssHighNibbleShift = 4U;
constexpr std::size_t kExpectedScriptLpkEntries = 418U;
constexpr std::size_t kExpectedSobFileCount = 413U;
constexpr std::size_t kExpectedNonSobFileCount = 5U;
constexpr std::size_t kSobMagicLength = 4U;
constexpr std::size_t kSyntheticUnpackedLen = 7U;
constexpr std::size_t kSyntheticLiteralLen = 4U;

std::string FindScriptLpkPath() {
    const std::string game_dir = oretsuba::apps::DefaultGameDirectory();
    if (game_dir.empty()) {
        return {};
    }
    const fs::path candidate = fs::u8path(game_dir) / "SCRIPT.LPK";
    if (!fs::is_regular_file(candidate)) {
        return {};
    }
    return candidate.u8string();
}

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

    const auto offset_low = static_cast<std::uint8_t>(oretsuba::lpk::kInitialRingPos & 0xFFU);
    const auto offset_high_and_len = static_cast<std::uint8_t>(
        ((oretsuba::lpk::kInitialRingPos >> 8U) << kLzssHighNibbleShift) | 0x03U);
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
    const oretsuba::Bytes bad_archive = {'B', 'A', 'D', '!', 0, 0, 0, 0};
    EXPECT_THROW(
        oretsuba::lpk::LpkArchiveReader(bad_archive, "SCRIPT"),
        std::runtime_error);
}

TEST(LpkArchiveTest, UnpacksRealScriptLpkAndVerifiesSobMagics) {
    const std::string lpk_path = FindScriptLpkPath();
    ASSERT_FALSE(lpk_path.empty()) << "SCRIPT.LPK not found in default game directory";

    const oretsuba::lpk::LpkArchiveReader reader(lpk_path);
    EXPECT_EQ(reader.EntryCount(), kExpectedScriptLpkEntries);

    std::size_t sob_count = 0;
    std::size_t non_sob_count = 0;
    for (const auto& entry : reader.Entries()) {
        if (entry.name.size() >= kSobMagicLength &&
            entry.name.substr(entry.name.size() - kSobMagicLength) == ".sob") {
            ++sob_count;
            continue;
        }
        ++non_sob_count;
    }
    EXPECT_EQ(sob_count, kExpectedSobFileCount);
    EXPECT_EQ(non_sob_count, kExpectedNonSobFileCount);

    const oretsuba::Bytes main_sob = reader.ReadEntryByName("main.sob");
    ASSERT_GE(main_sob.size(), kSobMagicLength);
    EXPECT_EQ(std::memcmp(main_sob.data(), "SOB0", kSobMagicLength), 0);

    const oretsuba::Bytes s01_sob = reader.ReadEntryByName("s01_01a.sob");
    ASSERT_GE(s01_sob.size(), kSobMagicLength);
    EXPECT_EQ(std::memcmp(s01_sob.data(), "SOB0", kSobMagicLength), 0);

    const oretsuba::Bytes user_var = reader.ReadEntryByName("user_var.sob");
    ASSERT_GE(user_var.size(), kSobMagicLength);
    EXPECT_EQ(std::memcmp(user_var.data(), "SOB1", kSobMagicLength), 0);

    const oretsuba::Bytes func_lib = reader.ReadEntryByName("func_lib.sob");
    ASSERT_GE(func_lib.size(), kSobMagicLength);
    EXPECT_EQ(std::memcmp(func_lib.data(), "SOB2", kSobMagicLength), 0);
}
