// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// DSC decompression + ARC20 list/read/repack round-trip unit tests.  The DSC
// helper crafts a perfect-tree blob (all code lengths 9) so the encoder is trivial.
#include "arc_fixture.h"
#include "bgi/bgi_archive.h"
#include "bgi/bgi_sysgrp.h"
#include "common/util.h"
#include "menu/bgra_image.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using snu::Bytes;
namespace bgi = snu::bgi;
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kHashWordMask = 0xFFFFu;
constexpr std::uint32_t kHashMulLo = 20021u;
constexpr std::uint32_t kHashMulHi = 346u;
constexpr int kWordShift = 16;
constexpr std::uint32_t kHashReturnMask = 0x7FFFu;
constexpr int kBitsPerByte = 8;
constexpr std::uint32_t kDefaultDscKey = 0x12345678u;
constexpr std::size_t kDscMagicFieldBytes = 16;
constexpr int kDwordBytes = 4;
constexpr int kHuffmanAlphabetSize = 512;
constexpr int kPerfectTreeCodeBits = 9;
constexpr std::uint32_t kByteMask = 0xFFu;
constexpr int kFirstLzSymbol = 256;
constexpr int kLzOffsetBits = 12;
constexpr int kLzCopyTestSymbol = 258;
constexpr int kLzCopyTestEncodedOffset = 2;
constexpr std::uint32_t kLzCopyTestDecodedSize = 8;
constexpr std::size_t kJunkDscBytes = 64;
constexpr std::uint8_t kJunkFillByte = 0x41;
constexpr std::size_t kArcHeaderBytes = 16;
constexpr std::size_t kArcIndexExtraOffset = 104;
constexpr std::size_t kArcRecordBytes = 128;
constexpr std::size_t kArcRecordLastOffset = 127;
constexpr std::uint8_t kFixturePaddingByte = 0xEE;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view utf8_name)
        : path_(fs::temp_directory_path() / fs::u8path(utf8_name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

// Mirror of the asmodean PRNG for scrambling test data.
std::uint32_t hash_update(std::uint32_t& state) {
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
            put_bit(static_cast<int>((value >> i) & 1u));
        }
    }
    void put_bit(int b) {
        cur_ = static_cast<std::uint8_t>((cur_ << 1) | (b & 1));
        if (++count_ == kBitsPerByte) {
            out_.push_back(cur_);
            cur_ = 0;
            count_ = 0;
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

// Build a DSC blob whose Huffman stream encodes `symbols` (0..255 literal,
// 256+n = LZ copy of length n+2 with the 12-bit offsets from `offsets`).
Bytes make_dsc(const std::vector<int>& symbols, const std::vector<int>& offsets,
               std::uint32_t dec_size, std::uint32_t key = kDefaultDscKey) {
    Bytes blob;
    const char magic[] = "DSC FORMAT 1.00";
    blob.insert(blob.end(), magic, magic + kDscMagicFieldBytes);
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < kDwordBytes; ++i) {
            blob.push_back(static_cast<std::uint8_t>(v >> (kBitsPerByte * i)));
        }
    };
    put32(key);
    put32(dec_size);
    put32(static_cast<std::uint32_t>(symbols.size()));
    put32(0);

    std::uint32_t state = key;
    for (int i = 0; i < kHuffmanAlphabetSize; ++i) {
        std::uint32_t adv = hash_update(state);
        blob.push_back(static_cast<std::uint8_t>((kPerfectTreeCodeBits + adv) & kByteMask));
    }

    BitWriter bw;
    std::size_t off_idx = 0;
    for (int sym : symbols) {
        bw.put_bits(static_cast<std::uint32_t>(sym), kPerfectTreeCodeBits);
        if (sym >= kFirstLzSymbol) {
            bw.put_bits(static_cast<std::uint32_t>(offsets.at(off_idx++)),
                        kLzOffsetBits);
        }
    }
    Bytes bits = bw.finish();
    blob.insert(blob.end(), bits.begin(), bits.end());
    return blob;
}

}  // namespace

TEST(BgiArchive, dsc_literal_round_trip) {
    const std::string payload = "Hello, BGI world!";
    std::vector<int> symbols(payload.begin(), payload.end());
    Bytes blob = make_dsc(symbols, {}, static_cast<std::uint32_t>(payload.size()));
    Bytes out = bgi::dsc_decompress(blob);
    ASSERT_EQ(out.size(), payload.size());
    EXPECT_EQ(std::memcmp(out.data(), payload.data(), payload.size()), 0);
}

TEST(BgiArchive, dsc_lz_copy) {
    // "ABAB" + LZ(length 4, offset 4) -> "ABABABAB".  Symbol 258 = length
    // (258-256)+2 = 4; stream offset field stores offset-2 = 2.
    const std::vector<int> symbols = {'A', 'B', 'A', 'B', kLzCopyTestSymbol};
    const Bytes blob =
        make_dsc(symbols, {kLzCopyTestEncodedOffset}, kLzCopyTestDecodedSize);
    const Bytes out = bgi::dsc_decompress(blob);
    const std::string expect = "ABABABAB";
    ASSERT_EQ(out.size(), expect.size());
    EXPECT_EQ(std::memcmp(out.data(), expect.data(), expect.size()), 0);
}

TEST(BgiArchive, dsc_bad_magic_throws) {
    const Bytes junk(kJunkDscBytes, kJunkFillByte);
    EXPECT_THROW(bgi::dsc_decompress(junk), std::runtime_error);
}

using snu::test_fixture::make_arc;

TEST(BgiArchive, arc20_list_and_read) {
    const Bytes a = {1, 2, 3, 4};
    const Bytes b = {9, 8, 7};
    const ScopedTempDir scratch("snu_ut_arc20_list");
    const std::string path = (scratch.path() / fs::u8path("test.arc")).u8string();
    snu::write_file(path, make_arc({{"first", a}, {"second", b}}));

    const auto entries = bgi::list_arc(path);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "first");
    EXPECT_EQ(entries[1].name, "second");
    EXPECT_EQ(entries[1].offset, 4u);
    EXPECT_EQ(entries[1].size, 3u);

    const Bytes r = bgi::read_entry(path, 1);
    EXPECT_EQ(r, b);
    const Bytes r2 = bgi::read_entry_by_name(path, "first");
    EXPECT_EQ(r2, a);
}

TEST(BgiArchive, arc20_repack_replaces_and_preserves) {
    const Bytes a = {1, 2, 3, 4};
    const Bytes b = {9, 8, 7};
    const ScopedTempDir scratch("snu_ut_arc20_repack");
    const std::string in = (scratch.path() / fs::u8path("in.arc")).u8string();
    const std::string out = (scratch.path() / fs::u8path("out.arc")).u8string();
    snu::write_file(in, make_arc({{"keep", a}, {"swap", b}}));

    const Bytes replacement = {0x55, 0x66, 0x77, 0x88, 0x99};
    bgi::repack_arc(in, out, {{"swap", replacement}});

    const auto entries = bgi::list_arc(out);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(bgi::read_entry(out, 0), a);
    EXPECT_EQ(bgi::read_entry(out, 1), replacement);

    // Index-extra bytes (offset 104..128 of each record) survive verbatim.
    const Bytes raw = snu::read_file(out);
    EXPECT_EQ(raw[kArcHeaderBytes + kArcIndexExtraOffset], kFixturePaddingByte);
    EXPECT_EQ(raw[kArcHeaderBytes + kArcRecordBytes + kArcRecordLastOffset],
              kFixturePaddingByte);
}

TEST(BgiArchive, arc20_unknown_name_throws) {
    const ScopedTempDir scratch("snu_ut_arc20_unk");
    const std::string path = (scratch.path() / fs::u8path("unk.arc")).u8string();
    snu::write_file(path, make_arc({{"a", Bytes{0x78}}}));
    EXPECT_THROW(bgi::read_entry_by_name(path, "not_in_archive"),
                 std::out_of_range);
}

TEST(BgiArchive, arc20_bad_magic_rejected) {
    const ScopedTempDir scratch("snu_ut_arc20_bad");
    const std::string path = (scratch.path() / fs::u8path("bad.arc")).u8string();
    const Bytes bogus = {'N', 'O', 'T', ' ', 'B', 'U', 'R', 'I', 'K', 'O', '!', '!',
                         0, 0, 0, 0};
    snu::write_file(path, bogus);
    EXPECT_THROW(bgi::list_arc(path), std::runtime_error);
}

TEST(BgiArchive, arc20_dsc_payload_transparent_decompress) {
    const std::string payload = "compressed!";
    const std::vector<int> symbols(payload.begin(), payload.end());
    const Bytes dsc =
        make_dsc(symbols, {}, static_cast<std::uint32_t>(payload.size()));
    const ScopedTempDir scratch("snu_ut_arc20_dsc");
    const std::string path = (scratch.path() / fs::u8path("dsc.arc")).u8string();
    snu::write_file(path, make_arc({{"script", dsc}}));

    const Bytes plain = bgi::read_entry(path, 0);
    ASSERT_EQ(plain.size(), payload.size());
    EXPECT_EQ(std::memcmp(plain.data(), payload.data(), payload.size()), 0);

    const Bytes raw = bgi::read_entry(path, 0, /*decompress=*/false);
    EXPECT_EQ(raw, dsc);
}

namespace {

constexpr std::size_t kDscHeaderBytes = 32;
constexpr std::size_t kDscKeyOffset = 16;
constexpr std::size_t kDscDecSizeOffset = 20;
constexpr std::size_t kDscDecCountOffset = 24;
constexpr std::size_t kDscReservedOffset = 28;
constexpr std::size_t kDscCodeTableOffset = 32;
constexpr std::size_t kDscBitStreamOffset = kDscHeaderBytes + kHuffmanAlphabetSize;
constexpr std::size_t kTwoLiteralPinnedBlobSize = kDscBitStreamOffset + 1;
constexpr std::uint8_t kPinnedZeroKeyCode0 = 0x01;
constexpr std::uint8_t kPinnedZeroKeyCode1 = 0x5B;
constexpr std::uint8_t kPinnedZeroKeyCode2 = 0x82;
constexpr std::uint8_t kPinnedTwoLiteralBitstream = 0x40;
constexpr std::size_t kMaxDscLzOffset = 4097;
constexpr std::size_t kRepeatPhraseBytes = 32;
constexpr std::uint8_t kWindowFillerByte = 0x5A;
constexpr std::uint8_t kPhraseBaseByte = 0xA0;
constexpr int kFibonacciSymbolCount = 18;
constexpr std::size_t kUiSyntheticPlateBytes = 32768;
constexpr std::size_t kUiRowBytes = 256;
constexpr std::size_t kMinCompressionDivisor = 3;
constexpr std::uint32_t kMinWindowTokenDelta = 20u;
constexpr std::size_t kTruncatedSysGrpHeaderBytes = 15;
constexpr int kUnsupportedSysGrpBpp = 16;

std::uint32_t ReadLe32(const Bytes& b, std::size_t offset) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + offset, sizeof(v));
    return v;
}

}  // namespace

TEST(BgiArchive, dsc_compress_pinned_two_literal_wire_vector) {
    // Two distinct bytes {0x00, 0x01} cannot form an LZ77 match (offset >= 2),
    // uniquely fixing frequencies {sym 0: 1, sym 1: 1}, canonical lengths {1, 1},
    // canonical codes {'0', '1'} -> bitstream 0b01000000 = 0x40, and key=0
    // scrambled lengths [0]=0x01, [1]=0x5B, [2]=0x82.
    const Bytes raw = {0x00, 0x01};
    const Bytes blob = bgi::dsc_compress(raw, 0u);

    ASSERT_EQ(blob.size(), kTwoLiteralPinnedBlobSize);
    EXPECT_TRUE(bgi::is_dsc(blob));
    EXPECT_EQ(ReadLe32(blob, kDscKeyOffset), 0u);
    EXPECT_EQ(ReadLe32(blob, kDscDecSizeOffset), 2u);
    EXPECT_EQ(ReadLe32(blob, kDscDecCountOffset), 2u);
    EXPECT_EQ(ReadLe32(blob, kDscReservedOffset), 0u);
    EXPECT_EQ(blob[kDscCodeTableOffset + 0], kPinnedZeroKeyCode0);
    EXPECT_EQ(blob[kDscCodeTableOffset + 1], kPinnedZeroKeyCode1);
    EXPECT_EQ(blob[kDscCodeTableOffset + 2], kPinnedZeroKeyCode2);
    EXPECT_EQ(blob[kDscBitStreamOffset], kPinnedTwoLiteralBitstream);
    EXPECT_EQ(bgi::dsc_decompress(blob), raw);
}

TEST(BgiArchive, dsc_compress_empty_and_single_symbol_boundaries) {
    const Bytes empty_raw{};
    const Bytes empty_blob = bgi::dsc_compress(empty_raw);
    ASSERT_GE(empty_blob.size(), kDscBitStreamOffset);
    EXPECT_EQ(ReadLe32(empty_blob, kDscKeyOffset), bgi::kDefaultDscHashKey);
    EXPECT_EQ(ReadLe32(empty_blob, kDscDecSizeOffset), 0u);
    EXPECT_EQ(ReadLe32(empty_blob, kDscDecCountOffset), 0u);
    EXPECT_EQ(ReadLe32(empty_blob, kDscReservedOffset), 0u);
    EXPECT_EQ(bgi::dsc_decompress(empty_blob), empty_raw);

    const Bytes single_byte = {0x80};
    const Bytes single_blob = bgi::dsc_compress(single_byte);
    ASSERT_GT(single_blob.size(), kDscBitStreamOffset);
    EXPECT_EQ(ReadLe32(single_blob, kDscDecSizeOffset), 1u);
    EXPECT_EQ(ReadLe32(single_blob, kDscDecCountOffset), 1u);
    EXPECT_EQ(bgi::dsc_decompress(single_blob), single_byte);

    // 4 distinct sign-bit boundary bytes -> zero LZ77 matches -> dec_count == 4.
    const Bytes sign_seam = {0x00, 0x7F, 0x80, 0xFF};
    const Bytes seam_blob = bgi::dsc_compress(sign_seam);
    ASSERT_GT(seam_blob.size(), kDscBitStreamOffset);
    EXPECT_EQ(ReadLe32(seam_blob, kDscDecSizeOffset), 4u);
    EXPECT_EQ(ReadLe32(seam_blob, kDscDecCountOffset), 4u);
    EXPECT_EQ(bgi::dsc_decompress(seam_blob), sign_seam);
}

TEST(BgiArchive, dsc_compress_lz77_window_seam_4097_and_plate_ratio) {
    auto build_window_probe = [](std::size_t distance) {
        Bytes buf(distance + kRepeatPhraseBytes, kWindowFillerByte);
        for (std::size_t i = 0; i < kRepeatPhraseBytes; ++i) {
            const auto v = static_cast<std::uint8_t>(kPhraseBaseByte + i);
            buf[i] = v;
            buf[distance + i] = v;
        }
        return buf;
    };

    // At exact max DSC window offset 4097, the second 32-byte phrase is reachable
    // in 1 LZ77 match token; at 4098 (1 byte past window), it falls out of reach
    // and emits 32 literal tokens instead.
    const Bytes at_limit = build_window_probe(kMaxDscLzOffset);
    const Bytes past_limit = build_window_probe(kMaxDscLzOffset + 1);

    const Bytes blob_at_limit = bgi::dsc_compress(at_limit);
    const Bytes blob_past_limit = bgi::dsc_compress(past_limit);

    ASSERT_EQ(bgi::dsc_decompress(blob_at_limit), at_limit);
    ASSERT_EQ(bgi::dsc_decompress(blob_past_limit), past_limit);

    const std::uint32_t count_at_limit = ReadLe32(blob_at_limit, kDscDecCountOffset);
    const std::uint32_t count_past_limit = ReadLe32(blob_past_limit, kDscDecCountOffset);
    EXPECT_GT(count_at_limit, 0u);
    EXPECT_LT(count_at_limit + kMinWindowTokenDelta, count_past_limit);

    // Synthetic UI-like scanline buffer (> 3x compression + non-zero dec_count).
    Bytes ui_plate(kUiSyntheticPlateBytes, 0);
    for (std::size_t i = 0; i < ui_plate.size(); ++i) {
        ui_plate[i] = static_cast<std::uint8_t>((i / kUiRowBytes) ^ (i & 3u));
    }
    const Bytes ui_blob = bgi::dsc_compress(ui_plate);
    EXPECT_LT(ui_blob.size(), ui_plate.size() / kMinCompressionDivisor);
    const std::uint32_t ui_tokens = ReadLe32(ui_blob, kDscDecCountOffset);
    EXPECT_GT(ui_tokens, 0u);
    EXPECT_LT(ui_tokens, static_cast<std::uint32_t>(ui_plate.size()));
    EXPECT_EQ(bgi::dsc_decompress(ui_blob), ui_plate);
}

TEST(BgiArchive, dsc_compress_clamps_skewed_huffman_depths_to_valid_tree) {
    // 18 distinct non-repeating pairs with Fibonacci frequencies force an
    // unconstrained Huffman tree past depth 15, exercising Kraft-complete
    // length limiting (<= 15 bits).
    Bytes skewed;
    std::size_t f_prev = 1;
    std::size_t f_cur = 1;
    for (int sym_idx = 0; sym_idx < kFibonacciSymbolCount; ++sym_idx) {
        for (std::size_t k = 0; k < f_prev; ++k) {
            skewed.push_back(static_cast<std::uint8_t>(sym_idx));
            skewed.push_back(static_cast<std::uint8_t>(0xFFu - sym_idx));
        }
        const std::size_t next = f_prev + f_cur;
        f_prev = f_cur;
        f_cur = next;
    }

    const Bytes blob = bgi::dsc_compress(skewed);
    EXPECT_GT(ReadLe32(blob, kDscDecCountOffset), 0u);
    EXPECT_EQ(bgi::dsc_decompress(blob), skewed);
}

TEST(BgiSysGrp, decode_and_encode_pinned_24bpp_and_32bpp_vectors) {
    // Pinned 2x2 24-bpp sysgrp payload (16B header + 12B top-down BGR).
    const Bytes raw_24 = {
        0x02, 0x00, 0x02, 0x00, 0x18, 0x00,
        0x01, 0x02, 0x7F, 0x80, 0xFE, 0xFF, 0x10, 0x20, 0x30, 0x40,
        0x00, 0x7F, 0x80,  // (0, 0)
        0xFF, 0x01, 0xFE,  // (1, 0)
        0x12, 0x34, 0x56,  // (0, 1)
        0x9A, 0xBC, 0xDE,  // (1, 1)
    };
    const std::array<std::uint8_t, bgi::kSysGrpReservedBytes> expected_res_24 = {
        0x01, 0x02, 0x7F, 0x80, 0xFE, 0xFF, 0x10, 0x20, 0x30, 0x40,
    };

    const bgi::SysGrpImage dec_24 = bgi::decode_sysgrp(raw_24);
    EXPECT_EQ(dec_24.orig_bpp, snu::menu::kBmpBpp24);
    EXPECT_EQ(dec_24.reserved, expected_res_24);
    ASSERT_EQ(dec_24.image.width(), 2);
    ASSERT_EQ(dec_24.image.height(), 2);
    EXPECT_EQ(dec_24.image.PixelAt(0, 0), (snu::menu::BgraColor{0x00, 0x7F, 0x80, 0xFF}));
    EXPECT_EQ(dec_24.image.PixelAt(1, 0), (snu::menu::BgraColor{0xFF, 0x01, 0xFE, 0xFF}));
    EXPECT_EQ(dec_24.image.PixelAt(0, 1), (snu::menu::BgraColor{0x12, 0x34, 0x56, 0xFF}));
    EXPECT_EQ(dec_24.image.PixelAt(1, 1), (snu::menu::BgraColor{0x9A, 0xBC, 0xDE, 0xFF}));
    EXPECT_EQ(bgi::encode_sysgrp(dec_24.image, snu::menu::kBmpBpp24, dec_24.reserved), raw_24);

    // Pinned 2x1 32-bpp sysgrp payload preserving RGB on alpha == 0 pixel.
    const Bytes raw_32 = {
        0x02, 0x00, 0x01, 0x00, 0x20, 0x00,
        0xAA, 0x55, 0x00, 0xFF, 0x7F, 0x80, 0x11, 0x22, 0x33, 0x44,
        0x33, 0x66, 0x99, 0x00,  // (0, 0) transparent with non-zero RGB
        0xFE, 0x80, 0x7F, 0x80,  // (1, 0) translucent sign-bit seam
    };
    const bgi::SysGrpImage dec_32 = bgi::decode_sysgrp(raw_32);
    EXPECT_EQ(dec_32.orig_bpp, snu::menu::kBmpBpp32);
    ASSERT_EQ(dec_32.image.width(), 2);
    ASSERT_EQ(dec_32.image.height(), 1);
    EXPECT_EQ(dec_32.image.PixelAt(0, 0), (snu::menu::BgraColor{0x33, 0x66, 0x99, 0x00}));
    EXPECT_EQ(dec_32.image.PixelAt(1, 0), (snu::menu::BgraColor{0xFE, 0x80, 0x7F, 0x80}));
    EXPECT_EQ(bgi::encode_sysgrp(dec_32.image, snu::menu::kBmpBpp32, dec_32.reserved), raw_32);
}

TEST(BgiSysGrp, rejects_malformed_headers_and_payload_lengths) {
    struct InvalidCase {
        const char* name;
        Bytes blob;
    };
    const InvalidCase kInvalidCases[] = {
        {"truncated_header_15b", Bytes(kTruncatedSysGrpHeaderBytes, 0)},
        {"zero_width", {0x00, 0x00, 0x01, 0x00, 0x20, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {"zero_height", {0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {"unsupported_bpp_16",
         {0x01, 0x00, 0x01, 0x00, 0x10, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2}},
        {"payload_short_by_1",
         {0x01, 0x00, 0x01, 0x00, 0x20, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3}},
        {"payload_long_by_1",
         {0x01, 0x00, 0x01, 0x00, 0x20, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5}},
    };

    for (const auto& tc : kInvalidCases) {
        SCOPED_TRACE(tc.name);
        EXPECT_THROW(bgi::decode_sysgrp(tc.blob), std::runtime_error);
    }

    const snu::menu::BgraImage empty_img;
    const snu::menu::BgraImage valid_img(1, 1, snu::menu::BgraColor{1, 2, 3, 4});
    EXPECT_THROW(bgi::encode_sysgrp(empty_img, snu::menu::kBmpBpp32), std::runtime_error);
    EXPECT_THROW(bgi::encode_sysgrp(valid_img, kUnsupportedSysGrpBpp), std::runtime_error);
}
