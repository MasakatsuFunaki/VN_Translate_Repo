// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for SiglusEngine scene chunk decryption, byte-LZSS decompression,
// SCENEHEADERV2 bounds checking, and unconditional SS_V2 string-pool decoding.
#include "siglus/scene_pck.h"
#include "siglus/siglus_decoder.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using namespace adi;
using namespace adi::siglus;

namespace {

constexpr std::uint8_t kByteMask = 0xFFu;
constexpr std::size_t kPrivateKeyMask = 0x0Fu;
constexpr unsigned kShift8 = 8;
constexpr unsigned kShift16 = 16;
constexpr unsigned kShift24 = 24;
constexpr std::size_t kChunkHeaderSize = 8u;
constexpr std::size_t kChunkDecompOffset = 4u;
constexpr std::uint32_t kSceneHeaderV2Size = 0x1Cu;
constexpr std::size_t kIdxOffField = 0x0Cu;
constexpr std::size_t kIdxCntField = 0x10u;
constexpr std::size_t kDataOffField = 0x14u;
constexpr std::uint32_t kIndexEntryStride = 8u;
constexpr std::size_t kEntryLenFieldOffset = 4u;

constexpr std::uint32_t kEightLiteralSize = 8u;
constexpr std::uint32_t kFourDecompSize = 4u;
constexpr std::uint32_t kElevenDecompSize = 11u;
constexpr std::uint32_t kThreeDecompSize = 3u;
constexpr std::uint32_t kTwoDecompSize = 2u;
constexpr std::size_t kThreeTruncatedSize = 3u;
constexpr std::uint32_t kSixteenDecompSize = 16u;
constexpr std::uint32_t kSmallCompSize = 4u;
constexpr std::uint32_t kNineCompSize = 9u;
constexpr std::uint32_t kImplausibleDecompSize = 0x10000001u;
constexpr std::size_t kKey256LastIdx = 255u;
constexpr std::uint8_t kExpectedKey0 = 0x70u;
constexpr std::uint8_t kExpectedKey255 = 0xBFu;
constexpr int kExpectedKeyChecksum = 33022;
constexpr std::size_t kPartialKeyCheckIdx = 4u;
constexpr std::uint8_t kExpectedPartialKeyByte4 = 0x7Du;
constexpr std::uint8_t kProbeByte0 = 0u;
constexpr std::uint8_t kProbeByte1 = 1u;
constexpr std::uint8_t kProbeByte2 = 2u;
constexpr std::uint8_t kProbeByte3 = 3u;
constexpr std::uint8_t kHighProbeByteA = 250u;
constexpr std::uint8_t kHighProbeByteB = 251u;

// Hand-computed per-index SS_V2 keys `(idx * 0x7087) & 0xFFFF`:
//   idx 0 -> 0x0000
//   idx 1 -> 0x7087
//   idx 2 -> 0xE10E
//   idx 3 -> 0x5195 (0x15195 & 0xFFFF)
constexpr std::uint16_t kKeyIdx1 = 0x7087u;
constexpr std::uint16_t kKeyIdx3 = 0x5195u;

// Pinned raw ciphertext for "white" at idx = 1 (`char ^ 0x7087`):
//   'w' (0x0077) ^ 0x7087 = 0x70F0
//   'h' (0x0068) ^ 0x7087 = 0x70EF
//   'i' (0x0069) ^ 0x7087 = 0x70EE
//   't' (0x0074) ^ 0x7087 = 0x70F3
//   'e' (0x0065) ^ 0x7087 = 0x70E2
constexpr std::uint16_t kCipherWhite0 = 0x70F0u;
constexpr std::uint16_t kCipherWhite1 = 0x70EFu;
constexpr std::uint16_t kCipherWhite2 = 0x70EEu;
constexpr std::uint16_t kCipherWhite3 = 0x70F3u;
constexpr std::uint16_t kCipherWhite4 = 0x70E2u;

// Pinned raw ciphertext for "あい" at idx = 2 (`unit ^ 0xE10E`):
//   U+3042 ('あ') ^ 0xE10E = 0xD14C
//   U+3044 ('い') ^ 0xE10E = 0xD14A
constexpr std::uint16_t kCipherHiraAIdx2 = 0xD14Cu;
constexpr std::uint16_t kCipherHiraIIdx2 = 0xD14Au;

// Surrogate boundary codepoints XORed at idx = 1 (`cp ^ 0x7087`):
//   0xD7FF ^ 0x7087 = 0xA778 (valid BMP below surrogates)
//   0xD800 ^ 0x7087 = 0xA887 (high-surrogate start -> throws)
//   0xDFFF ^ 0x7087 = 0xAF78 (low-surrogate end -> throws)
//   0xE000 ^ 0x7087 = 0x9087 (valid PUA above surrogates)
constexpr std::uint16_t kCipherBmpBeforeSurrogatesIdx1 = 0xA778u;
constexpr std::uint16_t kCipherHighSurrogateStartIdx1 = 0xA887u;
constexpr std::uint16_t kCipherLowSurrogateEndIdx1 = 0xAF78u;
constexpr std::uint16_t kCipherPuaAfterSurrogatesIdx1 = 0x9087u;

constexpr std::size_t kShortSceneBufSize = 0x1Bu;
constexpr std::uint32_t kTooManyEntries = 100001u;
constexpr std::uint32_t kMaxAllowedEntries = 100000u;
constexpr std::uint32_t kTooLongEntryUnits = 4097u;
constexpr std::uint32_t kOutOfBoundsWideOffset = 0xFFFFu;
constexpr std::size_t kExpectedGuardedEntryCount = 3u;
constexpr std::size_t kExpectedFourEntries = 4u;
constexpr std::size_t kExpectedThreeEntries = 3u;
constexpr std::size_t kExpectedTwoEntries = 2u;
constexpr std::size_t kExpectedOneEntry = 1u;

void put32(Bytes& b, std::size_t off, std::uint32_t v) {
    b[off] = static_cast<std::uint8_t>(v & kByteMask);
    b[off + 1] = static_cast<std::uint8_t>((v >> kShift8) & kByteMask);
    b[off + 2] = static_cast<std::uint8_t>((v >> kShift16) & kByteMask);
    b[off + 3] = static_cast<std::uint8_t>((v >> kShift24) & kByteMask);
}

Bytes encrypt(const Bytes& plain, const std::uint8_t* pk) {
    Bytes out(plain.size());
    for (std::size_t i = 0; i < plain.size(); ++i) {
        std::uint8_t b = static_cast<std::uint8_t>(plain[i] ^ SIGLUS_KEY_256[i & kByteMask]);
        if (pk) {
            b = static_cast<std::uint8_t>(b ^ pk[i & kPrivateKeyMask]);
        }
        out[i] = b;
    }
    return out;
}

Bytes chunk_header(std::uint32_t comp_size, std::uint32_t decomp_size, const Bytes& body) {
    Bytes b(kChunkHeaderSize, 0);
    put32(b, 0, comp_size);
    put32(b, kChunkDecompOffset, decomp_size);
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

Bytes build_scene(const std::vector<std::vector<std::uint16_t>>& entries) {
    const std::uint32_t idx_off = kSceneHeaderV2Size;
    const std::uint32_t data_off =
        idx_off + static_cast<std::uint32_t>(entries.size()) * kIndexEntryStride;
    Bytes out(data_off, 0);
    put32(out, 0, kSceneHeaderV2Size);
    put32(out, kIdxOffField, idx_off);
    put32(out, kIdxCntField, static_cast<std::uint32_t>(entries.size()));
    put32(out, kDataOffField, data_off);
    std::uint32_t woff = 0;
    Bytes blob;
    for (std::size_t j = 0; j < entries.size(); ++j) {
        const std::size_t entry_off = idx_off + j * kIndexEntryStride;
        put32(out, entry_off, woff);
        put32(out, entry_off + kEntryLenFieldOffset,
              static_cast<std::uint32_t>(entries[j].size()));
        for (std::uint16_t u : entries[j]) {
            blob.push_back(static_cast<std::uint8_t>(u & kByteMask));
            blob.push_back(static_cast<std::uint8_t>(u >> kShift8));
        }
        woff += static_cast<std::uint32_t>(entries[j].size());
    }
    out.insert(out.end(), blob.begin(), blob.end());
    return out;
}

}  // namespace

TEST(SiglusDecoder, LzssLiteralAndBackrefRoundTrip) {
    const Bytes lit = {0xFF, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
    Bytes out = decompress_lzss(lit.data(), lit.size(), kEightLiteralSize);
    EXPECT_EQ(std::string(out.begin(), out.end()), "ABCDEFGH");

    const Bytes mixed = {0x03, 'A', 'B', 0x20, 0x00};
    out = decompress_lzss(mixed.data(), mixed.size(), kFourDecompSize);
    EXPECT_EQ(std::string(out.begin(), out.end()), "ABAB");
}

TEST(SiglusDecoder, LzssOverlapReplicatesRun) {
    const Bytes s = {0x01, 'A', 0x18, 0x00};
    const Bytes out = decompress_lzss(s.data(), s.size(), kElevenDecompSize);
    EXPECT_EQ(std::string(out.begin(), out.end()), "AAAAAAAAAAA");
}

TEST(SiglusDecoder, LzssZeroOffsetAndNegativeSourceWriteZeros) {
    const Bytes zero_off = {0x00, 0x01, 0x00};
    Bytes out = decompress_lzss(zero_off.data(), zero_off.size(), kThreeDecompSize);
    EXPECT_EQ(out, (Bytes{0, 0, 0}));

    const Bytes neg = {0x00, 0x50, 0x00};
    out = decompress_lzss(neg.data(), neg.size(), kTwoDecompSize);
    EXPECT_EQ(out, (Bytes{0, 0}));
}

TEST(SiglusDecoder, LzssTruncatesOnShortInput) {
    const Bytes s = {0xFF, 'A', 'B', 'C'};
    const Bytes out = decompress_lzss(s.data(), s.size(), kEightLiteralSize);
    EXPECT_EQ(out.size(), kThreeTruncatedSize);
    EXPECT_EQ(std::string(out.begin(), out.end()), "ABC");

    const Bytes half = {0x00, 0x20};
    EXPECT_TRUE(decompress_lzss(half.data(), half.size(), kSixteenDecompSize).empty());
}

TEST(SiglusDecoder, DecodeSceneChunkRejectsShortAndImplausible) {
    const std::uint8_t* pk = ADIEU_PRIVATE_KEY.data();

    const Bytes tiny = encrypt(Bytes{1, 2, 3}, pk);
    EXPECT_THROW(decode_scene_chunk(tiny, pk), std::runtime_error);
    EXPECT_TRUE(decode_scene(tiny).empty());

    const Bytes zero = encrypt(chunk_header(kSmallCompSize, 0, Bytes{0xFF}), pk);
    EXPECT_THROW(decode_scene_chunk(zero, pk), std::runtime_error);
    EXPECT_TRUE(decode_scene(zero).empty());

    const Bytes huge =
        encrypt(chunk_header(kSmallCompSize, kImplausibleDecompSize, Bytes{0xFF}), pk);
    EXPECT_THROW(decode_scene_chunk(huge, pk), std::runtime_error);
    EXPECT_TRUE(decode_scene(huge).empty());

    const Bytes ok = encrypt(
        chunk_header(kNineCompSize, kEightLiteralSize,
                     Bytes{0xFF, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'}),
        pk);
    const Bytes plain = decode_scene_chunk(ok, pk);
    EXPECT_EQ(std::string(plain.begin(), plain.end()), "ABCDEFGH");
}

TEST(SiglusDecoder, DoubleXorKeysPinnedAndInvolutionHolds) {
    EXPECT_EQ(SIGLUS_KEY_256[0], kExpectedKey0);
    EXPECT_EQ(SIGLUS_KEY_256[kKey256LastIdx], kExpectedKey255);
    EXPECT_EQ(std::accumulate(SIGLUS_KEY_256.begin(), SIGLUS_KEY_256.end(), 0),
              kExpectedKeyChecksum);

    const std::array<std::uint8_t, kPrivateKeySize> expect_pk = {{
        0xA2, 0x7E, 0xC9, 0xDA, 0x2E, 0x2D, 0x09, 0x08,
        0xE5, 0xEA, 0xFA, 0x96, 0xA3, 0x1B, 0xED, 0xE0,
    }};
    EXPECT_EQ(ADIEU_PRIVATE_KEY, expect_pk);
    EXPECT_EQ(ADIEU_PRIVATE_KEY_PARTIAL[kPartialKeyCheckIdx], kExpectedPartialKeyByte4);

    const Bytes probe = {
        kProbeByte0, kProbeByte1, kProbeByte2, kProbeByte3, kHighProbeByteA, kHighProbeByteB,
    };
    EXPECT_EQ(decrypt_universal(decrypt_universal(probe)), probe);
}

// Scenes with zero hiragana (pure ASCII identifiers + kanji defaults, like
// `_start`, `_menu`, and `_cg_mode`) must still apply `(idx * 0x7087) & 0xFFFF`
// unconditionally instead of falling back to un-XORed fake-CJK codepoints.
TEST(SiglusDecoder, UnconditionalSsV2DecodesAsciiAndKanjiWithoutHiragana) {
    const Bytes ascii_kanji_scene = build_scene({
        {'_', 's', 't'},
        {kCipherWhite0, kCipherWhite1, kCipherWhite2, kCipherWhite3, kCipherWhite4},
        {},
        {static_cast<std::uint16_t>(u'人' ^ kKeyIdx3),
         static_cast<std::uint16_t>(u'見' ^ kKeyIdx3)},
    });
    const auto out = parse_scene_strings(ascii_kanji_scene);
    ASSERT_EQ(out.size(), kExpectedFourEntries);
    EXPECT_EQ(out[0], "_st");
    EXPECT_EQ(out[1], "white");
    EXPECT_EQ(out[2], "");
    EXPECT_EQ(out[3], "人見");

    const Bytes mixed_scene = build_scene({
        {'i', 'd'},
        {static_cast<std::uint16_t>(u'広' ^ kKeyIdx1),
         static_cast<std::uint16_t>(u'介' ^ kKeyIdx1)},
        {kCipherHiraAIdx2, kCipherHiraIIdx2},
    });
    const auto mixed_out = parse_scene_strings(mixed_scene);
    ASSERT_EQ(mixed_out.size(), kExpectedThreeEntries);
    EXPECT_EQ(mixed_out[0], "id");
    EXPECT_EQ(mixed_out[1], "広介");
    EXPECT_EQ(mixed_out[2], "あい");
}

TEST(SiglusDecoder, SurrogateBoundariesAfterSsV2XorRejectLoneSurrogates) {
    const Bytes valid_edges = build_scene({
        {0x0041u},
        {kCipherBmpBeforeSurrogatesIdx1, kCipherPuaAfterSurrogatesIdx1},
    });
    const auto ok = parse_scene_strings(valid_edges);
    ASSERT_EQ(ok.size(), kExpectedTwoEntries);
    EXPECT_EQ(ok[0], "A");
    EXPECT_EQ(char_len(ok[1]), kExpectedTwoEntries);

    const Bytes high_surrogate = build_scene({
        {0x0041u},
        {kCipherHighSurrogateStartIdx1},
    });
    EXPECT_THROW(parse_scene_strings(high_surrogate), LoneSurrogateError);

    const Bytes low_surrogate = build_scene({
        {0x0041u},
        {kCipherLowSurrogateEndIdx1},
    });
    EXPECT_THROW(parse_scene_strings(low_surrogate), LoneSurrogateError);
}

TEST(SiglusDecoder, ParseSceneStringsGuards) {
    EXPECT_TRUE(parse_scene_strings(Bytes(kShortSceneBufSize, 0)).empty());

    Bytes b = build_scene({{'a', 'b'}});
    put32(b, kIdxCntField, 0);
    EXPECT_TRUE(parse_scene_strings(b).empty());

    b = build_scene({{'a', 'b'}});
    put32(b, kIdxCntField, kTooManyEntries);
    EXPECT_TRUE(parse_scene_strings(b).empty());

    b = build_scene({{'a', 'b'}});
    put32(b, kIdxCntField, kMaxAllowedEntries);
    EXPECT_TRUE(parse_scene_strings(b).empty());

    b = build_scene({{'a', 'b'}});
    put32(b, kDataOffField, static_cast<std::uint32_t>(b.size()));
    EXPECT_TRUE(parse_scene_strings(b).empty());

    b = build_scene({{'a'}, {'b'}, {'c'}});
    put32(b, kSceneHeaderV2Size + 0 * kIndexEntryStride + kEntryLenFieldOffset, 0);
    put32(b, kSceneHeaderV2Size + 1 * kIndexEntryStride + kEntryLenFieldOffset,
          kTooLongEntryUnits);
    put32(b, kSceneHeaderV2Size + 2 * kIndexEntryStride, kOutOfBoundsWideOffset);
    const auto out = parse_scene_strings(b);
    ASSERT_EQ(out.size(), kExpectedGuardedEntryCount);
    EXPECT_EQ(out[0], "");
    EXPECT_EQ(out[1], "");
    EXPECT_EQ(out[2], "");
}

TEST(SiglusDecoder, TrailingNullsAndWhitespaceStripped) {
    const std::vector<std::uint16_t> padded = {
        static_cast<std::uint16_t>(u'あ'),
        static_cast<std::uint16_t>(u'　'),
        static_cast<std::uint16_t>(u'\0'),
        static_cast<std::uint16_t>(u'\0'),
    };
    const auto out = parse_scene_strings(build_scene({padded}));
    ASSERT_EQ(out.size(), kExpectedOneEntry);
    EXPECT_EQ(out[0], "あ");
}
