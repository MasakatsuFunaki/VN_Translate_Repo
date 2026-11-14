// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Contract-driven unit tests for SiglusEngine .g00 sprite decoding, encoding,
// LZSS framing, Type 2 multi-chip union-bbox consolidation, and metadata parity
// verification.
#include "g00/g00_codec.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINGDI_) || defined(__wincodec_h__)
#error "Windows header leaked"
#endif

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace adi::g00 {
namespace {

using imaging::BgraColor;
using imaging::BgraImage;
using imaging::RectI;

constexpr std::uint8_t kByteMask = 0xFFu;
constexpr unsigned kShift8 = 8u;
constexpr unsigned kShift16 = 16u;
constexpr unsigned kShift24 = 24u;
constexpr std::uint8_t kAllLiteralFlags = 0xFFu;
constexpr int kBitsPerFlagByte = 8;

constexpr std::uint16_t kType0Width = 3u;
constexpr std::uint16_t kType0Height = 2u;
constexpr std::uint32_t kType0UnpBytes = 24u;
constexpr std::uint32_t kType0ArcBytes = 19u;

constexpr std::uint16_t kWindowTestWidth = 65u;
constexpr std::uint16_t kWindowTestHeight = 65u;
constexpr std::size_t kWindowTestPixels = 4225u;
constexpr std::size_t kBeyondWindowPixelIdx = 4100u;
constexpr std::size_t kWithinWindowSrcIdx = 30u;
constexpr std::size_t kWithinWindowDstIdx = 4120u;
constexpr std::size_t kPatternPrefixCount = 16u;

constexpr std::size_t kByteWindowTotalLen = 4136u;
constexpr std::size_t kByteBeyondWindowOffset = 4104u;
constexpr std::size_t kByteWithinWindowDstOffset = 4120u;
constexpr std::size_t kByteWithinWindowSrcOffset = 30u;

constexpr std::size_t kOverlapRleCount = 18u;
constexpr std::uint8_t kOverlapFillByte = 0xAAu;

constexpr std::uint16_t kType1Width = 2u;
constexpr std::uint16_t kType1Height = 2u;
constexpr std::uint16_t kType1PaletteCount = 3u;

constexpr std::uint16_t kType2CanvasWidth = 32u;
constexpr std::uint16_t kType2CanvasHeight = 24u;
constexpr std::uint32_t kType2CutCount = 3u;

constexpr std::int32_t kCut0X1 = 2;
constexpr std::int32_t kCut0Y1 = 3;
constexpr std::int32_t kCut0X2 = 5;
constexpr std::int32_t kCut0Y2 = 4;
constexpr std::int32_t kCut0OriginX = 11;
constexpr std::int32_t kCut0OriginY = -7;
constexpr std::int32_t kCut0Width = 4;
constexpr std::int32_t kCut0Height = 2;
constexpr std::int32_t kCut0CenterX = 2;
constexpr std::int32_t kCut0CenterY = 1;

constexpr std::int32_t kCut1OrigX1 = 6;
constexpr std::int32_t kCut1OrigY1 = 8;
constexpr std::int32_t kCut1OrigX2 = 17;
constexpr std::int32_t kCut1OrigY2 = 11;
constexpr std::int32_t kCut1OriginX = 100;
constexpr std::int32_t kCut1OriginY = 200;
constexpr std::uint16_t kCut1CutType = 2u;
constexpr std::uint16_t kCut1OrigPartCount = 3u;
constexpr std::int32_t kCut1OrigBboxX = 1;
constexpr std::int32_t kCut1OrigBboxY = 0;
constexpr std::int32_t kCut1OrigBboxW = 10;
constexpr std::int32_t kCut1OrigBboxH = 4;
constexpr std::int32_t kCut1CenterX = 5;
constexpr std::int32_t kCut1CenterY = 2;
constexpr std::int32_t kCut1OrigCanvasW = 12;
constexpr std::int32_t kCut1OrigCanvasH = 4;

constexpr std::int32_t kCut1EditedMinX = 4;
constexpr std::int32_t kCut1EditedMaxX = 25;
constexpr std::int32_t kCut1EditedMinY = 8;
constexpr std::int32_t kCut1EditedMaxY = 14;
constexpr std::int32_t kCut1ExpectedConsolidatedW = 22;
constexpr std::int32_t kCut1ExpectedConsolidatedH = 7;

constexpr std::int32_t kCut2OriginX = -3;
constexpr std::int32_t kCut2OriginY = 9;
constexpr std::uint32_t kCut2EmptyOffsetSentinel = 28u;

constexpr std::uint16_t kTransFlagOpaque = 0u;
constexpr std::uint16_t kTransFlagTranslucent = 1u;

constexpr std::size_t kFirstReservedIdx = 0u;
constexpr std::size_t kLastCutReservedIdx = 79u;
constexpr std::size_t kMidCutReservedIdx = 40u;
constexpr std::size_t kLastPartReservedIdx = 81u;

constexpr std::uint8_t kCut0ResFirst = 0xA5u;
constexpr std::uint8_t kCut0ResLast = 0x5Au;
constexpr std::uint8_t kCut0PartResFirst = 0xC3u;
constexpr std::uint8_t kCut0PartResLast = 0x3Cu;

constexpr std::uint8_t kCut1ResFirst = 0x11u;
constexpr std::uint8_t kCut1ResLast = 0x88u;
constexpr std::uint8_t kCut1Part0ResFirst = 0x77u;
constexpr std::uint8_t kCut1Part0ResLast = 0xEEu;
constexpr std::uint8_t kCut1Part1ResFirst = 0x44u;

void AppendU16Le(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift8) & kByteMask));
}

void AppendU32Le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift8) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift16) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift24) & kByteMask));
}

void AppendI32Le(std::vector<std::uint8_t>& out, std::int32_t v) {
    AppendU32Le(out, static_cast<std::uint32_t>(v));
}

std::uint32_t ReadU32Le(const std::vector<std::uint8_t>& buf, std::size_t off) {
    return static_cast<std::uint32_t>(buf[off]) |
           (static_cast<std::uint32_t>(buf[off + 1u]) << kShift8) |
           (static_cast<std::uint32_t>(buf[off + 2u]) << kShift16) |
           (static_cast<std::uint32_t>(buf[off + 3u]) << kShift24);
}

void WriteU32LeAt(std::vector<std::uint8_t>& buf, std::size_t off, std::uint32_t v) {
    buf[off] = static_cast<std::uint8_t>(v & kByteMask);
    buf[off + 1u] = static_cast<std::uint8_t>((v >> kShift8) & kByteMask);
    buf[off + 2u] = static_cast<std::uint8_t>((v >> kShift16) & kByteMask);
    buf[off + 3u] = static_cast<std::uint8_t>((v >> kShift24) & kByteMask);
}

// Wraps `plain` as an uncompressed all-literal Siglus byte-LZSS frame so tests
// build valid wire inputs without mirroring the production LZSS matcher.
std::vector<std::uint8_t> WrapLiteralByteLzssFrame(
    const std::vector<std::uint8_t>& plain) {
    std::vector<std::uint8_t> body;
    for (std::size_t i = 0; i < plain.size(); i += static_cast<std::size_t>(kBitsPerFlagByte)) {
        const std::size_t rem = plain.size() - i;
        const std::size_t group =
            rem < static_cast<std::size_t>(kBitsPerFlagByte)
                ? rem
                : static_cast<std::size_t>(kBitsPerFlagByte);
        const std::uint8_t flag =
            group == static_cast<std::size_t>(kBitsPerFlagByte)
                ? kAllLiteralFlags
                : static_cast<std::uint8_t>((1u << group) - 1u);
        body.push_back(flag);
        for (std::size_t j = 0; j < group; ++j) {
            body.push_back(plain[i + j]);
        }
    }
    std::vector<std::uint8_t> framed;
    AppendU32Le(framed, static_cast<std::uint32_t>(kLzssFrameHeaderBytes + body.size()));
    AppendU32Le(framed, static_cast<std::uint32_t>(plain.size()));
    framed.insert(framed.end(), body.begin(), body.end());
    return framed;
}

std::vector<std::uint8_t> SerializeCutHeaderWire(const G00CutHeader& hdr) {
    std::vector<std::uint8_t> out;
    out.reserve(kG00CutHeaderBytes);
    AppendU16Le(out, hdr.cut_type);
    AppendU16Le(out, hdr.part_count);
    AppendI32Le(out, hdr.bbox_x);
    AppendI32Le(out, hdr.bbox_y);
    AppendI32Le(out, hdr.bbox_w);
    AppendI32Le(out, hdr.bbox_h);
    AppendI32Le(out, hdr.center_x);
    AppendI32Le(out, hdr.center_y);
    AppendI32Le(out, hdr.cut_canvas_w);
    AppendI32Le(out, hdr.cut_canvas_h);
    out.insert(out.end(), hdr.reserved.begin(), hdr.reserved.end());
    return out;
}

std::vector<std::uint8_t> SerializePartWire(
    const G00PartHeader& phdr, const std::vector<BgraColor>& pixels) {
    std::vector<std::uint8_t> out;
    out.reserve(kG00PartHeaderBytes + pixels.size() * 4u);
    AppendU16Le(out, phdr.part_x);
    AppendU16Le(out, phdr.part_y);
    AppendU16Le(out, phdr.trans_flag);
    AppendU16Le(out, phdr.part_w);
    AppendU16Le(out, phdr.part_h);
    out.insert(out.end(), phdr.reserved.begin(), phdr.reserved.end());
    for (const BgraColor& c : pixels) {
        out.push_back(c.b);
        out.push_back(c.g);
        out.push_back(c.r);
        out.push_back(c.a);
    }
    return out;
}

void AppendDescriptorWire(std::vector<std::uint8_t>& out, const G00CutDescriptor& d) {
    AppendI32Le(out, d.x1);
    AppendI32Le(out, d.y1);
    AppendI32Le(out, d.x2);
    AppendI32Le(out, d.y2);
    AppendI32Le(out, d.origin_x);
    AppendI32Le(out, d.origin_y);
}

std::vector<std::uint8_t> BuildMultiCutType2Fixture() {
    // Cut 0: 4x2 single-part translucent chip at canvas (2, 3)..(5, 4).
    G00CutHeader cut0_hdr{};
    cut0_hdr.cut_type = 1u;
    cut0_hdr.part_count = 1u;
    cut0_hdr.bbox_x = 0;
    cut0_hdr.bbox_y = 0;
    cut0_hdr.bbox_w = kCut0Width;
    cut0_hdr.bbox_h = kCut0Height;
    cut0_hdr.center_x = kCut0CenterX;
    cut0_hdr.center_y = kCut0CenterY;
    cut0_hdr.cut_canvas_w = kCut0Width;
    cut0_hdr.cut_canvas_h = kCut0Height;
    cut0_hdr.reserved[kFirstReservedIdx] = kCut0ResFirst;
    cut0_hdr.reserved[kLastCutReservedIdx] = kCut0ResLast;

    G00PartHeader cut0_part{};
    cut0_part.part_x = 0u;
    cut0_part.part_y = 0u;
    cut0_part.trans_flag = kTransFlagTranslucent;
    cut0_part.part_w = static_cast<std::uint16_t>(kCut0Width);
    cut0_part.part_h = static_cast<std::uint16_t>(kCut0Height);
    cut0_part.reserved[kFirstReservedIdx] = kCut0PartResFirst;
    cut0_part.reserved[kLastPartReservedIdx] = kCut0PartResLast;

    const std::vector<BgraColor> cut0_pixels = {
        {0x10, 0x20, 0x30, 0xFF}, {0x40, 0x50, 0x60, 0x80},
        {0x70, 0x80, 0x90, 0x00}, {0xFE, 0xDC, 0xBA, 0xFF},
        {0x01, 0x02, 0x03, 0x40}, {0x80, 0x7F, 0x00, 0xFF},
        {0x11, 0x22, 0x33, 0x99}, {0x44, 0x55, 0x66, 0xFF},
    };
    std::vector<std::uint8_t> cut0_blob = SerializeCutHeaderWire(cut0_hdr);
    const std::vector<std::uint8_t> cut0_p0 = SerializePartWire(cut0_part, cut0_pixels);
    cut0_blob.insert(cut0_blob.end(), cut0_p0.begin(), cut0_p0.end());

    // Cut 1: 12x4 cut at canvas (6, 8)..(17, 11) with 3 disjoint cut-local parts.
    G00CutHeader cut1_hdr{};
    cut1_hdr.cut_type = kCut1CutType;
    cut1_hdr.part_count = kCut1OrigPartCount;
    cut1_hdr.bbox_x = kCut1OrigBboxX;
    cut1_hdr.bbox_y = kCut1OrigBboxY;
    cut1_hdr.bbox_w = kCut1OrigBboxW;
    cut1_hdr.bbox_h = kCut1OrigBboxH;
    cut1_hdr.center_x = kCut1CenterX;
    cut1_hdr.center_y = kCut1CenterY;
    cut1_hdr.cut_canvas_w = kCut1OrigCanvasW;
    cut1_hdr.cut_canvas_h = kCut1OrigCanvasH;
    cut1_hdr.reserved[kFirstReservedIdx] = kCut1ResFirst;
    cut1_hdr.reserved[kLastCutReservedIdx] = kCut1ResLast;

    G00PartHeader p0{};
    p0.part_x = 0u;
    p0.part_y = 0u;
    p0.trans_flag = kTransFlagOpaque;
    p0.part_w = 2u;
    p0.part_h = 2u;
    p0.reserved[kFirstReservedIdx] = kCut1Part0ResFirst;
    p0.reserved[kLastPartReservedIdx] = kCut1Part0ResLast;
    const std::vector<BgraColor> p0_pix(4u, BgraColor{0x12, 0x34, 0x56, 0xFF});

    G00PartHeader p1{};
    p1.part_x = 5u;
    p1.part_y = 1u;
    p1.trans_flag = kTransFlagTranslucent;
    p1.part_w = 2u;
    p1.part_h = 2u;
    p1.reserved[kFirstReservedIdx] = kCut1Part1ResFirst;
    const std::vector<BgraColor> p1_pix(4u, BgraColor{0x80, 0x90, 0xA0, 0x80});

    G00PartHeader p2{};
    p2.part_x = 9u;
    p2.part_y = 2u;
    p2.trans_flag = kTransFlagOpaque;
    p2.part_w = 2u;
    p2.part_h = 2u;
    const std::vector<BgraColor> p2_pix(4u, BgraColor{0xFE, 0x80, 0x01, 0xFF});

    std::vector<std::uint8_t> cut1_blob = SerializeCutHeaderWire(cut1_hdr);
    for (const auto& chunk : {SerializePartWire(p0, p0_pix),
                              SerializePartWire(p1, p1_pix),
                              SerializePartWire(p2, p2_pix)}) {
        cut1_blob.insert(cut1_blob.end(), chunk.begin(), chunk.end());
    }

    // Decompressed Type 2 stream: [u32 index_count = 3] + 3 * {offset, size} + blobs.
    constexpr std::uint32_t kTableBytes = 4u + kType2CutCount * 8u;
    const std::uint32_t cut0_off = kTableBytes;
    const std::uint32_t cut0_sz = static_cast<std::uint32_t>(cut0_blob.size());
    const std::uint32_t cut1_off = cut0_off + cut0_sz;
    const std::uint32_t cut1_sz = static_cast<std::uint32_t>(cut1_blob.size());

    std::vector<std::uint8_t> decompressed;
    AppendU32Le(decompressed, kType2CutCount);
    AppendU32Le(decompressed, cut0_off);
    AppendU32Le(decompressed, cut0_sz);
    AppendU32Le(decompressed, cut1_off);
    AppendU32Le(decompressed, cut1_sz);
    AppendU32Le(decompressed, kCut2EmptyOffsetSentinel);
    AppendU32Le(decompressed, 0u);
    decompressed.insert(decompressed.end(), cut0_blob.begin(), cut0_blob.end());
    decompressed.insert(decompressed.end(), cut1_blob.begin(), cut1_blob.end());

    const std::vector<std::uint8_t> framed = WrapLiteralByteLzssFrame(decompressed);

    std::vector<std::uint8_t> g00;
    g00.push_back(static_cast<std::uint8_t>(G00Type::kType2MultiCutBgra32));
    AppendU16Le(g00, kType2CanvasWidth);
    AppendU16Le(g00, kType2CanvasHeight);
    AppendU32Le(g00, kType2CutCount);
    AppendDescriptorWire(
        g00, G00CutDescriptor{kCut0X1, kCut0Y1, kCut0X2, kCut0Y2, kCut0OriginX, kCut0OriginY});
    AppendDescriptorWire(
        g00, G00CutDescriptor{kCut1OrigX1, kCut1OrigY1, kCut1OrigX2, kCut1OrigY2,
                              kCut1OriginX, kCut1OriginY});
    AppendDescriptorWire(
        g00, G00CutDescriptor{0, 0, 0, 0, kCut2OriginX, kCut2OriginY});
    g00.insert(g00.end(), framed.begin(), framed.end());
    return g00;
}

}  // namespace

TEST(G00Codec, ByteLzssPinnedWireVectorsAndWindowBoundaries) {
    // Sign-bit seam literals (0x00, 0x7F, 0x80, 0xFF) followed by back-ref
    // offset = 2, count = 2 (`token = (2 << 4) | 0 = 0x0020`), flag = 0x0F.
    const std::vector<std::uint8_t> wire_sign_seam = {
        0x0F, 0x00, 0x00, 0x00,  // arc_size = 15
        0x06, 0x00, 0x00, 0x00,  // unp_size = 6
        0x0F,                    // bits 0..3 literals, bit 4 back-ref
        0x00, 0x7F, 0x80, 0xFF,
        0x20, 0x00,
    };
    const std::vector<std::uint8_t> expected_sign_seam = {
        0x00, 0x7F, 0x80, 0xFF, 0x80, 0xFF,
    };
    EXPECT_EQ(DecompressByteLzss(wire_sign_seam), expected_sign_seam);

    // Overlapping run `offset = 1`, `count = 17` (`kByteLzssMaxMatch`):
    // `token = (1 << 4) | (17 - 2) = 0x001F`, flag = 0x01 -> 18 bytes of 0xAA.
    const std::vector<std::uint8_t> wire_overlap = {
        0x0C, 0x00, 0x00, 0x00,  // arc_size = 12
        0x12, 0x00, 0x00, 0x00,  // unp_size = 18
        0x01, kOverlapFillByte, 0x1F, 0x00,
    };
    EXPECT_EQ(DecompressByteLzss(wire_overlap),
              std::vector<std::uint8_t>(kOverlapRleCount, kOverlapFillByte));

    // Empty payload round-trips cleanly (`arc_size = 8`, `unp_size = 0`).
    const std::vector<std::uint8_t> empty_arc = CompressByteLzss({});
    ASSERT_EQ(empty_arc.size(), kLzssFrameHeaderBytes);
    EXPECT_EQ(ReadU32Le(empty_arc, 0u), static_cast<std::uint32_t>(kLzssFrameHeaderBytes));
    EXPECT_EQ(ReadU32Le(empty_arc, 4u), 0u);
    EXPECT_TRUE(DecompressByteLzss(empty_arc).empty());

    // Sliding-window wrap test (`> 4095` bytes): pattern at [0..15] repeated at
    // [4104..4119] (distance 4104 > 4095, out of window) and [30..45] repeated at
    // [4120..4135] (distance 4090 <= 4095, inside window).
    std::vector<std::uint8_t> long_plain(kByteWindowTotalLen, 0u);
    for (std::size_t i = 0; i < long_plain.size(); ++i) {
        long_plain[i] = static_cast<std::uint8_t>((i * 73u + 19u) & kByteMask);
    }
    for (std::size_t i = 0; i < kPatternPrefixCount; ++i) {
        long_plain[i] = static_cast<std::uint8_t>(0xD0u + i);
        long_plain[kByteBeyondWindowOffset + i] = long_plain[i];
        long_plain[kByteWithinWindowDstOffset + i] = long_plain[kByteWithinWindowSrcOffset + i];
    }
    const std::vector<std::uint8_t> long_arc = CompressByteLzss(long_plain);
    ASSERT_GE(long_arc.size(), kLzssFrameHeaderBytes);
    EXPECT_EQ(ReadU32Le(long_arc, 0u), static_cast<std::uint32_t>(long_arc.size()));
    EXPECT_EQ(ReadU32Le(long_arc, 4u), static_cast<std::uint32_t>(long_plain.size()));
    EXPECT_EQ(DecompressByteLzss(long_arc), long_plain);
}

TEST(G00Codec, Type0HandCraftedWireDecodeAndWindowWrapRoundTrip) {
    // Hand-crafted 3x2 Type 0 .g00 (`unp_size = 3 * 2 * 4 = 24` in 4-byte BGRA stride):
    //   bit 0 = 1: literal BGR (0x00, 0x80, 0xFF) -> pixel (0,0)
    //   bit 1 = 0: overlapping back-ref offset=1, count=2 (0x0011) -> pixels (1,0), (2,0)
    //   bit 2 = 1: literal BGR (0x7F, 0x10, 0xFE) -> pixel (0,1)
    //   bit 3 = 0: back-ref offset=2, count=2 (0x0021) -> pixels (1,1)=pix2, (2,1)=pix3
    //   flag = 0b00000101 = 0x05; arc_size = 8 + 11 = 19, unp_size = 24.
    const std::vector<std::uint8_t> wire_type0 = {
        0x00,                    // type = 0
        0x03, 0x00,              // width = 3 (u16 LE)
        0x02, 0x00,              // height = 2 (u16 LE)
        0x13, 0x00, 0x00, 0x00,  // arc_size = 19
        0x18, 0x00, 0x00, 0x00,  // unp_size = 24 (width * height * 4)
        0x05,
        0x00, 0x80, 0xFF,
        0x11, 0x00,
        0x7F, 0x10, 0xFE,
        0x21, 0x00,
    };

    G00Image decoded = G00Codec::Decode(wire_type0);
    EXPECT_EQ(decoded.type, G00Type::kType0Bgr24);
    EXPECT_EQ(decoded.width, kType0Width);
    EXPECT_EQ(decoded.height, kType0Height);
    ASSERT_EQ(decoded.canvas.width(), static_cast<int>(kType0Width));
    ASSERT_EQ(decoded.canvas.height(), static_cast<int>(kType0Height));

    const BgraColor kPixA{0x00, 0x80, 0xFF, 0xFF};
    const BgraColor kPixB{0x7F, 0x10, 0xFE, 0xFF};
    EXPECT_EQ(decoded.canvas.PixelAt(0, 0), kPixA);
    EXPECT_EQ(decoded.canvas.PixelAt(1, 0), kPixA);
    EXPECT_EQ(decoded.canvas.PixelAt(2, 0), kPixA);
    EXPECT_EQ(decoded.canvas.PixelAt(0, 1), kPixB);
    EXPECT_EQ(decoded.canvas.PixelAt(1, 1), kPixA);
    EXPECT_EQ(decoded.canvas.PixelAt(2, 1), kPixB);

    const BgraColor kEditedPix{0x99, 0x44, 0x22, 0xFF};
    decoded.MutableCanvas().SetPixel(1, 0, kEditedPix);
    EXPECT_TRUE(decoded.canvas_modified);

    const std::vector<std::uint8_t> reencoded = G00Codec::Encode(decoded);
    ASSERT_GE(reencoded.size(), kG00FileHeaderBytes + kLzssFrameHeaderBytes);
    EXPECT_EQ(reencoded[0], static_cast<std::uint8_t>(G00Type::kType0Bgr24));
    EXPECT_EQ(ReadU32Le(reencoded, kG00FileHeaderBytes + 4u), kType0UnpBytes);
    EXPECT_EQ(ReadU32Le(reencoded, kG00FileHeaderBytes),
              static_cast<std::uint32_t>(reencoded.size() - kG00FileHeaderBytes));

    std::string parity_err;
    EXPECT_TRUE(G00Codec::VerifyMetadataParity(wire_type0, reencoded, {}, &parity_err))
        << parity_err;
    const G00Image roundtrip = G00Codec::Decode(reencoded);
    EXPECT_EQ(roundtrip.canvas, decoded.canvas);
    EXPECT_EQ(wire_type0.size(), kG00FileHeaderBytes + kType0ArcBytes);

    // 65x65 (4225 pixels > 4095 window) PixelLzss24 window wrap round-trip.
    G00Image large_img;
    large_img.type = G00Type::kType0Bgr24;
    large_img.width = kWindowTestWidth;
    large_img.height = kWindowTestHeight;
    large_img.canvas = BgraImage(kWindowTestWidth, kWindowTestHeight);
    for (std::size_t p = 0; p < kWindowTestPixels; ++p) {
        const int x = static_cast<int>(p % kWindowTestWidth);
        const int y = static_cast<int>(p / kWindowTestWidth);
        large_img.canvas.SetPixel(
            x, y,
            BgraColor{static_cast<std::uint8_t>((p * 37u) & kByteMask),
                      static_cast<std::uint8_t>((p * 91u + 7u) & kByteMask),
                      static_cast<std::uint8_t>((p * 13u + 0x80u) & kByteMask),
                      0xFFu});
    }
    for (std::size_t k = 0; k < kPatternPrefixCount; ++k) {
        const BgraColor c0 = large_img.canvas.PixelAt(static_cast<int>(k), 0);
        const std::size_t beyond = kBeyondWindowPixelIdx + k;
        large_img.canvas.SetPixel(static_cast<int>(beyond % kWindowTestWidth),
                                  static_cast<int>(beyond / kWindowTestWidth), c0);

        const std::size_t src = kWithinWindowSrcIdx + k;
        const BgraColor cs = large_img.canvas.PixelAt(
            static_cast<int>(src % kWindowTestWidth),
            static_cast<int>(src / kWindowTestWidth));
        const std::size_t dst = kWithinWindowDstIdx + k;
        large_img.canvas.SetPixel(static_cast<int>(dst % kWindowTestWidth),
                                  static_cast<int>(dst / kWindowTestWidth), cs);
    }
    const std::vector<std::uint8_t> large_encoded = G00Codec::Encode(large_img);
    const G00Image large_decoded = G00Codec::Decode(large_encoded);
    EXPECT_EQ(large_decoded.canvas, large_img.canvas);
}

TEST(G00Codec, Type1PalettedUntouchedPreservationAndModifiedReencode) {
    // 2x2 Type 1 paletted sprite with 3 palette entries (slot 1 intentionally
    // unreferenced so untouched re-encoding proves verbatim palette retention).
    const BgraColor kPal0{0x10, 0x20, 0x80, 0xFF};
    const BgraColor kPal1Unused{0xDE, 0xAD, 0xBE, 0xEF};
    const BgraColor kPal2Semi{0xFE, 0x7F, 0x01, 0x80};

    std::vector<std::uint8_t> plain_type1;
    AppendU16Le(plain_type1, kType1PaletteCount);
    for (const BgraColor& c : {kPal0, kPal1Unused, kPal2Semi}) {
        plain_type1.push_back(c.b);
        plain_type1.push_back(c.g);
        plain_type1.push_back(c.r);
        plain_type1.push_back(c.a);
    }
    const std::vector<std::uint8_t> expected_indices = {0u, 2u, 2u, 0u};
    plain_type1.insert(plain_type1.end(), expected_indices.begin(), expected_indices.end());

    const std::vector<std::uint8_t> framed_type1 = WrapLiteralByteLzssFrame(plain_type1);
    std::vector<std::uint8_t> wire_type1;
    wire_type1.push_back(static_cast<std::uint8_t>(G00Type::kType1Paletted8));
    AppendU16Le(wire_type1, kType1Width);
    AppendU16Le(wire_type1, kType1Height);
    wire_type1.insert(wire_type1.end(), framed_type1.begin(), framed_type1.end());

    G00Image img = G00Codec::Decode(wire_type1);
    EXPECT_EQ(img.type, G00Type::kType1Paletted8);
    EXPECT_FALSE(img.canvas_modified);
    ASSERT_EQ(img.type1_palette.size(), static_cast<std::size_t>(kType1PaletteCount));
    EXPECT_EQ(img.type1_palette[0], kPal0);
    EXPECT_EQ(img.type1_palette[1], kPal1Unused);
    EXPECT_EQ(img.type1_palette[2], kPal2Semi);
    EXPECT_EQ(img.type1_indices, expected_indices);
    EXPECT_EQ(img.canvas.PixelAt(0, 0), kPal0);
    EXPECT_EQ(img.canvas.PixelAt(1, 0), kPal2Semi);
    EXPECT_EQ(img.canvas.PixelAt(0, 1), kPal2Semi);
    EXPECT_EQ(img.canvas.PixelAt(1, 1), kPal0);

    // Untouched re-encode must preserve the exact decompressed palette + index bytes.
    const std::vector<std::uint8_t> untouched_encoded = G00Codec::Encode(img);
    ASSERT_GT(untouched_encoded.size(), kG00FileHeaderBytes);
    const std::vector<std::uint8_t> untouched_plain = DecompressByteLzss(
        boost::span<const std::uint8_t>(untouched_encoded.data() + kG00FileHeaderBytes,
                                        untouched_encoded.size() - kG00FileHeaderBytes));
    EXPECT_EQ(untouched_plain, plain_type1);

    // Modified canvas re-encodes new colors cleanly and round-trips pixel-exact.
    const BgraColor kNewColor{0x33, 0x66, 0x99, 0xFF};
    img.MutableCanvas().SetPixel(1, 1, kNewColor);
    const std::vector<std::uint8_t> modified_encoded = G00Codec::Encode(img);
    const G00Image modified_decoded = G00Codec::Decode(modified_encoded);
    EXPECT_EQ(modified_decoded.canvas, img.canvas);
    G00Codec::AssertMetadataParity(wire_type1, modified_encoded);
}

TEST(G00Codec, Type2MultiCutUntouchedPreservationAndDisjointChipConsolidation) {
    const std::vector<std::uint8_t> orig_wire = BuildMultiCutType2Fixture();
    G00Image img = G00Codec::Decode(orig_wire);

    EXPECT_EQ(img.type, G00Type::kType2MultiCutBgra32);
    EXPECT_EQ(img.width, kType2CanvasWidth);
    EXPECT_EQ(img.height, kType2CanvasHeight);
    ASSERT_EQ(img.cuts.size(), static_cast<std::size_t>(kType2CutCount));

    // Verify Cut 0 decoded metadata & cut-local pixel placement at (2, 3).
    EXPECT_FALSE(img.cuts[0].is_empty);
    EXPECT_EQ(img.cuts[0].descriptor.origin_x, kCut0OriginX);
    EXPECT_EQ(img.cuts[0].descriptor.origin_y, kCut0OriginY);
    EXPECT_EQ(img.cuts[0].canvas.PixelAt(kCut0X1, kCut0Y1),
              (BgraColor{0x10, 0x20, 0x30, 0xFF}));

    // Verify Cut 1 decoded all 3 disjoint cut-local chips relative to (x1=6, y1=8).
    EXPECT_FALSE(img.cuts[1].is_empty);
    ASSERT_EQ(img.cuts[1].part_headers.size(),
              static_cast<std::size_t>(kCut1OrigPartCount));
    EXPECT_EQ(img.cuts[1].canvas.PixelAt(kCut1OrigX1 + 0, kCut1OrigY1 + 0),
              (BgraColor{0x12, 0x34, 0x56, 0xFF}));
    EXPECT_EQ(img.cuts[1].canvas.PixelAt(kCut1OrigX1 + 5, kCut1OrigY1 + 1),
              (BgraColor{0x80, 0x90, 0xA0, 0x80}));
    EXPECT_EQ(img.cuts[1].canvas.PixelAt(kCut1OrigX1 + 9, kCut1OrigY1 + 2),
              (BgraColor{0xFE, 0x80, 0x01, 0xFF}));

    // Verify Cut 2 empty slot.
    EXPECT_TRUE(img.cuts[2].is_empty);
    EXPECT_EQ(img.cuts[2].empty_index_offset, kCut2EmptyOffsetSentinel);
    EXPECT_EQ(img.cuts[2].descriptor.origin_x, kCut2OriginX);
    EXPECT_EQ(img.cuts[2].descriptor.origin_y, kCut2OriginY);

    const auto orig_cut0_desc_bytes = img.cuts[0].raw_descriptor_bytes;
    const auto orig_cut0_raw_bytes = img.cuts[0].raw_cut_bytes;
    const auto orig_cut2_desc_bytes = img.cuts[2].raw_descriptor_bytes;
    const auto orig_cut1_reserved = img.cuts[1].cut_header.reserved;
    const auto orig_cut1_part0_reserved = img.cuts[1].part_headers[0].reserved;

    // Prove untouched cuts emit `raw_descriptor_bytes` and `raw_cut_bytes`
    // verbatim without re-serializing `descriptor` or overwriting the 116-byte
    // `cut_header` slice when `modified == false`.
    {
        G00Image untouched_probe = img;
        ASSERT_FALSE(untouched_probe.cuts[0].modified);
        ASSERT_FALSE(untouched_probe.cuts[2].modified);
        untouched_probe.cuts[0].descriptor.origin_x = 9999;
        untouched_probe.cuts[0].cut_header.center_x = 9999;
        untouched_probe.cuts[2].descriptor.origin_y = -9999;
        const G00Image probe_redecoded = G00Codec::Decode(G00Codec::Encode(untouched_probe));
        EXPECT_EQ(probe_redecoded.cuts[0].raw_descriptor_bytes, orig_cut0_desc_bytes);
        EXPECT_EQ(probe_redecoded.cuts[0].raw_cut_bytes, orig_cut0_raw_bytes);
        EXPECT_EQ(probe_redecoded.cuts[0].descriptor.origin_x, kCut0OriginX);
        EXPECT_EQ(probe_redecoded.cuts[0].cut_header.center_x, kCut0CenterX);
        EXPECT_EQ(probe_redecoded.cuts[2].raw_descriptor_bytes, orig_cut2_desc_bytes);
        EXPECT_EQ(probe_redecoded.cuts[2].descriptor.origin_y, kCut2OriginY);
    }

    // Modify ONLY Cut 1 with wider English ink spanning [4..25] x [8..14] (22x7)
    // while retaining transparent background gaps inside the consolidated slab.
    BgraImage& cut1_canvas = img.MutableCutCanvas(1u);
    cut1_canvas.SetPixel(kCut1EditedMinX, kCut1EditedMinY, BgraColor{0xF0, 0xF0, 0xF0, 0xFF});
    cut1_canvas.SetPixel(kCut1EditedMaxX, kCut1EditedMaxY, BgraColor{0xE0, 0xE0, 0xE0, 0xC0});
    const BgraImage expected_cut1_canvas = cut1_canvas;

    const std::vector<std::uint8_t> patched_wire = G00Codec::Encode(img);
    std::string parity_err;
    EXPECT_TRUE(G00Codec::VerifyMetadataParity(orig_wire, patched_wire, {1u}, &parity_err))
        << parity_err;
    G00Codec::AssertMetadataParity(orig_wire, patched_wire, {1u});

    const G00Image redecoded = G00Codec::Decode(patched_wire);
    ASSERT_EQ(redecoded.cuts.size(), static_cast<std::size_t>(kType2CutCount));

    // Cut 0 untouched: raw 24B descriptor and raw cut block must be byte-identical.
    EXPECT_EQ(redecoded.cuts[0].raw_descriptor_bytes, orig_cut0_desc_bytes);
    EXPECT_EQ(redecoded.cuts[0].raw_cut_bytes, orig_cut0_raw_bytes);
    EXPECT_EQ(redecoded.cuts[0].canvas, img.cuts[0].canvas);

    // Cut 1 consolidated: inclusive descriptor [4, 8, 25, 14], cut-local 116B & 92B
    // headers at (0, 0) with dimensions 22x7, alpha-derived trans_flag = 1, and
    // preserved origins, centers, cut_type, and reserved arrays.
    const G00Cut& c1 = redecoded.cuts[1];
    EXPECT_FALSE(c1.is_empty);
    EXPECT_EQ(c1.descriptor.x1, kCut1EditedMinX);
    EXPECT_EQ(c1.descriptor.y1, kCut1EditedMinY);
    EXPECT_EQ(c1.descriptor.x2, kCut1EditedMaxX);
    EXPECT_EQ(c1.descriptor.y2, kCut1EditedMaxY);
    EXPECT_EQ(c1.descriptor.origin_x, kCut1OriginX);
    EXPECT_EQ(c1.descriptor.origin_y, kCut1OriginY);

    EXPECT_EQ(c1.cut_header.cut_type, kCut1CutType);
    EXPECT_EQ(c1.cut_header.part_count, 1u);
    EXPECT_EQ(c1.cut_header.bbox_x, 0);
    EXPECT_EQ(c1.cut_header.bbox_y, 0);
    EXPECT_EQ(c1.cut_header.bbox_w, kCut1ExpectedConsolidatedW);
    EXPECT_EQ(c1.cut_header.bbox_h, kCut1ExpectedConsolidatedH);
    EXPECT_EQ(c1.cut_header.center_x, kCut1CenterX);
    EXPECT_EQ(c1.cut_header.center_y, kCut1CenterY);
    EXPECT_EQ(c1.cut_header.cut_canvas_w, kCut1ExpectedConsolidatedW);
    EXPECT_EQ(c1.cut_header.cut_canvas_h, kCut1ExpectedConsolidatedH);
    EXPECT_EQ(c1.cut_header.reserved, orig_cut1_reserved);

    ASSERT_EQ(c1.part_headers.size(), 1u);
    EXPECT_EQ(c1.part_headers[0].part_x, 0u);
    EXPECT_EQ(c1.part_headers[0].part_y, 0u);
    EXPECT_EQ(c1.part_headers[0].trans_flag, kTransFlagTranslucent);
    EXPECT_EQ(c1.part_headers[0].part_w,
              static_cast<std::uint16_t>(kCut1ExpectedConsolidatedW));
    EXPECT_EQ(c1.part_headers[0].part_h,
              static_cast<std::uint16_t>(kCut1ExpectedConsolidatedH));
    EXPECT_EQ(c1.part_headers[0].reserved, orig_cut1_part0_reserved);
    EXPECT_EQ(c1.canvas, expected_cut1_canvas);

    // Cut 2 remains empty with preserved origin and empty_index_offset.
    EXPECT_TRUE(redecoded.cuts[2].is_empty);
    EXPECT_EQ(redecoded.cuts[2].empty_index_offset, kCut2EmptyOffsetSentinel);
    EXPECT_EQ(redecoded.cuts[2].descriptor.origin_x, kCut2OriginX);
    EXPECT_EQ(redecoded.cuts[2].descriptor.origin_y, kCut2OriginY);

    // Also verify trans_flag = 0 when a modified cut's union slab is 100% opaque.
    G00Image opaque_edit = G00Codec::Decode(orig_wire);
    opaque_edit.MutableCutCanvas(0u).FillRect(
        RectI{kCut0X1, kCut0Y1, kCut0Width, kCut0Height},
        BgraColor{0x20, 0x40, 0x60, 0xFF});
    const G00Image opaque_decoded = G00Codec::Decode(G00Codec::Encode(opaque_edit));
    ASSERT_EQ(opaque_decoded.cuts[0].part_headers.size(), 1u);
    EXPECT_EQ(opaque_decoded.cuts[0].part_headers[0].trans_flag, kTransFlagOpaque);
}

TEST(G00Codec, VerifyMetadataParityRejectsOriginCenterReservedAndUntouchedDrift) {
    const std::vector<std::uint8_t> orig_wire = BuildMultiCutType2Fixture();
    G00Image edited = G00Codec::Decode(orig_wire);
    edited.MutableCutCanvas(1u).SetPixel(
        kCut1OrigX1, kCut1OrigY1, BgraColor{0xAA, 0xBB, 0xCC, 0xFF});
    const std::vector<std::uint8_t> valid_patched = G00Codec::Encode(edited);

    // Untouched cut drift: Cut 1 changed, but modified_cuts is empty.
    std::string err;
    EXPECT_FALSE(G00Codec::VerifyMetadataParity(orig_wire, valid_patched, {}, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_THROW(G00Codec::AssertMetadataParity(orig_wire, valid_patched, {}),
                 std::runtime_error);

    // Mutated CutDescriptor.origin_x on Cut 1.
    G00Image bad_origin = edited;
    bad_origin.cuts[1].descriptor.origin_x += 1;
    EXPECT_FALSE(
        G00Codec::VerifyMetadataParity(orig_wire, G00Codec::Encode(bad_origin), {1u}, &err));

    // Mutated CutHeader.center_y on Cut 1.
    G00Image bad_center = edited;
    bad_center.cuts[1].cut_header.center_y -= 1;
    EXPECT_FALSE(
        G00Codec::VerifyMetadataParity(orig_wire, G00Codec::Encode(bad_center), {1u}, &err));

    // Mutated CutHeader.reserved byte on Cut 1.
    G00Image bad_reserved = edited;
    bad_reserved.cuts[1].cut_header.reserved[kMidCutReservedIdx] ^= kByteMask;
    EXPECT_FALSE(
        G00Codec::VerifyMetadataParity(orig_wire, G00Codec::Encode(bad_reserved), {1u}, &err));

    // Mutated canvas width.
    G00Image bad_dim = G00Codec::Decode(orig_wire);
    bad_dim.width = static_cast<std::uint16_t>(bad_dim.width + 1u);
    for (G00Cut& c : bad_dim.cuts) {
        c.canvas = BgraImage(bad_dim.width, bad_dim.height);
    }
    EXPECT_FALSE(
        G00Codec::VerifyMetadataParity(orig_wire, G00Codec::Encode(bad_dim), {}, &err));
}

TEST(G00Codec, RejectsTruncatedAndCorruptWireInputs) {
    struct CorruptCase {
        const char* name;
        std::vector<std::uint8_t> bytes;
    };

    // Type 2 container with mismatched `index_count != cut_count`.
    std::vector<std::uint8_t> mismatched_index_plain;
    AppendU32Le(mismatched_index_plain, 2u);  // index_count = 2 != cut_count = 1
    AppendU32Le(mismatched_index_plain, 0u);
    AppendU32Le(mismatched_index_plain, 0u);
    AppendU32Le(mismatched_index_plain, 0u);
    AppendU32Le(mismatched_index_plain, 0u);
    const std::vector<std::uint8_t> mismatched_framed =
        WrapLiteralByteLzssFrame(mismatched_index_plain);
    std::vector<std::uint8_t> bad_index_g00 = {0x02, 0x04, 0x00, 0x04, 0x00,
                                               0x01, 0x00, 0x00, 0x00};
    AppendDescriptorWire(bad_index_g00, G00CutDescriptor{});
    bad_index_g00.insert(bad_index_g00.end(), mismatched_framed.begin(),
                         mismatched_framed.end());

    // Type 2 container with `cut_offset + cut_size` 32-bit overflow wraparound.
    std::vector<std::uint8_t> overflow_cut_plain;
    AppendU32Le(overflow_cut_plain, 1u);
    AppendU32Le(overflow_cut_plain, 0xFFFFFFF0u);
    AppendU32Le(overflow_cut_plain, 0x00000080u);
    const std::vector<std::uint8_t> overflow_framed =
        WrapLiteralByteLzssFrame(overflow_cut_plain);
    std::vector<std::uint8_t> overflow_cut_g00 = {0x02, 0x04, 0x00, 0x04, 0x00,
                                                  0x01, 0x00, 0x00, 0x00};
    AppendDescriptorWire(overflow_cut_g00, G00CutDescriptor{});
    overflow_cut_g00.insert(overflow_cut_g00.end(), overflow_framed.begin(),
                            overflow_framed.end());

    // Type 1 with out-of-range high-bit index `0x80` (`palette_count = 2`).
    std::vector<std::uint8_t> bad_pal_plain = {
        0x02, 0x00,
        0x00, 0x00, 0x00, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,
        0x80,  // index 128 >= palette_count 2 (catches signed char < 0 bug!)
    };
    const std::vector<std::uint8_t> bad_pal_framed = WrapLiteralByteLzssFrame(bad_pal_plain);
    std::vector<std::uint8_t> bad_pal_g00 = {0x01, 0x01, 0x00, 0x01, 0x00};
    bad_pal_g00.insert(bad_pal_g00.end(), bad_pal_framed.begin(), bad_pal_framed.end());

    const std::vector<CorruptCase> cases = {
        {"empty_buffer", {}},
        {"short_4byte_header", {0x00, 0x01, 0x00, 0x01}},
        {"unknown_type_3", {0x03, 0x01, 0x00, 0x01, 0x00, 0x08, 0x00, 0x00, 0x00, 0, 0, 0, 0}},
        {"unknown_type_0xFF", {0xFF, 0x01, 0x00, 0x01, 0x00, 0x08, 0x00, 0x00, 0x00, 0, 0, 0, 0}},
        {"zero_width", {0x00, 0x00, 0x00, 0x02, 0x00, 0x08, 0x00, 0x00, 0x00, 0, 0, 0, 0}},
        {"type0_wrong_3bpp_unp_size",
         {0x00, 0x01, 0x00, 0x01, 0x00,
          0x0C, 0x00, 0x00, 0x00,
          0x03, 0x00, 0x00, 0x00,  // 3 instead of 1 * 1 * 4 = 4
          0x01, 0x10, 0x20, 0x30}},
        {"type0_truncated_literal",
         {0x00, 0x01, 0x00, 0x01, 0x00,
          0x0B, 0x00, 0x00, 0x00,
          0x04, 0x00, 0x00, 0x00,
          0x01, 0x10, 0x20}},
        {"type0_zero_offset_backref",
         {0x00, 0x01, 0x00, 0x01, 0x00,
          0x0B, 0x00, 0x00, 0x00,
          0x04, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00}},
        {"type1_high_bit_index_0x80_out_of_bounds", bad_pal_g00},
        {"type2_mismatched_index_count", bad_index_g00},
        {"type2_cut_offset_overflow", overflow_cut_g00},
    };

    const std::vector<std::uint8_t> valid_fixture = BuildMultiCutType2Fixture();
    EXPECT_EQ(G00Codec::Decode(valid_fixture).cuts.size(),
              static_cast<std::size_t>(kType2CutCount));

    ASSERT_EQ(cases.size(), 11u);
    for (const CorruptCase& tc : cases) {
        SCOPED_TRACE(tc.name);
        EXPECT_THROW(G00Codec::Decode(tc.bytes), std::runtime_error);
        std::string err;
        EXPECT_FALSE(G00Codec::VerifyMetadataParity(valid_fixture, tc.bytes, {}, &err));
        EXPECT_FALSE(err.empty());
    }

    // Valid -> error -> valid check: codec remains clean after rejecting malformed inputs.
    std::vector<std::uint8_t> truncated = valid_fixture;
    WriteU32LeAt(truncated, 5u, 0xFFFFFFFFu);
    EXPECT_THROW(G00Codec::Decode(truncated), std::runtime_error);
    EXPECT_EQ(G00Codec::Decode(valid_fixture).cuts.size(),
              static_cast<std::size_t>(kType2CutCount));
}

}  // namespace adi::g00
