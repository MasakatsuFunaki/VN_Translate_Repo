// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// LZSS decoder tests.  The magic numbers (ring 4096 @ 0x20, start 0xFEE,
// 12-bit offset, length + 3) are the whole format -- an off-by-one anywhere
// shifts every string offset in extracted_text.json.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "willplus/sn_archive.h"

using namespace crc;
using crc::willplus::lzss_decompress;

namespace {

Bytes with_header(std::uint32_t expected, const Bytes& stream) {
    Bytes out{static_cast<std::uint8_t>(expected & 0xFF),
              static_cast<std::uint8_t>((expected >> 8) & 0xFF),
              static_cast<std::uint8_t>((expected >> 16) & 0xFF),
              static_cast<std::uint8_t>((expected >> 24) & 0xFF)};
    out.insert(out.end(), stream.begin(), stream.end());
    return out;
}

// Flag 0xFF + 8 literals per group -- the encoding the synthetic-corpus
// generator uses, so this also validates the verification harness.
Bytes all_literal(const Bytes& payload) {
    Bytes out;
    for (std::size_t i = 0; i < payload.size(); i += 8) {
        out.push_back(0xFF);
        for (std::size_t j = i; j < i + 8 && j < payload.size(); ++j) out.push_back(payload[j]);
    }
    return out;
}

}  // namespace

TEST(SnArchive, LzssAllLiteralRoundTrips) {
    Bytes payload;
    for (int i = 0; i < 36; ++i) payload.push_back(static_cast<std::uint8_t>('A' + (i % 26)));
    const Bytes got = lzss_decompress(with_header(36, all_literal(payload)));
    EXPECT_EQ(got, payload);
}

TEST(SnArchive, LzssBackReferenceUsesRingAndWraps) {
    // Flag 0x00 => slot 0 is a back-reference into never-written ring space,
    // which must read back the 0x20 pre-fill.  b2 == 0x00 means length 0+3.
    const Bytes fill = lzss_decompress(with_header(0, Bytes{0x00, 0x00, 0x00}));
    EXPECT_EQ(fill, (Bytes{0x20, 0x20, 0x20}));

    // ref_off = b1 | ((b2 & 0xF0) << 4): the high nibble supplies bits 8-11.
    // Group 1 writes ABCDEFGH at ring[0xFEE..0xFF5]; group 2's reference
    // (b1 = 0xEE, b2 = 0xF5 -> off 0xFEE, len 5 + 3 = 8) copies it back.
    Bytes stream{0xFF, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
    stream.push_back(0x00);  // all-reference flag byte
    stream.push_back(0xEE);
    stream.push_back(0xF5);
    const Bytes got = lzss_decompress(with_header(0, stream));
    EXPECT_EQ(std::string(got.begin(), got.end()), "ABCDEFGHABCDEFGH");

    // (ref_off + j) & 0xFFF: a reference straddling the end of the ring must
    // wrap to index 0 instead of running off the end of the buffer.
    Bytes wrap{0x00, 0xFE, 0xF2};  // off = 0xFE | 0xF00 = 0xFFE, len = 2 + 3 = 5
    const Bytes wrapped = lzss_decompress(with_header(0, wrap));
    EXPECT_EQ(wrapped, (Bytes{0x20, 0x20, 0x20, 0x20, 0x20}));
}

TEST(SnArchive, LzssStopsMidFlagByteWithoutOverrun) {
    // Flag says 8 literals but only 3 bytes follow: the `si >= src_len` check
    // at the top of each bit iteration must return the partial output.
    const Bytes stream{0xFF, 'a', 'b', 'c'};
    const Bytes got = lzss_decompress(with_header(999, stream));
    EXPECT_EQ(got, (Bytes{'a', 'b', 'c'}));

    // A 2-byte reference with only one byte left must also bail out cleanly.
    const Bytes stream2{0x01, 'a', 0x00};
    const Bytes got2 = lzss_decompress(with_header(0, stream2));
    EXPECT_EQ(got2, (Bytes{'a'}));
}

TEST(SnArchive, LzssIgnoresHeaderSizeMismatch) {
    Bytes payload{'x', 'y', 'z'};
    // Header claims 5,000,000 bytes; nothing is padded or truncated to it.
    const Bytes got = lzss_decompress(with_header(5000000, all_literal(payload)));
    EXPECT_EQ(got, payload);
    EXPECT_EQ(willplus::sn_expected_size(with_header(5000000, {})), 5000000u);
}
