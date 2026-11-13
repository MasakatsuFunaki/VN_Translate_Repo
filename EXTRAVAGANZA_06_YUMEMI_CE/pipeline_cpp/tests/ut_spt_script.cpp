// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "spt/spt_script.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace exm::spt {
namespace {

constexpr std::size_t kHeaderOffset = 0x30;
constexpr std::size_t kSptHeaderMagicLen = 10;
constexpr std::size_t kStringCountOffset = 0x18;
constexpr std::size_t kDataStartOffset = 0x100;
constexpr std::size_t kAlignment = 4;
constexpr std::uint32_t kByteShift1 = 8;
constexpr std::uint32_t kByteShift2 = 16;
constexpr std::uint32_t kByteShift3 = 24;
constexpr std::uint32_t kByteMask = 0xFFu;
constexpr std::size_t kByteOffset0 = 0;
constexpr std::size_t kByteOffset1 = 1;
constexpr std::size_t kByteOffset2 = 2;
constexpr std::size_t kByteOffset3 = 3;
constexpr std::uint32_t kExpectedSyntheticStringCount = 4u;
constexpr std::size_t kDialogueIndex = 0;
constexpr std::size_t kNarrativeIndex = 1;
constexpr std::size_t kMenuIndex = 2;
constexpr std::size_t kNameIndex = 3;

Bytes make_synthetic_spt(const std::vector<std::string>& utf8_strings) {
    Bytes dec(kDataStartOffset, 0);
    std::memcpy(dec.data() + kHeaderOffset, "SPTHEADER0", kSptHeaderMagicLen);
    const auto count = static_cast<std::uint32_t>(utf8_strings.size());
    dec[kStringCountOffset + kByteOffset0] =
        static_cast<std::uint8_t>(count & kByteMask);
    dec[kStringCountOffset + kByteOffset1] =
        static_cast<std::uint8_t>((count >> kByteShift1) & kByteMask);
    dec[kStringCountOffset + kByteOffset2] =
        static_cast<std::uint8_t>((count >> kByteShift2) & kByteMask);
    dec[kStringCountOffset + kByteOffset3] =
        static_cast<std::uint8_t>((count >> kByteShift3) & kByteMask);

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
    EXPECT_EQ(script.header_string_count(), kExpectedSyntheticStringCount);
    ASSERT_EQ(script.strings().size(), kExpectedSyntheticStringCount);

    EXPECT_EQ(script.strings()[kDialogueIndex].type, "dialogue");
    EXPECT_EQ(script.strings()[kDialogueIndex].offset, kDataStartOffset);
    EXPECT_TRUE(script.strings()[kDialogueIndex].has_jp);

    EXPECT_EQ(script.strings()[kNarrativeIndex].type, "narrative");
    EXPECT_EQ(script.strings()[kMenuIndex].type, "menu");
    EXPECT_EQ(script.strings()[kNameIndex].type, "name");
}

}  // namespace
}  // namespace exm::spt
