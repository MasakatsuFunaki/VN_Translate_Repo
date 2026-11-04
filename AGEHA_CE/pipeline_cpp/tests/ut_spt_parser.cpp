// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// SPT reader tests: the XOR-0xFF envelope, the 4-byte-aligned string walk,
// and the classifier's rule ordering.
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "common/util.h"
#include "spt/spt_script.h"

using namespace exm;

namespace {

constexpr std::size_t kHeaderSize = 0x80;
constexpr std::uint8_t kSpaceByte = 0x20;
constexpr std::size_t kMagicOffset = 0x30;
constexpr const char* kMagicText = "SPTHEADER0";
constexpr std::size_t kMagicLen = 10;
constexpr std::size_t kCountOffset = 0x18;
constexpr unsigned int kShift8 = 8;
constexpr unsigned int kShift16 = 16;
constexpr unsigned int kShift24 = 24;
constexpr std::size_t kAlignment = 4;
constexpr std::size_t kShortInvalidBufSize = 0x20;
constexpr std::uint32_t kTestHeaderCount = 0x1234u;

Bytes make_spt(const std::vector<Bytes>& payloads, std::uint32_t hdr_count = 0) {
    Bytes dec(kHeaderSize, kSpaceByte);
    std::memcpy(dec.data() + kMagicOffset, kMagicText, kMagicLen);
    dec[kCountOffset] = static_cast<std::uint8_t>(hdr_count);
    dec[kCountOffset + 1] = static_cast<std::uint8_t>(hdr_count >> kShift8);
    dec[kCountOffset + 2] = static_cast<std::uint8_t>(hdr_count >> kShift16);
    dec[kCountOffset + 3] = static_cast<std::uint8_t>(hdr_count >> kShift24);
    for (const auto& p : payloads) {
        dec.insert(dec.end(), p.begin(), p.end());
        dec.push_back(0);
        while (dec.size() % kAlignment) dec.push_back(0);
    }
    return dec;
}

Bytes cp932(const char* utf8) {
    return utf8_to_cp932_replace(utf8);
}

}  // namespace

TEST(SptParser, decrypt_is_xor_ff_and_self_inverse) {
    const Bytes plain{0x00, 0x41, 0xFF, 0x80};
    const Bytes enc = spt::decrypt(plain);
    EXPECT_EQ(enc, (Bytes{0xFF, 0xBE, 0x00, 0x7F}));
    EXPECT_EQ(spt::decrypt(enc), plain);
}

TEST(SptParser, validity_requires_header_magic) {
    EXPECT_TRUE(spt::is_valid(make_spt({})));
    Bytes bad = make_spt({});
    bad[kMagicOffset] = 'X';
    EXPECT_FALSE(spt::is_valid(bad));
    EXPECT_FALSE(spt::is_valid(Bytes(kShortInvalidBufSize, 0)));
}

TEST(SptParser, reads_header_string_count) {
    EXPECT_EQ(spt::header_string_count(make_spt({}, kTestHeaderCount)), kTestHeaderCount);
}

TEST(SptParser, extracts_strings_with_offsets) {
    const Bytes dec = make_spt({cp932("あい"), cp932("hello")});
    const auto out = spt::extract_strings(dec);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].offset, kHeaderSize);
    EXPECT_EQ(out[0].text, "あい");
    EXPECT_TRUE(out[0].has_jp);
    EXPECT_EQ(out[1].text, "hello");
    EXPECT_FALSE(out[1].has_jp);
    EXPECT_EQ(out[1].offset % kAlignment, 0u);
}

TEST(SptParser, drops_bytecode_noise_and_short_ascii) {
    const Bytes dec = make_spt({cp932("ff"), cp932("UU"), cp932("abc"), cp932("abcd")});
    const auto out = spt::extract_strings(dec);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].text, "abcd");
}

TEST(SptParser, keeps_short_japanese) {
    const Bytes dec = make_spt({cp932("心")});
    const auto out = spt::extract_strings(dec);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(out[0].has_jp);
}

TEST(SptParser, has_japanese_covers_fullwidth_and_symbols) {
    EXPECT_TRUE(spt::has_japanese("あ"));
    EXPECT_TRUE(spt::has_japanese("Ａ"));
    EXPECT_TRUE(spt::has_japanese("「"));
    EXPECT_FALSE(spt::has_japanese("plain ascii"));
}

TEST(SptParser, classify_rules) {
    EXPECT_EQ(spt::classify("アゲハ\r\n「あ」"), "dialogue");
    EXPECT_EQ(spt::classify("あいうえおかきくけこさ\r\nし"), "narrative");
    EXPECT_EQ(spt::classify("「あ」"), "dialogue");
    EXPECT_EQ(spt::classify("★ start"), "menu");
    EXPECT_EQ(spt::classify("bgm_01"), "label");
    EXPECT_EQ(spt::classify("アゲハ"), "name");
    EXPECT_EQ(spt::classify("あいうえおかき"), "narrative");
    EXPECT_EQ(spt::classify("   "), "empty");
}

TEST(SptParser, spt_script_pimpl_decrypts_classifies_and_supports_move) {
    const Bytes dec = make_spt({cp932("アゲハ\r\n「あ」"), cp932("bgm_01")}, kTestHeaderCount);
    const Bytes raw = spt::decrypt(dec);

    spt::SptScript script(raw);
    EXPECT_TRUE(script.is_valid());
    EXPECT_EQ(script.file_size(), raw.size());
    EXPECT_EQ(script.header_string_count(), kTestHeaderCount);
    EXPECT_EQ(script.raw(), raw);
    EXPECT_EQ(script.decrypted(), dec);

    spt::SptScript moved(std::move(script));
    EXPECT_TRUE(moved.is_valid());
    ASSERT_EQ(moved.strings().size(), 2u);
    EXPECT_EQ(moved.strings()[0].text, "アゲハ\r\n「あ」");
    EXPECT_EQ(moved.strings()[0].type, "dialogue");
    EXPECT_TRUE(moved.strings()[0].has_jp);
    EXPECT_EQ(moved.strings()[1].text, "bgm_01");
    EXPECT_EQ(moved.strings()[1].type, "label");
    EXPECT_FALSE(moved.strings()[1].has_jp);

    const spt::SptScript invalid(Bytes(kShortInvalidBufSize, 0));
    EXPECT_FALSE(invalid.is_valid());
    EXPECT_EQ(invalid.header_string_count(), 0u);
    EXPECT_TRUE(invalid.strings().empty());
}
