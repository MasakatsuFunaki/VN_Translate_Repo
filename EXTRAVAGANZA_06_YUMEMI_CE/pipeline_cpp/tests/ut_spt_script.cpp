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

Bytes make_synthetic_spt(const std::vector<std::string>& utf8_strings) {
    Bytes dec(kDataStartOffset, 0);
    std::memcpy(dec.data() + kHeaderOffset, "SPTHEADER0", 10);
    const auto count = static_cast<std::uint32_t>(utf8_strings.size());
    dec[kStringCountOffset + 0] = static_cast<std::uint8_t>(count & 0xFFu);
    dec[kStringCountOffset + 1] = static_cast<std::uint8_t>((count >> 8) & 0xFFu);
    dec[kStringCountOffset + 2] = static_cast<std::uint8_t>((count >> 16) & 0xFFu);
    dec[kStringCountOffset + 3] = static_cast<std::uint8_t>((count >> 24) & 0xFFu);

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
        "夢美\r\n「やめてください……」",
        "口に突き入れて、粘液を飲ませることができる。",
        "★乳首に針を刺されたときのセリフ\r\n",
        "口を責める",
    });

    const SptScript script(encrypted);
    ASSERT_TRUE(script.is_valid());
    EXPECT_EQ(script.header_string_count(), 4u);
    ASSERT_EQ(script.strings().size(), 4u);

    EXPECT_EQ(script.strings()[0].type, "dialogue");
    EXPECT_EQ(script.strings()[0].offset, kDataStartOffset);
    EXPECT_TRUE(script.strings()[0].has_jp);

    EXPECT_EQ(script.strings()[1].type, "narrative");
    EXPECT_EQ(script.strings()[2].type, "menu");
    EXPECT_EQ(script.strings()[3].type, "name");
}

TEST(SptScriptContract, ParsesRealGameSptWhenAvailable) {
    const char* env_dir = std::getenv("VN_GAME_DIR");
    if (!env_dir || !*env_dir) {
        GTEST_SKIP() << "VN_GAME_DIR not set; skipping live game SPT check";
    }
    const std::filesystem::path real_spt =
        std::filesystem::u8path(env_dir) / "spt" / "m_tokuten.spt";
    if (!std::filesystem::exists(real_spt)) {
        GTEST_SKIP() << "Installed game m_tokuten.spt not present on this machine";
    }
    const SptScript script(read_file(real_spt.u8string()));
    ASSERT_TRUE(script.is_valid());
    EXPECT_EQ(script.header_string_count(), 689u);
    EXPECT_GT(script.strings().size(), 1000u);
}

}  // namespace
}  // namespace exm::spt
