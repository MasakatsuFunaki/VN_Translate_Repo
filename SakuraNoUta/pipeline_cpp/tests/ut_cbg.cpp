// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CompressedBG decoder unit tests against a hand-crafted 2x2 24bpp blob with
// hand-computed differential-decode expectations, plus BMP writer shape.
#include "bgi/bgi_cbg.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

using snu::Bytes;
namespace bgi = snu::bgi;

namespace {

constexpr std::uint32_t kHashWordMask = 0xFFFFu;
constexpr std::uint32_t kHashMulLo = 20021u;
constexpr std::uint32_t kHashMulHi = 346u;
constexpr int kWordShift = 16;
constexpr std::uint32_t kHashReturnMask = 0x7FFFu;
constexpr int kBitsPerByte = 8;
constexpr std::size_t kCbgHeaderBytes = 0x30;
constexpr std::size_t kCbgMagicBytes = 16;
constexpr int kDwordBytes = 4;
constexpr std::uint32_t kByteMask = 0xFFu;
constexpr std::uint32_t kDefaultCbgKey = 0xCAFEu;
constexpr int kCbgLeafCount = 256;

// Mirror of the decoder's PRNG (asmodean hash, same as DSC).
std::uint32_t cbg_hash_update(std::uint32_t& state) {
    std::uint32_t eax = (state & kHashWordMask) * kHashMulLo;
    std::uint32_t edx =
        ((state >> kWordShift) & kHashWordMask) * kHashMulLo + state * kHashMulHi;
    edx = edx + (eax >> kWordShift);
    state = (((edx & kHashWordMask) << kWordShift) | (eax & kHashWordMask)) + 1u;
    return edx & kHashReturnMask;
}

class BitWriter {
public:
    void put_bits(std::uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            cur_ = static_cast<std::uint8_t>((cur_ << 1) | ((value >> i) & 1u));
            if (++count_ == kBitsPerByte) {
                out_.push_back(cur_);
                cur_ = 0;
                count_ = 0;
            }
        }
    }
    Bytes finish() {
        if (count_ > 0) {
            out_.push_back(static_cast<std::uint8_t>(cur_ << (kBitsPerByte - count_)));
        }
        return out_;
    }

private:
    Bytes out_;
    std::uint8_t cur_ = 0;
    int count_ = 0;
};

// All 256 leaf weights = 1 gives a perfect merge-two-smallest tree where
// symbol b's Huffman code is b in 8-bit MSB-first binary.
Bytes make_cbg_2x2(const Bytes& rle_stream, std::uint32_t key = kDefaultCbgKey) {
    Bytes blob(kCbgHeaderBytes, 0);
    const char magic[] = "CompressedBG___";
    std::memcpy(blob.data(), magic, kCbgMagicBytes);
    auto put16 = [&](std::size_t off, std::uint16_t v) {
        blob[off] = static_cast<std::uint8_t>(v & kByteMask);
        blob[off + 1] = static_cast<std::uint8_t>(v >> kBitsPerByte);
    };
    auto put32 = [&](std::size_t off, std::uint32_t v) {
        for (int i = 0; i < kDwordBytes; ++i) {
            blob[off + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((v >> (kBitsPerByte * i)) & kByteMask);
        }
    };
    put16(0x10, 2);   // width
    put16(0x12, 2);   // height
    put16(0x14, 24);  // bpp
    put32(0x20, static_cast<std::uint32_t>(rle_stream.size()));
    put32(0x24, key);
    put32(0x28, kCbgLeafCount);
    blob[0x2C] = 0x00;
    blob[0x2D] = 0x00;
    put16(0x2E, 1);  // version

    std::uint32_t state = key;
    for (int i = 0; i < kCbgLeafCount; ++i) {
        std::uint32_t adv = cbg_hash_update(state);
        blob.push_back(static_cast<std::uint8_t>((1u + adv) & kByteMask));
    }

    BitWriter bw;
    for (std::uint8_t b : rle_stream) {
        bw.put_bits(b, kBitsPerByte);
    }
    Bytes bits = bw.finish();
    blob.insert(blob.end(), bits.begin(), bits.end());
    return blob;
}

}  // namespace

TEST(Cbg, cbg_decode_2x2_diff) {
    Bytes rle = {24, 10, 20, 30, 1, 1, 1, 5, 5, 5, 2, 2, 2};
    Bytes blob = make_cbg_2x2(rle);
    bgi::CbgImage img = bgi::decode_cbg(blob);
    EXPECT_EQ(img.width, 2);
    EXPECT_EQ(img.height, 2);
    EXPECT_EQ(img.bpp, 24);
    const Bytes expect = {10, 20, 30, 11, 21, 31, 15, 25, 35, 15, 25, 35};
    EXPECT_EQ(img.pixels, expect);
}

TEST(Cbg, cbg_zero_run) {
    Bytes rle = {11, 7, 100, 100, 100, 1, 2, 3, 4};
    Bytes blob = make_cbg_2x2(rle);
    bgi::CbgImage img = bgi::decode_cbg(blob);
    const Bytes expect = {0, 0, 0, 0, 0, 100, 100, 100, 1, 52, 53, 54};
    EXPECT_EQ(img.pixels, expect);
}

TEST(Cbg, cbg_checksum_mismatch_throws) {
    Bytes rle = {24, 10, 20, 30, 1, 1, 1, 5, 5, 5, 2, 2, 2};
    Bytes blob = make_cbg_2x2(rle);
    blob[0x2C] ^= 0xFF;
    EXPECT_THROW(bgi::decode_cbg(blob), std::runtime_error);
}

TEST(Cbg, cbg_v2_rejected) {
    Bytes rle = {24, 10, 20, 30, 1, 1, 1, 5, 5, 5, 2, 2, 2};
    Bytes blob = make_cbg_2x2(rle);
    blob[0x2E] = 2;
    EXPECT_THROW(bgi::decode_cbg(blob), std::runtime_error);
}

TEST(Cbg, cbg_bad_magic_throws) {
    Bytes junk(64, 0);
    EXPECT_THROW(bgi::decode_cbg(junk), std::runtime_error);
}

TEST(Cbg, to_bmp_shape) {
    Bytes px = {1, 2, 3, 4, 5, 6};
    Bytes bmp = bgi::to_bmp(px, 2, 1, 24);
    ASSERT_GE(bmp.size(), 54u + 8u);
    EXPECT_EQ(bmp[0], 'B');
    EXPECT_EQ(bmp[1], 'M');
    auto rd32 = [&](std::size_t off) {
        return static_cast<std::uint32_t>(bmp[off]) | (bmp[off + 1] << 8) |
               (bmp[off + 2] << 16) | (bmp[off + 3] << 24);
    };
    EXPECT_EQ(rd32(10), 54u);
    EXPECT_EQ(rd32(18), 2u);
    EXPECT_EQ(rd32(22), 1u);
    EXPECT_EQ(bmp[28], 24);
    EXPECT_EQ(bmp[54], 1);
    EXPECT_EQ(bmp[57], 4);
}
