// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// SPT reader tests: the XOR-0xFF envelope, the 4-byte-aligned string walk,
// and the classifier's rule ordering.
#include "spt/spt_script.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace exc;

namespace {

constexpr std::size_t kSptHeaderSize = 0x80;
constexpr std::uint8_t kHeaderFillByte = 0x20;
constexpr std::size_t kMagicOffset = 0x30;
constexpr std::size_t kMagicLength = 10;
constexpr std::size_t kStringCountOffset = 0x18;
constexpr std::size_t kDwordAlignment = 4;
constexpr unsigned int kByte1Shift = 8;
constexpr unsigned int kByte2Shift = 16;
constexpr unsigned int kByte3Shift = 24;
constexpr std::size_t kByteIndex0 = 0;
constexpr std::size_t kByteIndex1 = 1;
constexpr std::size_t kByteIndex2 = 2;
constexpr std::size_t kByteIndex3 = 3;
constexpr std::size_t kTruncatedHeaderSize = 0x20;
constexpr std::uint32_t kSampleHeaderCount = 0x1234u;

Bytes make_spt(const std::vector<Bytes>& payloads, std::uint32_t hdr_count = 0) {
    Bytes dec(kSptHeaderSize, kHeaderFillByte);
    std::memcpy(dec.data() + kMagicOffset, "SPTHEADER0", kMagicLength);
    dec[kStringCountOffset + kByteIndex0] = static_cast<std::uint8_t>(hdr_count);
    dec[kStringCountOffset + kByteIndex1] =
        static_cast<std::uint8_t>(hdr_count >> kByte1Shift);
    dec[kStringCountOffset + kByteIndex2] =
        static_cast<std::uint8_t>(hdr_count >> kByte2Shift);
    dec[kStringCountOffset + kByteIndex3] =
        static_cast<std::uint8_t>(hdr_count >> kByte3Shift);
    for (const auto& p : payloads) {
        dec.insert(dec.end(), p.begin(), p.end());
        dec.push_back(0);
        while (dec.size() % kDwordAlignment != 0) {
            dec.push_back(0);
        }
    }
    return dec;
}

Bytes cp932(const char* utf8) {
    return utf8_to_cp932_replace(utf8);
}

}  // namespace

TEST(SptScript, decrypt_is_xor_ff_and_self_inverse) {
    const Bytes plain{0x00, 0x41, 0xFF, 0x80};
    const Bytes enc = spt::decrypt(plain);
    EXPECT_EQ(enc, (Bytes{0xFF, 0xBE, 0x00, 0x7F}));
    EXPECT_EQ(spt::decrypt(enc), plain);
}

TEST(SptScript, validity_requires_header_magic) {
    EXPECT_TRUE(spt::is_valid(make_spt({})));
    Bytes bad = make_spt({});
    bad[kMagicOffset] = 'X';
    EXPECT_FALSE(spt::is_valid(bad));
    EXPECT_FALSE(spt::is_valid(Bytes(kTruncatedHeaderSize, 0)));
}

TEST(SptScript, reads_header_string_count) {
    EXPECT_EQ(spt::header_string_count(make_spt({}, kSampleHeaderCount)),
              kSampleHeaderCount);
}

TEST(SptScript, extracts_strings_with_offsets) {
    const Bytes dec = make_spt({cp932("あい"), cp932("hello")});
    auto out = spt::extract_strings(dec);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].offset, kSptHeaderSize);
    EXPECT_EQ(out[0].text, "あい");
    EXPECT_TRUE(out[0].has_jp);
    EXPECT_EQ(out[1].text, "hello");
    EXPECT_FALSE(out[1].has_jp);
    EXPECT_EQ(out[1].offset % kDwordAlignment, 0u);
}

TEST(SptScript, drops_bytecode_noise_and_short_ascii) {
    const Bytes dec = make_spt({cp932("ff"), cp932("UU"), cp932("abc"), cp932("abcd")});
    auto out = spt::extract_strings(dec);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].text, "abcd");
}

TEST(SptScript, keeps_short_japanese) {
    const Bytes dec = make_spt({cp932("心")});
    auto out = spt::extract_strings(dec);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(out[0].has_jp);
}

TEST(SptScript, has_japanese_covers_fullwidth_and_symbols) {
    EXPECT_TRUE(spt::has_japanese("あ"));
    EXPECT_TRUE(spt::has_japanese("Ａ"));
    EXPECT_TRUE(spt::has_japanese("「"));
    EXPECT_FALSE(spt::has_japanese("plain ascii"));
}

TEST(SptScript, classify_rules) {
    EXPECT_EQ(spt::classify("美弥香\r\n「あ」"), "dialogue");
    EXPECT_EQ(spt::classify("あいうえおかきくけこさ\r\nし"), "narrative");
    EXPECT_EQ(spt::classify("「あ」"), "dialogue");
    EXPECT_EQ(spt::classify("★ start"), "menu");
    EXPECT_EQ(spt::classify("bgm_01"), "label");
    EXPECT_EQ(spt::classify("美弥香"), "name");
    EXPECT_EQ(spt::classify("あいうえおかき"), "narrative");
    EXPECT_EQ(spt::classify("   "), "empty");
}
