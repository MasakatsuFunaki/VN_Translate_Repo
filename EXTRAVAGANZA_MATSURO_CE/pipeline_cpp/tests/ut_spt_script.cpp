// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// SPT reader tests: the XOR-0xFF envelope, the 4-byte-aligned string walk,
// and the classifier's rule ordering.
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "common/util.h"
#include "spt/spt_script.h"

using namespace exm;

namespace {

// Build a decrypted SPT image: 0x80 header (with the magic at 0x30) followed
// by NUL-terminated, 4-byte-aligned CP932 strings.
Bytes make_spt(const std::vector<Bytes>& payloads, std::uint32_t hdr_count = 0) {
    Bytes dec(0x80, 0x20);
    std::memcpy(dec.data() + 0x30, "SPTHEADER0", 10);
    dec[0x18] = static_cast<std::uint8_t>(hdr_count);
    dec[0x19] = static_cast<std::uint8_t>(hdr_count >> 8);
    dec[0x1A] = static_cast<std::uint8_t>(hdr_count >> 16);
    dec[0x1B] = static_cast<std::uint8_t>(hdr_count >> 24);
    for (const auto& p : payloads) {
        dec.insert(dec.end(), p.begin(), p.end());
        dec.push_back(0);
        while (dec.size() % 4) dec.push_back(0);
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
    bad[0x30] = 'X';
    EXPECT_FALSE(spt::is_valid(bad));
    EXPECT_FALSE(spt::is_valid(Bytes(0x20, 0)));
}

TEST(SptScript, reads_header_string_count) {
    EXPECT_EQ(spt::header_string_count(make_spt({}, 0x1234)), 0x1234u);
}

TEST(SptScript, extracts_strings_with_offsets) {
    const Bytes dec = make_spt({cp932("\xE3\x81\x82\xE3\x81\x84"), cp932("hello")});
    auto out = spt::extract_strings(dec);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].offset, 0x80u);
    EXPECT_EQ(out[0].text, "\xE3\x81\x82\xE3\x81\x84");
    EXPECT_TRUE(out[0].has_jp);
    EXPECT_EQ(out[1].text, "hello");
    EXPECT_FALSE(out[1].has_jp);
    // Second string starts on the next 4-byte boundary after the first NUL.
    EXPECT_EQ(out[1].offset % 4, 0u);
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
    const Bytes dec = make_spt({cp932("\xE5\xBF\x83")});  // 心
    auto out = spt::extract_strings(dec);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(out[0].has_jp);
}

// The extractor's JP test is deliberately wider than the translator's: it
// also accepts fullwidth forms and CJK symbols.
TEST(SptScript, has_japanese_covers_fullwidth_and_symbols) {
    EXPECT_TRUE(spt::has_japanese("\xE3\x81\x82"));          // あ hiragana
    EXPECT_TRUE(spt::has_japanese("\xEF\xBC\xA1"));          // Ａ fullwidth
    EXPECT_TRUE(spt::has_japanese("\xE3\x80\x8C"));          // 「 CJK symbol
    EXPECT_FALSE(spt::has_japanese("plain ascii"));
}

TEST(SptScript, classify_rules) {
    // Short first segment before a CRLF -> speaker plate -> dialogue.
    EXPECT_EQ(spt::classify("\xE7\xBE\x8E\xE5\xBC\xA5\xE9\xA6\x99\r\n\xE3\x80\x8C"
                            "\xE3\x81\x82\xE3\x80\x8D"),
              "dialogue");
    // Long first segment -> narrative.
    EXPECT_EQ(spt::classify(
                  "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86\xE3\x81\x88\xE3\x81\x8A"
                  "\xE3\x81\x8B\xE3\x81\x8D\xE3\x81\x8F\xE3\x81\x91\xE3\x81\x93"
                  "\xE3\x81\x95\r\n\xE3\x81\x97"),
              "narrative");
    // Bracketed one-liner -> dialogue.
    EXPECT_EQ(spt::classify("\xE3\x80\x8C\xE3\x81\x82\xE3\x80\x8D"), "dialogue");
    // Menu markers.
    EXPECT_EQ(spt::classify("\xE2\x98\x85 start"), "menu");
    // Pure ASCII -> label.
    EXPECT_EQ(spt::classify("bgm_01"), "label");
    // Short JP -> name; long JP -> narrative.
    EXPECT_EQ(spt::classify("\xE7\xBE\x8E\xE5\xBC\xA5\xE9\xA6\x99"), "name");
    EXPECT_EQ(spt::classify("\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86\xE3\x81\x88"
                            "\xE3\x81\x8A\xE3\x81\x8B\xE3\x81\x8D"),
              "narrative");
    EXPECT_EQ(spt::classify("   "), "empty");
}
