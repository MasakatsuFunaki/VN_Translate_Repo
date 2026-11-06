// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>

#include "spt/spt_script.h"

namespace exm::spt {
namespace {

constexpr std::size_t kHeaderOffset = 0x30;
constexpr std::size_t kStringCountOffset = 0x18;
constexpr std::size_t kDataStartOffset = 0x100;
constexpr std::size_t kAlignment = 4;
constexpr std::size_t kHeaderSignatureLen = 10;
constexpr unsigned int kByteShift8 = 8;
constexpr unsigned int kByteShift16 = 16;
constexpr unsigned int kByteShift24 = 24;
constexpr std::uint32_t kByteMask = 0xFFu;
constexpr std::uint32_t kExpectedSyntheticStringCount = 5;
constexpr std::uint32_t kExpectedTantai3HeaderCount = 729;
constexpr std::size_t kMinExpectedTantai3Strings = 1800;

Bytes make_synthetic_spt(const std::vector<std::string>& utf8_strings) {
    Bytes dec(kDataStartOffset, 0);
    std::memcpy(dec.data() + kHeaderOffset, "SPTHEADER0", kHeaderSignatureLen);
    const auto count = static_cast<std::uint32_t>(utf8_strings.size());
    dec[kStringCountOffset + 0] = static_cast<std::uint8_t>(count & kByteMask);
    dec[kStringCountOffset + 1] = static_cast<std::uint8_t>((count >> kByteShift8) & kByteMask);
    dec[kStringCountOffset + 2] = static_cast<std::uint8_t>((count >> kByteShift16) & kByteMask);
    dec[kStringCountOffset + 3] = static_cast<std::uint8_t>((count >> kByteShift24) & kByteMask);

    for (const auto& s : utf8_strings) {
        const Bytes cp932 = utf8_to_cp932_replace(s);
        dec.insert(dec.end(), cp932.begin(), cp932.end());
        dec.push_back(0);
        while (dec.size() % kAlignment != 0) {
            dec.push_back(0);
        }
    }
    return decrypt(dec);
}

TEST(SptScriptContract, DecryptsAndClassifiesAlignedStringPool) {
    const Bytes encrypted = make_synthetic_spt({
        "水菜\r\n「史郎くん、おはよう……」",
        "史郎\r\n（まさか水菜がこんなことを……）",
        "放課後の教室には、もう誰も残っていなかった。",
        "■最初から始める\r\n",
        "水菜",
    });

    const SptScript script(encrypted);
    ASSERT_TRUE(script.is_valid());
    EXPECT_EQ(script.header_string_count(), kExpectedSyntheticStringCount);
    ASSERT_EQ(script.strings().size(), kExpectedSyntheticStringCount);

    EXPECT_EQ(script.strings()[0].type, "dialogue");
    EXPECT_EQ(script.strings()[0].offset, kDataStartOffset);
    EXPECT_TRUE(script.strings()[0].has_jp);

    EXPECT_EQ(script.strings()[1].type, "dialogue");
    EXPECT_EQ(script.strings()[2].type, "narrative");
    EXPECT_EQ(script.strings()[3].type, "menu");
    EXPECT_EQ(script.strings()[4].type, "name");
}

TEST(SptScriptContract, ParsesRealGameSptWhenAvailable) {
    const char* env_dir = std::getenv("VN_GAME_DIR");
    if (!env_dir || !*env_dir) {
        GTEST_SKIP() << "VN_GAME_DIR not set; skipping live game SPT check";
    }
    const std::filesystem::path real_spt =
        std::filesystem::u8path(env_dir) / "spt" / "tantai3.spt";
    if (!std::filesystem::exists(real_spt)) {
        GTEST_SKIP() << "Installed game tantai3.spt not present on this machine";
    }
    const SptScript script(read_file(real_spt.u8string()));
    ASSERT_TRUE(script.is_valid());
    EXPECT_EQ(script.header_string_count(), kExpectedTantai3HeaderCount);
    EXPECT_GT(script.strings().size(), kMinExpectedTantai3Strings);
}

}  // namespace
}  // namespace exm::spt
