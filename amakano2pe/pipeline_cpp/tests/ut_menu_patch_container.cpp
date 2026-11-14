// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/core/fes_script_codec.h"
#include "menu/core/menu_patch_container.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kKnownHashEmptySeed0 = 0x00000000U;
constexpr std::uint32_t kKnownHash1ByteA = 0x92685F5EU;
constexpr std::uint32_t kKnownHash2BytesAB = 0x1AA14063U;
constexpr std::uint32_t kKnownHash3BytesABC = 0x13577C9BU;
constexpr std::uint32_t kKnownHash4BytesABCD = 0x26873021U;
constexpr std::size_t kOrigSliceLen = 128;
constexpr std::uint64_t kSampleOrigOffset = 0x123400ULL;
constexpr std::uint32_t kFesSampleReserved = 0x42U;
constexpr std::size_t kKcsHeaderSizeFieldOffset = 12;

std::vector<std::uint8_t> MakeDeterministicSlice(std::size_t len, std::uint8_t seed) {
    std::vector<std::uint8_t> out(len);
    for (std::size_t i = 0; i < len; ++i) {
        out[i] = static_cast<std::uint8_t>((i * 37U + seed) & 0xFFU);
    }
    return out;
}

}  // namespace

TEST(MenuPatchContainer, MurmurHash2MatchesCanonicalKnownAnswerVectors) {
    EXPECT_EQ(ama::menu::MurmurHash2(nullptr, 0, 0U), kKnownHashEmptySeed0);

    const std::array<std::uint8_t, 4> four_bytes{'a', 'b', 'c', 'd'};
    EXPECT_EQ(ama::menu::MurmurHash2(four_bytes.data(), 1, 0U), kKnownHash1ByteA);
    EXPECT_EQ(ama::menu::MurmurHash2(four_bytes.data(), 2, 0U), kKnownHash2BytesAB);
    EXPECT_EQ(ama::menu::MurmurHash2(four_bytes.data(), 3, 0U), kKnownHash3BytesABC);
    EXPECT_EQ(ama::menu::MurmurHash2(four_bytes.data(), 4, 0U), kKnownHash4BytesABCD);
}

TEST(MenuPatchContainer, Amk2MenuV1RoundTripUnmaskAndTamperDetection) {
    const std::vector<std::uint8_t> orig_slice_1 = MakeDeterministicSlice(kOrigSliceLen, 0x11);
    const std::vector<std::uint8_t> new_raw_1{'H', 'G', '-', '3', 0x0C, 0x00, 0x00, 0x00,
                                              0x00, 0x03, 0x00, 0x00, 0xAA, 0xBB, 0xCC, 0xDD};

    const std::vector<std::uint8_t> orig_slice_2 = MakeDeterministicSlice(kOrigSliceLen, 0x55);
    const std::vector<std::uint8_t> new_raw_2{'<', 'x', 'm', 'l', '>', 't', 'e', 's', 't'};

    ama::menu::MenuPatchBuilder builder;

    ama::menu::MenuPatchRecordInput rec1;
    rec1.source_rel_path = "image01.int";
    rec1.target_rel_path = "image\\sys_config.hg3";
    rec1.flags = ama::menu::kFlagLooseOverride;
    rec1.orig_offset = kSampleOrigOffset;
    rec1.orig_slice = orig_slice_1;
    rec1.new_raw = new_raw_1;
    builder.AddRecord(rec1);

    ama::menu::MenuPatchRecordInput rec2;
    rec2.source_rel_path = "config\\startup.xml";
    rec2.target_rel_path = "config\\startup.xml";
    rec2.flags = ama::menu::kFlagBackupStandalone;
    rec2.orig_offset = 0;
    rec2.orig_slice = orig_slice_2;
    rec2.new_raw = new_raw_2;
    builder.AddRecord(rec2);

    const std::vector<std::uint8_t> packed = builder.BuildBytes();
    ASSERT_GE(packed.size(), ama::menu::kMenuPatchHeaderSize);
    EXPECT_TRUE(ama::menu::VerifyNoRawAssetSignatures(packed));

    const ama::menu::MenuPatchContainerData manifest = ama::menu::ParseMenuPatchContainer(packed);
    EXPECT_EQ(manifest.format_version, ama::menu::kMenuPatchFormatVersion);
    ASSERT_EQ(manifest.records.size(), 2U);

    const std::vector<std::uint8_t> unmasked_1 =
        ama::menu::UnmaskMenuPatchRecord(manifest.records[0], orig_slice_1);
    EXPECT_EQ(unmasked_1, new_raw_1);

    const std::vector<std::uint8_t> unmasked_2 =
        ama::menu::UnmaskMenuPatchRecord(manifest.records[1], orig_slice_2);
    EXPECT_EQ(unmasked_2, new_raw_2);

    // Tamper with orig_slice -> must fail orig_slice_hash check.
    std::vector<std::uint8_t> bad_orig = orig_slice_1;
    bad_orig[0] ^= 0xFFU;
    EXPECT_THROW((void)ama::menu::UnmaskMenuPatchRecord(manifest.records[0], bad_orig),
                 std::runtime_error);

    // Tamper with xor_payload -> must fail new_raw_hash check.
    ama::menu::MenuPatchRecord corrupted_rec = manifest.records[0];
    corrupted_rec.xor_payload[0] ^= 0xFFU;
    EXPECT_THROW((void)ama::menu::UnmaskMenuPatchRecord(corrupted_rec, orig_slice_1),
                 std::runtime_error);

    // Tamper with manifest_hash in header -> must fail ParseMenuPatchContainer.
    std::vector<std::uint8_t> bad_header = packed;
    bad_header[16] ^= 0xFFU;
    EXPECT_THROW((void)ama::menu::ParseMenuPatchContainer(bad_header), std::runtime_error);

    // Path traversal or undersized orig_slice must be rejected by AddRecord.
    ama::menu::MenuPatchRecordInput bad_path = rec1;
    bad_path.target_rel_path = "..\\evil.hg3";
    EXPECT_THROW(builder.AddRecord(bad_path), std::invalid_argument);

    ama::menu::MenuPatchRecordInput tiny_slice = rec1;
    tiny_slice.orig_slice.resize(ama::menu::kMinOrigSliceBytes - 1);
    EXPECT_THROW(builder.AddRecord(tiny_slice), std::invalid_argument);
}

TEST(MenuPatchContainer, SignatureSanitizerRejectsRawSignatures) {
    const std::vector<std::vector<std::uint8_t>> forbidden_samples{
        {'x', 'H', 'G', '-', '3', 'y'},
        {0x00, 0x89, 'P', 'N', 'G', 0x0D},
        {'a', 'J', 'F', 'I', 'F', 'b'},
        {'F', 'E', 'S', '\0', 0x01},
        {'K', 'C', 'S', '\0', 0x02},
    };
    for (const auto& sample : forbidden_samples) {
        EXPECT_FALSE(ama::menu::VerifyNoRawAssetSignatures(sample));
    }
}

TEST(MenuPatchContainer, FesAndKcsScriptCodecLosslessRoundTrip) {
    const std::string fes_script = "// test FES script\nstrcmp g_TEMP_0 $str10 和倉\n";
    const std::vector<std::uint8_t> packed_fes =
        ama::menu::FesScriptCodec::PackFesText(fes_script, kFesSampleReserved);
    const ama::menu::FesPayload unpacked_fes = ama::menu::FesScriptCodec::UnpackFes(packed_fes);
    EXPECT_EQ(unpacked_fes.reserved, kFesSampleReserved);
    EXPECT_EQ(unpacked_fes.AsText(), fes_script);

    ama::menu::KcsPayload kcs_in;
    std::memcpy(kcs_in.header.data(), "KCS\0", 4);
    kcs_in.header[4] = 2;
    kcs_in.header[kKcsHeaderSizeFieldOffset] =
        static_cast<std::uint8_t>(ama::menu::kKcsHeaderBytes);
    kcs_in.header[24] = 0x77;
    kcs_in.body = {'K', 'C', 'S', '_', 'B', 'O', 'D', 'Y', '_', 'P', 'A', 'Y', 'L', 'O', 'A', 'D'};

    const std::vector<std::uint8_t> packed_kcs = ama::menu::FesScriptCodec::PackKcs(kcs_in);
    const ama::menu::KcsPayload unpacked_kcs = ama::menu::FesScriptCodec::UnpackKcs(packed_kcs);
    EXPECT_EQ(unpacked_kcs.header[24], 0x77);
    EXPECT_EQ(unpacked_kcs.header[20], static_cast<std::uint8_t>(kcs_in.body.size()));
    EXPECT_EQ(unpacked_kcs.body, kcs_in.body);
}
