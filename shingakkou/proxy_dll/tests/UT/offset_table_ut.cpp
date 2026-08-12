// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — tail offset-table location for shingakkou.
//
// Tests translator_logic::FindOffsetTable, which locates the script's
// tail offset table (3-byte big-endian, ascending, terminated by 0xFF,
// each entry pointing at an FF 01 80 marker). The regression that
// motivated this function: the original walk assumed every entry's high
// byte was 0x00 (offsets < 64KB) and therefore found NO table on any
// script larger than 64KB -- the whole main story -- leaving saves
// taken there unrelocatable after translation. LargeScriptHighByteEntries
// pins that case.
//
// All inputs are synthetic in-memory buffers; no file I/O.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using translator_logic::FindOffsetTable;
using translator_logic::OffsetTable;

namespace {

void PushBE24(std::vector<unsigned char>& b, std::uint32_t v) {
    b.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    b.push_back(static_cast<unsigned char>(v & 0xFF));
}

// Lay down a text record (03 0d 36 preamble + FF 01 80 marker) at `off`.
// FindOffsetTable only validates bytes [off+3..off+5].
void PutMarker(std::vector<unsigned char>& b, std::size_t off) {
    b[off + 0] = 0x03; b[off + 1] = 0x0d; b[off + 2] = 0x36;
    b[off + 3] = 0xFF; b[off + 4] = 0x01; b[off + 5] = 0x80;
}

}  // namespace

TEST(FindOffsetTable, EmptyOrUnterminated) {
    std::vector<unsigned char> tiny(3, 0xFF);
    EXPECT_EQ(FindOffsetTable(tiny.data(), tiny.size()).count, 0);

    std::vector<unsigned char> notTerminated(64, 0x00);
    EXPECT_EQ(FindOffsetTable(notTerminated.data(), notTerminated.size()).count, 0);
}

TEST(FindOffsetTable, SmallScriptAllLowOffsets) {
    std::vector<unsigned char> b(0x400, 0x00);
    const std::size_t m0 = 0x40, m1 = 0x80, m2 = 0x120;
    PutMarker(b, m0); PutMarker(b, m1); PutMarker(b, m2);

    const std::size_t tableStart = b.size();
    PushBE24(b, static_cast<std::uint32_t>(m0));
    PushBE24(b, static_cast<std::uint32_t>(m1));
    PushBE24(b, static_cast<std::uint32_t>(m2));
    b.push_back(0xFF);  // terminator

    const OffsetTable t = FindOffsetTable(b.data(), b.size());
    EXPECT_EQ(t.start, tableStart);
    EXPECT_EQ(t.count, 3);
}

TEST(FindOffsetTable, LargeScriptHighByteEntries) {
    // Regression for the >64KB bug: an entry whose offset needs a
    // non-zero third byte (>= 0x010000). The old high-byte==0x00 walk
    // found ZERO entries here.
    const std::size_t big = 0x010000;  // 65536
    std::vector<unsigned char> b(big + 0x100, 0x00);
    const std::size_t m0 = 0x100;      // low offset  (high byte 0x00)
    const std::size_t m1 = big + 0x10; // high offset (high byte 0x01)
    PutMarker(b, m0); PutMarker(b, m1);

    const std::size_t tableStart = b.size();
    PushBE24(b, static_cast<std::uint32_t>(m0));
    PushBE24(b, static_cast<std::uint32_t>(m1));
    b.push_back(0xFF);

    const OffsetTable t = FindOffsetTable(b.data(), b.size());
    EXPECT_EQ(t.start, tableStart);
    EXPECT_EQ(t.count, 2);
}

TEST(FindOffsetTable, StopsBeforeNonMarkerEntry) {
    // A 3-byte value preceding the table that points at a non-marker
    // must terminate the walk, not be swallowed as an entry.
    std::vector<unsigned char> b(0x400, 0x00);
    const std::size_t m0 = 0x40, m1 = 0x80;
    PutMarker(b, m0); PutMarker(b, m1);

    PushBE24(b, 0x000010);  // offset 0x10 has no FF 01 80 -> boundary
    const std::size_t tableStart = b.size();
    PushBE24(b, static_cast<std::uint32_t>(m0));
    PushBE24(b, static_cast<std::uint32_t>(m1));
    b.push_back(0xFF);

    const OffsetTable t = FindOffsetTable(b.data(), b.size());
    EXPECT_EQ(t.count, 2);
    EXPECT_EQ(t.start, tableStart);
}
