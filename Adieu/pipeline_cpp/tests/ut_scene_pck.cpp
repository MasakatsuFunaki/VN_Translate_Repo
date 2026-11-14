// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Scene.pck container layout: which u32 is which, and the slice clamping in
// the name tables.
#include "siglus/scene_pck.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

using namespace adi;
using namespace adi::siglus;

namespace {

constexpr std::uint8_t kByteMask = 0xFFu;
constexpr unsigned kShift8 = 8;
constexpr unsigned kShift16 = 16;
constexpr unsigned kShift24 = 24;
constexpr std::size_t kHeaderBufSize = 0x5Cu;
constexpr std::uint32_t kHeaderSizeValue = 0x5Cu;
constexpr std::size_t kSectionTableOffset = 4u;
constexpr std::size_t kSectionStride = 8u;
constexpr std::size_t kSectionCountOffset = 4u;
constexpr std::uint32_t kSyntheticBaseOffset = 0x1000u;
constexpr std::uint32_t kSyntheticCountMultiplier = 3u;
constexpr std::size_t kEvenTruncatedBufSize = 0x14u;
constexpr std::size_t kOddTruncatedBufSize = 0x15u;
constexpr std::uint32_t kTestBlobOffset = 0x10u;
constexpr std::uint32_t kRequestedWideUnits = 4u;
constexpr std::uint32_t kSingleEntryCount = 1u;
constexpr std::size_t kChar0ByteOffset = 0x10u;
constexpr std::size_t kChar0HighByteOffset = 0x11u;
constexpr std::size_t kChar1ByteOffset = 0x12u;
constexpr std::size_t kChar1HighByteOffset = 0x13u;
constexpr std::size_t kChar2ByteOffset = 0x14u;
constexpr std::uint32_t kPastEofWideOffset = 0x100u;
constexpr std::uint32_t kTwoWideUnits = 2u;
constexpr std::uint64_t kSliceStart1 = 1u;
constexpr std::uint64_t kSliceLen2 = 2u;
constexpr std::uint64_t kSliceStart3 = 3u;
constexpr std::uint64_t kOversizedSliceLen = 100u;
constexpr std::uint64_t kSliceAtEnd = 5u;
constexpr std::uint64_t kSlicePastEnd = 99u;
constexpr std::uint64_t kSliceLen1 = 1u;

void put32(Bytes& b, std::size_t off, std::uint32_t v) {
    b[off] = static_cast<std::uint8_t>(v & kByteMask);
    b[off + 1] = static_cast<std::uint8_t>((v >> kShift8) & kByteMask);
    b[off + 2] = static_cast<std::uint8_t>((v >> kShift16) & kByteMask);
    b[off + 3] = static_cast<std::uint8_t>((v >> kShift24) & kByteMask);
}

}  // namespace

TEST(ScenePck, HeaderFieldPositions) {
    Bytes b(kHeaderBufSize, 0);
    put32(b, 0, kHeaderSizeValue);
    for (int i = 0; i < SECTION_COUNT; ++i) {
        const std::size_t entry_off =
            kSectionTableOffset + static_cast<std::size_t>(i) * kSectionStride;
        put32(b, entry_off, kSyntheticBaseOffset + static_cast<std::uint32_t>(i));
        put32(b, entry_off + kSectionCountOffset,
              static_cast<std::uint32_t>(i) * kSyntheticCountMultiplier);
    }
    const Header h = parse_header(b);
    EXPECT_EQ(h.header_size, kHeaderSizeValue);
    for (int i = 0; i < SECTION_COUNT; ++i) {
        EXPECT_EQ(h.sections[i].offset, kSyntheticBaseOffset + static_cast<std::uint32_t>(i));
        EXPECT_EQ(h.sections[i].count,
                  static_cast<std::uint32_t>(i) * kSyntheticCountMultiplier);
    }
}

TEST(ScenePck, NameTableClampsOutOfRangeSlice) {
    // Index says 4 wchars from blob_off 0x10, i.e. bytes [0x10, 0x18) -- but
    // the file ends at 0x14.  The slice clamps rather than raising.
    Bytes b(kEvenTruncatedBufSize, 0);
    put32(b, 0, 0);
    put32(b, kSectionCountOffset, kRequestedWideUnits);
    b[kChar0ByteOffset] = 'A';
    b[kChar0HighByteOffset] = 0x00;
    b[kChar1ByteOffset] = 'B';
    b[kChar1HighByteOffset] = 0x00;
    auto names = read_name_table(b, 0, kSingleEntryCount, kTestBlobOffset);
    ASSERT_EQ(names.size(), kSingleEntryCount);
    EXPECT_EQ(names[0], "AB");

    // One byte more: the clamp now lands on an ODD byte count, and the lone
    // trailing byte decodes to a single U+FFFD.
    Bytes t(kOddTruncatedBufSize, 0);
    put32(t, 0, 0);
    put32(t, kSectionCountOffset, kRequestedWideUnits);
    t[kChar0ByteOffset] = 'A';
    t[kChar0HighByteOffset] = 0x00;
    t[kChar1ByteOffset] = 'B';
    t[kChar1HighByteOffset] = 0x00;
    t[kChar2ByteOffset] = 'C';
    names = read_name_table(t, 0, kSingleEntryCount, kTestBlobOffset);
    EXPECT_EQ(names[0], std::string("AB") + "\xEF\xBF\xBD");

    // A start beyond EOF yields the empty string, not a throw.
    Bytes s(kEvenTruncatedBufSize, 0);
    put32(s, 0, kPastEofWideOffset);
    put32(s, kSectionCountOffset, kTwoWideUnits);
    names = read_name_table(s, 0, kSingleEntryCount, kTestBlobOffset);
    EXPECT_EQ(names[0], "");
}

TEST(ScenePck, SliceClampedTruncatesAndNeverThrows) {
    const Bytes d = {0, 1, 2, 3, 4};
    EXPECT_EQ(slice_clamped(d, kSliceStart1, kSliceLen2), (Bytes{1, 2}));
    EXPECT_EQ(slice_clamped(d, kSliceStart3, kOversizedSliceLen), (Bytes{3, 4}));
    EXPECT_TRUE(slice_clamped(d, kSliceAtEnd, kSliceLen1).empty());
    EXPECT_TRUE(slice_clamped(d, kSlicePastEnd, kSliceLen1).empty());
}
