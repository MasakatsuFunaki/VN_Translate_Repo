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

using namespace mgi;

namespace {

constexpr std::size_t kSptHeaderSize = 0x80u;
constexpr std::uint8_t kSptSpaceFill = 0x20u;
constexpr std::size_t kSptMagicOffset = 0x30u;
constexpr std::size_t kSptMagicLen = 10u;
constexpr std::size_t kSptCountOffset = 0x18u;
constexpr std::uint32_t kByteShift1 = 8u;
constexpr std::uint32_t kByteShift2 = 16u;
constexpr std::uint32_t kByteShift3 = 24u;
constexpr std::size_t kSptStringAlignment = 4u;
constexpr std::size_t kTruncatedHeaderSize = 0x20u;
constexpr std::uint32_t kSampleHeaderCount = 0x1234u;

// Build a decrypted SPT image: 0x80 header (with the magic at 0x30) followed
// by NUL-terminated, 4-byte-aligned CP932 strings.
Bytes make_spt(const std::vector<Bytes>& payloads, std::uint32_t hdr_count = 0) {
    Bytes dec(kSptHeaderSize, kSptSpaceFill);
    std::memcpy(dec.data() + kSptMagicOffset, "SPTHEADER0", kSptMagicLen);
    dec[kSptCountOffset + 0] = static_cast<std::uint8_t>(hdr_count);
    dec[kSptCountOffset + 1] = static_cast<std::uint8_t>(hdr_count >> kByteShift1);
    dec[kSptCountOffset + 2] = static_cast<std::uint8_t>(hdr_count >> kByteShift2);
    dec[kSptCountOffset + 3] = static_cast<std::uint8_t>(hdr_count >> kByteShift3);
    for (const auto& p : payloads) {
        dec.insert(dec.end(), p.begin(), p.end());
        dec.push_back(0u);
        while (dec.size() % kSptStringAlignment != 0u) {
            dec.push_back(0u);
        }
    }
    return dec;
}

Bytes cp932(const char* utf8) {
    return utf8_to_cp932_replace(utf8);
}

}  // namespace

TEST(SptScript, decrypt_is_xor_ff_and_self_inverse) {
    const Bytes plain{0x00u, 0x41u, 0xFFu, 0x80u};
    const Bytes enc = spt::decrypt(plain);
    EXPECT_EQ(enc, (Bytes{0xFFu, 0xBEu, 0x00u, 0x7Fu}));
    EXPECT_EQ(spt::decrypt(enc), plain);
}

TEST(SptScript, validity_requires_header_magic) {
    EXPECT_TRUE(spt::is_valid(make_spt({})));
    Bytes bad = make_spt({});
    bad[kSptMagicOffset] = 'X';
    EXPECT_FALSE(spt::is_valid(bad));
    EXPECT_FALSE(spt::is_valid(Bytes(kTruncatedHeaderSize, 0u)));
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
    // Second string starts on the next 4-byte boundary after the first NUL.
    EXPECT_EQ(out[1].offset % kSptStringAlignment, 0u);
}

// Short non-JP runs are opcode operands, not text.
TEST(SptScript, drops_bytecode_noise_and_short_ascii) {
    const Bytes dec = make_spt({cp932("ff"), cp932("UU"), cp932("abc"), cp932("abcd")});
    auto out = spt::extract_strings(dec);
    // 'ff'/'UU' are on the noise list; 'abc' is <4 bytes and non-JP.
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].text, "abcd");
}

// A 2-byte JP string is kept even though it's under the 4-byte floor.
TEST(SptScript, keeps_short_japanese) {
    const Bytes dec = make_spt({cp932("心")});
    auto out = spt::extract_strings(dec);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(out[0].has_jp);
}

// The extractor's JP test is deliberately wider than the translator's: it
// also accepts fullwidth forms and CJK symbols.
TEST(SptScript, has_japanese_covers_fullwidth_and_symbols) {
    EXPECT_TRUE(spt::has_japanese("あ"));
    EXPECT_TRUE(spt::has_japanese("Ａ"));
    EXPECT_TRUE(spt::has_japanese("「"));
    EXPECT_FALSE(spt::has_japanese("plain ascii"));
}

TEST(SptScript, classify_rules) {
    // Short first segment before a CRLF -> speaker plate -> dialogue.
    EXPECT_EQ(spt::classify("美弥香\r\n「あ」"), "dialogue");
    // Long first segment -> narrative.
    EXPECT_EQ(spt::classify("あいうえおかきくけこさ\r\nし"), "narrative");
    // Bracketed one-liner -> dialogue.
    EXPECT_EQ(spt::classify("「あ」"), "dialogue");
    // Menu markers.
    EXPECT_EQ(spt::classify("★ start"), "menu");
    // Pure ASCII -> label.
    EXPECT_EQ(spt::classify("bgm_01"), "label");
    // Short JP -> name; long JP -> narrative.
    EXPECT_EQ(spt::classify("美弥香"), "name");
    EXPECT_EQ(spt::classify("あいうえおかき"), "narrative");
    EXPECT_EQ(spt::classify("   "), "empty");
}
