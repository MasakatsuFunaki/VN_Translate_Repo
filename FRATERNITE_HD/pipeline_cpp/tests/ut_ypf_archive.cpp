// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// YPF v490 index parsing, YSTB decryption and entry reading.
#include "test_support.h"
#include "yuris/ypf_archive.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace frat;
using namespace frat::yuris;
using frat_test::build_ypf;
using frat_test::put_u32;
using frat_test::put_u64;

namespace {

constexpr std::uint32_t kYstbVersion = 0x1E1u;
constexpr std::uint32_t kYstbSingleInst = 1u;
constexpr std::uint32_t kFillStride = 0x11u;
constexpr std::uint32_t kFillModulus = 15u;
constexpr std::size_t kExpectedThreeEntries = 3u;
constexpr std::size_t kExpectedSingleEntry = 1u;
constexpr std::uint8_t kCompressedFlag = 1u;
constexpr std::uint8_t kUncompressedFlag = 0u;
constexpr std::size_t kNameLengthBias = 3u;
constexpr std::uint8_t kNameXorKey = 0xFFu;
constexpr std::uint32_t kSampleSmallSize = 7u;
constexpr std::uint64_t kSampleOffset = 999u;
constexpr std::uint32_t kYpfVersion490 = 490u;
constexpr std::size_t kReservedHeaderZeros = 16u;
constexpr std::uint32_t kFallbackRawSize = 100u;
constexpr std::uint32_t kFallbackPackedSize = 50u;
constexpr std::uint64_t kFallbackOffset = 4242u;
constexpr std::size_t kWrongMagicImageSize = 64u;
constexpr std::uint32_t kMinSectionBytes = 4u;
constexpr std::uint32_t kMaxSectionBytesExclusive = 8u;
constexpr std::uint32_t kTailSectionBytes = 3u;
constexpr std::size_t kYstbBodyOffset = 0x20u;
constexpr std::uint8_t kKeyLowByte0 = 0xC3u;
constexpr std::uint8_t kKeyLowByte1 = 0xDAu;
constexpr std::uint8_t kKeyLowByte2 = 0x94u;
constexpr std::uint8_t kExtraMismatchByte = 0xABu;
constexpr std::size_t kMagicByteCount = 4u;

Bytes body(const std::string& s) { return Bytes(s.begin(), s.end()); }

// A minimal YSTB whose four section sizes add up, so decrypt_ystb engages.
Bytes make_ystb(std::uint32_t csz, std::uint32_t asz, std::uint32_t rsz,
                std::uint32_t osz) {
    Bytes b;
    for (char c : std::string("YSTB")) {
        b.push_back(static_cast<std::uint8_t>(c));
    }
    put_u32(b, kYstbVersion);
    put_u32(b, kYstbSingleInst);
    put_u32(b, csz);
    put_u32(b, asz);
    put_u32(b, rsz);
    put_u32(b, osz);
    put_u32(b, 0);
    for (std::uint32_t i = 0; i < csz + asz + rsz + osz; ++i) {
        b.push_back(static_cast<std::uint8_t>(kFillStride * (i % kFillModulus) + 1));
    }
    return b;
}

}  // namespace

TEST(YpfArchive, ParseIndex_SyntheticArchive_RoundTrips) {
    const std::vector<std::pair<std::string, Bytes>> files = {
        {"ysbin\\yst00156.ybn", body("first entry payload")},
        {"ysbin\\yse.ybn", body("second")},
        {"ysbin\\yst_list.ybn", body("third entry, a bit longer")},
    };
    const Bytes image = build_ypf(files);
    const auto entries = parse_ypf_index_bytes(image, "mem");
    ASSERT_EQ(entries.size(), kExpectedThreeEntries);
    for (std::size_t i = 0; i < kExpectedThreeEntries; ++i) {
        EXPECT_EQ(entries[i].name, files[i].first);
        EXPECT_EQ(entries[i].compressed, kCompressedFlag);
        EXPECT_EQ(entries[i].raw_size, files[i].second.size());
        EXPECT_EQ(read_entry(image, entries[i]), files[i].second);
    }
    // Entry stride is 27 + name length; a wrong stride shows up as a garbled
    // second name, so pin the data offsets too.
    EXPECT_GT(entries[1].data_offset, entries[0].data_offset);
    EXPECT_GT(entries[2].data_offset, entries[1].data_offset);
}

TEST(YpfArchive, ParseIndex_NameProbe_StopsAtTheRealLength) {
    // name_size_raw lies (hinted = real + 3).  The probe must still land on
    // the real length: an ASCII name byte XOR 0xFF is 0x81-0xDF, always > 20,
    // so the `t > 20` guard can never stop inside the name.
    const std::string name = "ysbin\\yse.ybn";
    Bytes index;
    put_u32(index, 0);
    index.push_back(static_cast<std::uint8_t>((name.size() + kNameLengthBias) ^ kNameXorKey));
    for (char c : name) {
        index.push_back(static_cast<std::uint8_t>(c) ^ kNameXorKey);
    }
    index.push_back(0);
    index.push_back(kCompressedFlag);
    put_u32(index, kSampleSmallSize);
    put_u32(index, kSampleSmallSize);
    put_u64(index, kSampleOffset);
    put_u32(index, 0);

    Bytes image;
    for (char c : std::string("YPF")) {
        image.push_back(static_cast<std::uint8_t>(c));
    }
    image.push_back(0);
    put_u32(image, kYpfVersion490);
    put_u32(image, kExpectedSingleEntry);
    put_u32(image, static_cast<std::uint32_t>(index.size()));
    image.insert(image.end(), kReservedHeaderZeros, 0);
    image.insert(image.end(), index.begin(), index.end());

    const auto entries = parse_ypf_index_bytes(image, "mem");
    ASSERT_EQ(entries.size(), kExpectedSingleEntry);
    EXPECT_EQ(entries[0].name, name);
    EXPECT_EQ(entries[0].data_offset, kSampleOffset);
}

TEST(YpfArchive, ParseIndex_NameProbe_FallsBackToHint) {
    // compressed == 0 AND raw != packed, so no probe length is ever accepted;
    // name_len stays 0 and the hinted length is used verbatim.
    const std::string name = "ab";
    Bytes index;
    put_u32(index, 0);
    index.push_back(static_cast<std::uint8_t>(name.size() ^ kNameXorKey));
    for (char c : name) {
        index.push_back(static_cast<std::uint8_t>(c) ^ kNameXorKey);
    }
    index.push_back(0);
    index.push_back(kUncompressedFlag);
    put_u32(index, kFallbackRawSize);
    put_u32(index, kFallbackPackedSize);
    put_u64(index, kFallbackOffset);
    put_u32(index, 0);

    Bytes image;
    for (char c : std::string("YPF")) {
        image.push_back(static_cast<std::uint8_t>(c));
    }
    image.push_back(0);
    put_u32(image, kYpfVersion490);
    put_u32(image, kExpectedSingleEntry);
    put_u32(image, static_cast<std::uint32_t>(index.size()));
    image.insert(image.end(), kReservedHeaderZeros, 0);
    image.insert(image.end(), index.begin(), index.end());

    const auto entries = parse_ypf_index_bytes(image, "mem");
    ASSERT_EQ(entries.size(), kExpectedSingleEntry);
    EXPECT_EQ(entries[0].name, "ab");
    EXPECT_EQ(entries[0].raw_size, kFallbackRawSize);
    EXPECT_EQ(entries[0].packed_size, kFallbackPackedSize);
    EXPECT_EQ(entries[0].data_offset, kFallbackOffset);
}

TEST(YpfArchive, ParseIndex_RejectsNonYpf) {
    EXPECT_THROW(parse_ypf_index_bytes(body("short"), "x.ypf"), std::runtime_error);
    Bytes wrong_magic(kWrongMagicImageSize, 0);
    wrong_magic[0] = 'Y';
    wrong_magic[1] = 'P';
    wrong_magic[2] = 'X';
    EXPECT_THROW(parse_ypf_index_bytes(wrong_magic, "x.ypf"), std::runtime_error);
}

TEST(YpfArchive, DecryptYstb_IsAnInvolution_AllSectionRemainders) {
    for (std::uint32_t c = kMinSectionBytes; c < kMaxSectionBytesExclusive; ++c) {
        for (std::uint32_t a = kMinSectionBytes; a < kMaxSectionBytesExclusive; ++a) {
            for (std::uint32_t r = kMinSectionBytes; r < kMaxSectionBytesExclusive; ++r) {
                for (std::uint32_t o = kMinSectionBytes; o < kMaxSectionBytesExclusive; ++o) {
                    const Bytes plain = make_ystb(c, a, r, o);
                    const Bytes enc = decrypt_ystb(plain);
                    EXPECT_NE(enc, plain);
                    EXPECT_EQ(decrypt_ystb(enc), plain)
                        << c << "/" << a << "/" << r << "/" << o;
                }
            }
        }
    }
}

TEST(YpfArchive, DecryptYstb_TailBytesUseTheKeyLowBytesInOrder) {
    // A 3-byte code section: the tail bytes XOR with 0xC3, 0xDA, 0x94 -- the
    // key's low bytes ascending.  A big-endian tail would use 0x65 first.
    const Bytes plain = make_ystb(kTailSectionBytes, 0, 0, 0);
    const Bytes enc = decrypt_ystb(plain);
    EXPECT_EQ(enc[kYstbBodyOffset] ^ plain[kYstbBodyOffset], kKeyLowByte0);
    EXPECT_EQ(enc[kYstbBodyOffset + 1u] ^ plain[kYstbBodyOffset + 1u], kKeyLowByte1);
    EXPECT_EQ(enc[kYstbBodyOffset + 2u] ^ plain[kYstbBodyOffset + 2u], kKeyLowByte2);
}

TEST(YpfArchive, DecryptYstb_NoOpOnSizeMismatchAndOnNonYstbMagic) {
    Bytes bad = make_ystb(kMinSectionBytes, kMinSectionBytes, kMinSectionBytes, kMinSectionBytes);
    bad.push_back(kExtraMismatchByte);  // total no longer equals 0x20 + sections
    EXPECT_EQ(decrypt_ystb(bad), bad);

    // The plaintext sidecars are YSCF / YSER / YSTL; decrypting them would
    // turn readable strings into garbage.
    for (const char* magic : {"YSCF", "YSER", "YSTL"}) {
        Bytes blob =
            make_ystb(kMinSectionBytes, kMinSectionBytes, kMinSectionBytes, kMinSectionBytes);
        std::memcpy(blob.data(), magic, kMagicByteCount);
        EXPECT_EQ(decrypt_ystb(blob), blob) << magic;
    }
}

TEST(YpfArchive, Basename_SplitsOnBothSeparators) {
    EXPECT_EQ(basename("ysbin\\yst00156.ybn"), "yst00156.ybn");
    EXPECT_EQ(basename("a/b\\c.ybn"), "c.ybn");
    EXPECT_EQ(basename("plain.ybn"), "plain.ybn");
}

TEST(YpfArchive, ParseIndex_Cp932FilenameWithPermutedLengthByte_PreservesRawMetadata) {
    const std::string utf8_name = "cgsys\\extra\\cgmode\\bgv\\紗英子_a.png";
    const auto cp932_opt = utf8_to_cp932_strict(utf8_name);
    ASSERT_TRUE(cp932_opt.has_value());
    const Bytes& name_cp932 = *cp932_opt;

    const Bytes payload = body("raw png payload bytes");
    const std::uint32_t expected_nhash = ypf_murmur_hash2(name_cp932);
    const std::uint32_t expected_dhash = ypf_murmur_hash2(payload);
    // Simulate YU-RIS v490 length-table permutation (0xF9 -> hinted 6, real length > 30).
    constexpr std::uint8_t kPermutedRawSize = 0xF9u;
    constexpr std::uint8_t kPngEntryType = 2u;
    constexpr std::uint64_t kFooterRemainBytes = 12u;

    Bytes index;
    put_u32(index, expected_nhash);
    index.push_back(kPermutedRawSize);
    for (const std::uint8_t b : name_cp932) {
        index.push_back(static_cast<std::uint8_t>(b ^ YPF_NAME_XOR_KEY));
    }
    index.push_back(kPngEntryType);
    index.push_back(kUncompressedFlag);
    put_u32(index, static_cast<std::uint32_t>(payload.size()));
    put_u32(index, static_cast<std::uint32_t>(payload.size()));
    const std::uint64_t data_offset = YPF_HEADER_SIZE + index.size() + kFooterRemainBytes;
    put_u64(index, data_offset);
    put_u32(index, expected_dhash);

    Bytes image;
    for (char c : std::string("YPF")) {
        image.push_back(static_cast<std::uint8_t>(c));
    }
    image.push_back(0);
    put_u32(image, kYpfVersion490);
    put_u32(image, kExpectedSingleEntry);
    put_u32(image, static_cast<std::uint32_t>(YPF_HEADER_SIZE + index.size()));
    image.insert(image.end(), kReservedHeaderZeros, 0);
    image.insert(image.end(), index.begin(), index.end());
    image.insert(image.end(), payload.begin(), payload.end());

    const auto entries = parse_ypf_index_bytes(image, "cgsys.ypf");
    ASSERT_EQ(entries.size(), kExpectedSingleEntry);
    EXPECT_EQ(entries[0].name, utf8_name);
    EXPECT_EQ(entries[0].name_hash, expected_nhash);
    EXPECT_EQ(entries[0].name_size_raw, kPermutedRawSize);
    EXPECT_EQ(entries[0].name_bytes_xor.size(), name_cp932.size());
    EXPECT_EQ(entries[0].type, kPngEntryType);
    EXPECT_EQ(entries[0].compressed, kUncompressedFlag);
    EXPECT_EQ(entries[0].raw_size, payload.size());
    EXPECT_EQ(entries[0].packed_size, payload.size());
    EXPECT_EQ(entries[0].data_offset, data_offset);
    EXPECT_EQ(entries[0].data_hash, expected_dhash);
    EXPECT_EQ(read_entry(image, entries[0]), payload);
}
