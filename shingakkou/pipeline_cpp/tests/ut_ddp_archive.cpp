// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// DDP3 container / ShsCompression / DDWuHXB tests.
//
// The control-byte cases below matter disproportionately: three of them
// (0x1F, 0x60+0xFE, 0x60+0xFF) have ZERO coverage in the shipped archive, so
// nothing else in the pipeline would catch a transcription error in them.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "ddp/ddp_archive.h"

using namespace shin;
namespace ddp = shin::ddp;

namespace {

Bytes B(std::initializer_list<int> v) {
    Bytes out;
    for (int x : v) out.push_back(static_cast<std::uint8_t>(x));
    return out;
}

std::string as_string(const Bytes& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

void append(Bytes& dst, std::initializer_list<int> v) {
    for (int x : v) dst.push_back(static_cast<std::uint8_t>(x));
}

void append_n(Bytes& dst, char c, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) dst.push_back(static_cast<std::uint8_t>(c));
}

void put_u32le(Bytes& dst, std::uint32_t v) {
    dst.push_back(static_cast<std::uint8_t>(v));
    dst.push_back(static_cast<std::uint8_t>(v >> 8));
    dst.push_back(static_cast<std::uint8_t>(v >> 16));
    dst.push_back(static_cast<std::uint8_t>(v >> 24));
}

}  // namespace

// A back-reference whose source range overlaps the bytes it is writing.  1,345
// of these occur in the real archive; memcpy is UB and memmove would emit the
// pre-copy (zeroed) window instead of the RLE expansion.
TEST(Shs, decompress_overlapping_backref) {
    // 0x00 -> 1 literal 'A'; 0x60 00 03 -> offset 1, count 10.
    const Bytes src = B({0x00, 'A', 0x60, 0x00, 0x03});
    EXPECT_EQ(as_string(ddp::shs_decompress(src, 11)), std::string(11, 'A'));
}

TEST(Shs, decompress_all_control_branches) {
    Bytes src;
    append(src, {0x04, 'A', 'B', 'C', 'D', 'E'});  // literal-short: count = ctl+1
    append(src, {0x20});                           // 0x20 block: offset 1, count 3
    append(src, {0x40, 0x00});                     // 0x40 block: offset 1, count 7
    append(src, {0xA0, 0x00});                     // 0x80 block: offset 1, count 4
    append(src, {0x1D, 0x00});                     // literal: count = b + 0x1E = 30
    append_n(src, 'F', 30);
    append(src, {0x1E, 0x00, 0x00});               // literal: count = BE16 + 0x11E = 286
    append_n(src, 'G', 286);
    append(src, {0x1F, 0x00, 0x00, 0x00, 0x05});   // literal: count = BE32, no bias
    append(src, {'H', 'I', 'J', 'K', 'L'});
    append(src, {0x60, 0x00, 0xFE, 0x00, 0x00});   // backref: count = BE16 + 0x102 + 3
    append(src, {0x60, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x02});  // backref: count = BE32 + 3

    const std::size_t total = 4 + 15 + 30 + 286 + 4 + 267;
    ASSERT_EQ(total, 606u);
    const std::string expect = std::string("ABCD") + std::string(15, 'E') +
                               std::string(30, 'F') + std::string(286, 'G') +
                               std::string("HIJK") + std::string(267, 'L');
    EXPECT_EQ(as_string(ddp::shs_decompress(src, 606)), expect);
}

// Pins the 32-bit wraparound of the key multiply: computing it in int64 (or
// letting a signed type overflow) silently changes every decrypted byte.
TEST(Hxb, decrypt_key_derivation) {
    Bytes data(0x14, 0x00);
    // Plaintext header: "DDWuHXB\0" then the BE24 length -- 53443 is main05's.
    const char magic[] = "DDWuHXB";
    for (int i = 0; i < 7; ++i) data[i] = static_cast<std::uint8_t>(magic[i]);
    data[8] = 0x00;
    data[9] = 0xD0;
    data[10] = 0xC3;  // 0x00D0C3 == 53443
    data[0x10] = 0x00;
    data[0x11] = 0x00;
    data[0x12] = 0x00;
    data[0x13] = 0x00;

    const Bytes dec = ddp::decrypt_hxb(data);
    // key = (((53443<<5) ^ 0xA5) * (53443 + 0x6F349)) ^ 0x34A9B129  == 0x9D3B4C15
    EXPECT_EQ(dec[0x10], 0x15);
    EXPECT_EQ(dec[0x11], 0x4C);
    EXPECT_EQ(dec[0x12], 0x3B);
    EXPECT_EQ(dec[0x13], 0x9D);
    // Everything below 0x10 is plaintext and must be untouched.
    for (std::size_t i = 0; i < 0x10; ++i) EXPECT_EQ(dec[i], data[i]) << "byte " << i;
}

TEST(Hxb, decrypt_short_input_passthrough) {
    const Bytes data(0x10, 0xAB);
    EXPECT_EQ(ddp::decrypt_hxb(data), data);
}

TEST(Ddp3, rejects_bad_magic) {
    EXPECT_THROW(ddp::parse_ddp3(B({'D', 'D', 'P', '2', 0, 0, 0, 0})), std::runtime_error);
    EXPECT_THROW(ddp::parse_ddp3(Bytes{}), std::runtime_error);
}

TEST(Ddp3, walks_entries) {
    // Two populated sections plus one empty one.  Section 1 carries a zero
    // terminator byte after its single entry; section 2's second entry claims
    // more bytes than the block holds and must be dropped.
    const auto make_entry = [](std::uint8_t len, std::uint32_t off, std::uint32_t unp,
                               std::uint32_t pck, const std::string& name) {
        Bytes e;
        e.push_back(len);
        put_u32le(e, off);
        put_u32le(e, unp);
        put_u32le(e, pck);
        put_u32le(e, 0);  // e[13..17) padding
        for (char c : name) {
            e.push_back(static_cast<std::uint8_t>(c));
            e.push_back(0);
        }
        while (e.size() < len) e.push_back(0);  // trailing NULs -> rstrip('\x00')
        return e;
    };

    Bytes sec0 = make_entry(31, 0x1000, 100, 50, "main05");  // 17 + 12 + 2 NUL = 31
    sec0.push_back(0);                                       // entry_len == 0 -> stop
    Bytes sec2 = make_entry(43, 0x2000, 200, 60, "gabriel_ed_good");  // 17 + 30 = 47 > 43
    sec2.resize(43);
    sec2.push_back(60);  // claims 60 bytes but only 1 remains -> dropped

    const std::uint32_t sec0_off = 32 + 3 * 8;
    const std::uint32_t sec2_off = sec0_off + static_cast<std::uint32_t>(sec0.size());

    Bytes data;
    append(data, {'D', 'D', 'P', '3'});
    put_u32le(data, 3);  // num_outer
    while (data.size() < 32) data.push_back(0);
    put_u32le(data, static_cast<std::uint32_t>(sec0.size()));
    put_u32le(data, sec0_off);
    put_u32le(data, 0);  // zero-length section -> skipped
    put_u32le(data, 0);
    put_u32le(data, static_cast<std::uint32_t>(sec2.size()));
    put_u32le(data, sec2_off);
    data.insert(data.end(), sec0.begin(), sec0.end());
    data.insert(data.end(), sec2.begin(), sec2.end());

    const auto scripts = ddp::parse_ddp3(data);
    ASSERT_EQ(scripts.size(), 2u);
    EXPECT_EQ(scripts[0].name, "main05");
    EXPECT_EQ(scripts[0].offset, 0x1000u);
    EXPECT_EQ(scripts[0].decomp_size, 100u);
    EXPECT_EQ(scripts[0].comp_size, 50u);
    // The name slice starts at byte 17 and 43-17 = 26 bytes hold 13 UTF-16
    // units; the truncated tail of "gabriel_ed_good" is what fits.
    EXPECT_EQ(scripts[1].name, "gabriel_ed_go");
    EXPECT_EQ(scripts[1].offset, 0x2000u);
}
