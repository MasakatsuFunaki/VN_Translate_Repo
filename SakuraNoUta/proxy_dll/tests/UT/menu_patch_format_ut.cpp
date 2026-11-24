// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-room unit tests for menu_patch_format wire codecs, hashes, path
// security validators, and BURIKO ARC20 archive slice/repack operations.

#include "menu_patch_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace menu_patch_format {
namespace {

constexpr std::size_t kEmptyBytes = 0u;
constexpr std::size_t kOneByte = 1u;
constexpr std::size_t kTwoBytes = 2u;
constexpr std::size_t kThreeBytes = 3u;
constexpr std::size_t kFourBytes = 4u;
constexpr std::size_t kFiveBytes = 5u;

constexpr std::uint32_t kNonZeroSeed = 1u;
constexpr std::uint32_t kPinnedXorHash = 0xA1B2C3D4u;

constexpr std::size_t kOffsetVersion = 0x08u;
constexpr std::size_t kOffsetRecordCount = 0x0Cu;
constexpr std::size_t kOffsetManifestHash = 0x10u;
constexpr std::size_t kOffsetReserved = 0x14u;
constexpr std::size_t kOffsetReservedLast = 0x1Fu;

constexpr std::size_t kSentinelOffsetVersion = 0x08u;
constexpr std::size_t kSentinelOffsetManifestHash = 0x0Cu;
constexpr std::size_t kSentinelOffsetPatchSize = 0x10u;
constexpr std::size_t kSentinelOffsetEntryCount = 0x18u;
constexpr std::size_t kMinSentinelFrameBytes =
    kSentinelHeaderBytes + kSentinelTrailerHashBytes;

constexpr std::size_t kArc20CountOffset = 0x0Cu;
constexpr std::size_t kArc20RelOffsetField = 0x60u;
constexpr std::size_t kArc20SizeField = 0x64u;

constexpr std::size_t kU16Bytes = 2u;
constexpr std::size_t kU32Bytes = 4u;
constexpr std::size_t kU64Bytes = 8u;
constexpr std::size_t kRecordFixedTailBytes =
    kU64Bytes + kU64Bytes + kU32Bytes + kU64Bytes + kU32Bytes + kU32Bytes;

constexpr int kShift8 = 8;
constexpr int kShift16 = 16;
constexpr int kShift24 = 24;
constexpr int kByteBits = 8;
constexpr std::uint32_t kLowByteMask = 0xFFu;

constexpr std::size_t kMinSliceVecSize =
    static_cast<std::size_t>(kMinOrigSliceBytes);
constexpr std::size_t kUndersizeOrigSliceBytes = kMinSliceVecSize - 1u;
constexpr std::size_t kShrunkPayloadBytes = 48u;
constexpr std::size_t kGrownPayloadBytes = 96u;
constexpr std::size_t kSmallPayloadBytes = 12u;
constexpr std::uint32_t kPatternStride = 17u;
constexpr std::size_t kGrownArcEntryBytes = 11u;
constexpr std::size_t kThreeArcEntries = 3u;

constexpr std::uint64_t kSampleArcSentinelSize = 262144ULL;
constexpr std::uint64_t kSampleSplashSentinelSize = 2764854ULL;

constexpr std::uint32_t kUnknownFlagBit = 0x04u;
constexpr std::uint32_t kCombinedExclusiveFlags =
    kFlagBackupStandalone | kFlagArchiveEntry;

constexpr wchar_t kSakuraBlossomLeadSurrogate = static_cast<wchar_t>(0xD83Cu);
constexpr wchar_t kSakuraBlossomTrailSurrogate = static_cast<wchar_t>(0xDF38u);

std::uint16_t ReadLe16(ByteSpan bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[offset + 1u]) << kShift8));
}

std::uint32_t ReadLe32(ByteSpan bytes, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1u]) << kShift8) |
           (static_cast<std::uint32_t>(bytes[offset + 2u]) << kShift16) |
           (static_cast<std::uint32_t>(bytes[offset + 3u]) << kShift24);
}

std::uint64_t ReadLe64(ByteSpan bytes, std::size_t offset) noexcept {
    std::uint64_t value = 0u;
    for (std::size_t i = 0u; i < kU64Bytes; ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i])
                 << static_cast<int>(i * static_cast<std::size_t>(kByteBits));
    }
    return value;
}

void WriteLe32(std::vector<std::uint8_t>& bytes,
               std::size_t offset,
               std::uint32_t value) noexcept {
    bytes[offset] = static_cast<std::uint8_t>(value & kLowByteMask);
    bytes[offset + 1u] =
        static_cast<std::uint8_t>((value >> kShift8) & kLowByteMask);
    bytes[offset + 2u] =
        static_cast<std::uint8_t>((value >> kShift16) & kLowByteMask);
    bytes[offset + 3u] =
        static_cast<std::uint8_t>((value >> kShift24) & kLowByteMask);
}

void WriteLe64(std::vector<std::uint8_t>& bytes,
               std::size_t offset,
               std::uint64_t value) noexcept {
    for (std::size_t i = 0u; i < kU64Bytes; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(
            (value >>
             static_cast<int>(i * static_cast<std::size_t>(kByteBits))) &
            kLowByteMask);
    }
}

void ResealManifestHash(std::vector<std::uint8_t>& blob) noexcept {
    const ByteSpan body = ByteSpan(blob).subspan(kPatchHeaderBytes);
    WriteLe32(blob, kOffsetManifestHash, MurmurHash2(body, kDefaultSeed));
}

void ResealSentinelTrailer(std::vector<std::uint8_t>& blob) noexcept {
    const std::size_t body_len = blob.size() - kSentinelTrailerHashBytes;
    const std::uint32_t hash =
        MurmurHash2(ByteSpan(blob).subspan(0u, body_len), kDefaultSeed);
    WriteLe32(blob, body_len, hash);
}

std::vector<std::uint8_t> MakePatternBytes(std::size_t count,
                                           std::uint8_t base_byte) {
    std::vector<std::uint8_t> out(count, 0u);
    for (std::size_t i = 0u; i < count; ++i) {
        out[i] = static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(base_byte) +
             static_cast<std::uint32_t>(i) * kPatternStride) &
            kLowByteMask);
    }
    return out;
}

MenuPatchRecordInput MakeStandaloneRecordInput() {
    MenuPatchRecordInput rec;
    rec.source_rel_path = "data02501.arc";
    rec.target_rel_path = "att01";
    rec.entry_name = "att01";
    rec.orig_offset = 0x190u;
    rec.orig_slice = MakePatternBytes(kMinSliceVecSize, 0x21u);
    rec.new_raw = MakePatternBytes(kSmallPayloadBytes, 0x80u);
    rec.flags = kFlagBackupStandalone;
    return rec;
}

struct SyntheticArcEntry {
    std::string_view name;
    std::vector<std::uint8_t> payload;
    std::array<std::uint8_t, kArc20ExtraFieldBytes> extra;
};

std::vector<std::uint8_t> BuildArc20Fixture(
    const std::vector<SyntheticArcEntry>& entries) {
    const std::size_t index_bytes = entries.size() * kArc20IndexEntryBytes;
    std::size_t payload_bytes = 0u;
    for (const auto& entry : entries) {
        payload_bytes += entry.payload.size();
    }

    std::vector<std::uint8_t> arc(
        kArc20HeaderBytes + index_bytes + payload_bytes, 0u);
    std::memcpy(arc.data(), kArc20Magic, kArc20MagicBytes);
    WriteLe32(arc, kArc20CountOffset,
              static_cast<std::uint32_t>(entries.size()));

    std::uint32_t rel_offset = 0u;
    const std::size_t data_start = kArc20HeaderBytes + index_bytes;
    for (std::size_t i = 0u; i < entries.size(); ++i) {
        const std::size_t slot = kArc20HeaderBytes + i * kArc20IndexEntryBytes;
        const auto& entry = entries[i];
        std::memcpy(arc.data() + slot, entry.name.data(), entry.name.size());
        WriteLe32(arc, slot + kArc20RelOffsetField, rel_offset);
        WriteLe32(arc, slot + kArc20SizeField,
                  static_cast<std::uint32_t>(entry.payload.size()));
        std::memcpy(arc.data() + slot + kArc20ExtraFieldOffset,
                    entry.extra.data(), kArc20ExtraFieldBytes);

        if (!entry.payload.empty()) {
            std::memcpy(arc.data() + data_start + rel_offset,
                        entry.payload.data(), entry.payload.size());
        }
        rel_offset += static_cast<std::uint32_t>(entry.payload.size());
    }
    return arc;
}

std::vector<SyntheticArcEntry> MakeThreeEntryArcSpec() {
    return {
        {
            "SGTitle990000",
            {0x00u, 0x7Fu, 0x80u, 0xFFu, 0x11u, 0x22u},
            {0x80u, 0x81u, 0x82u, 0x83u, 0x84u, 0x85u, 0x86u, 0x87u,
             0x88u, 0x89u, 0x8Au, 0x8Bu, 0x8Cu, 0x8Du, 0x8Eu, 0x8Fu,
             0x90u, 0x91u, 0x92u, 0x93u, 0x94u, 0x95u, 0x7Fu, 0xFFu},
        },
        {
            "att01",
            {0xDEu, 0xADu, 0xBEu, 0xEFu},
            {0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u, 0x70u, 0x80u,
             0x90u, 0xA0u, 0xB0u, 0xC0u, 0xD0u, 0xE0u, 0xF0u, 0x01u,
             0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u, 0x09u},
        },
        {
            "select01-1",
            {0x50u, 0x51u, 0x52u, 0x53u, 0x54u, 0x55u, 0x56u, 0x57u},
            {0xFFu, 0xFEu, 0xFDu, 0xFCu, 0xFBu, 0xFAu, 0xF9u, 0xF8u,
             0x77u, 0x66u, 0x55u, 0x44u, 0x33u, 0x22u, 0x11u, 0x00u,
             0xA5u, 0x5Au, 0xC3u, 0x3Cu, 0x96u, 0x69u, 0x80u, 0x7Fu},
        },
    };
}

TEST(MenuPatchFormatUt, MurmurHash2MatchesHandWorkedBlockTailAndSignBitVectors) {
    // Every expected 32-bit digest below is worked out by hand directly from
    // Austin Appleby's canonical LE MurmurHash2 specification (m = 0x5BD1E995,
    // r = 24, final shifts 13 and 15).
    constexpr std::array<std::uint8_t, kOneByte> kTail1 = {0x80u};
    constexpr std::array<std::uint8_t, kTwoBytes> kTail2 = {0x02u, 0x01u};
    constexpr std::array<std::uint8_t, kThreeBytes> kTail3 = {
        0x03u, 0x02u, 0x01u,
    };
    constexpr std::array<std::uint8_t, kFourBytes> kBlock4 = {
        0x01u, 0x00u, 0x00u, 0x00u,
    };
    constexpr std::array<std::uint8_t, kFiveBytes> kBlockPlusTail5 = {
        0x01u, 0x00u, 0x00u, 0x00u, 0x0Fu,
    };
    constexpr std::array<std::uint8_t, kFourBytes> kSignBitRamp = {
        0x00u, 0x7Fu, 0x80u, 0xFFu,
    };
    constexpr std::array<std::uint8_t, kFourBytes> kSignBitLowByte = {
        0x80u, 0x00u, 0x00u, 0x00u,
    };

    struct HashVector {
        const char* name;
        const std::uint8_t* bytes;
        std::size_t len;
        std::uint32_t seed;
        std::uint32_t expected;
    };

    const HashVector kVectors[] = {
        {"empty_seed0", nullptr, kEmptyBytes, kDefaultSeed, 0x00000000u},
        {"empty_seed1", nullptr, kEmptyBytes, kNonZeroSeed, 0x5BD15E36u},
        {"len1_sign_bit_tail", kTail1.data(), kTail1.size(), kDefaultSeed,
         0x9B891BB0u},
        {"len2_tail", kTail2.data(), kTail2.size(), kDefaultSeed, 0x4B25EC76u},
        {"len3_tail", kTail3.data(), kTail3.size(), kDefaultSeed, 0x3877533Bu},
        {"len4_exact_word", kBlock4.data(), kBlock4.size(), kDefaultSeed,
         0x27CC91B6u},
        {"len5_word_plus_tail", kBlockPlusTail5.data(), kBlockPlusTail5.size(),
         kDefaultSeed, 0xB8379726u},
        {"sign_bit_seam_00_7f_80_ff", kSignBitRamp.data(), kSignBitRamp.size(),
         kDefaultSeed, 0x26824341u},
        {"sign_bit_low_lane_80_00_00_00", kSignBitLowByte.data(),
         kSignBitLowByte.size(), kDefaultSeed, 0xA8F3ACFFu},
    };

    for (const auto& tc : kVectors) {
        SCOPED_TRACE(tc.name);
        EXPECT_EQ(MurmurHash2(tc.bytes, tc.len, tc.seed), tc.expected);
        EXPECT_EQ(MurmurHash2(ByteSpan(tc.bytes, tc.len), tc.seed),
                  tc.expected);
    }
}

TEST(MenuPatchFormatUt, ComputeXorMaskByteMatchesHandWorkedRotationAndSalt) {
    struct XorMaskCase {
        std::size_t index;
        std::uint8_t orig_byte;
        std::uint8_t expected_mask;
    };

    // Pinned against orig_slice_hash = 0xA1B2C3D4 (LE bytes D4, C3, B2, A1)
    // and salt progression 0x37, 0xD5, 0x73, 0x11, 0xAF across indices 0..4.
    constexpr XorMaskCase kCases[] = {
        {0u, 0x00u, 0xE3u},
        {1u, 0x7Fu, 0x69u},
        {2u, 0x80u, 0x41u},
        {3u, 0xFFu, 0x4Fu},
        {4u, 0x5Au, 0x21u},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.index);
        EXPECT_EQ(ComputeXorMaskByte(tc.orig_byte, kPinnedXorHash, tc.index),
                  tc.expected_mask);
    }
}

TEST(MenuPatchFormatUt, ValidatesRelativeAndTargetPathsAcrossSecuritySeams) {
    const std::string max_len_path(kMaxRelativePathBytes, 'a');
    const std::string over_max_path(kMaxRelativePathBytes + 1u, 'a');
    constexpr char kEmbeddedNulBuf[] = {'a', 't', 't', '\0', '0', '1'};
    constexpr std::uint8_t kOverlongSlash[] = {0xC0u, 0xAFu};
    constexpr std::uint8_t kOverlong3Byte[] = {0xE0u, 0x80u, 0x80u};
    constexpr std::uint8_t kUtf16Surrogate[] = {0xEDu, 0xA0u, 0x80u};
    constexpr std::uint8_t kTruncatedMultibyte[] = {0xE6u, 0xABu};

    const std::string overlong_2(
        reinterpret_cast<const char*>(kOverlongSlash), sizeof(kOverlongSlash));
    const std::string overlong_3(
        reinterpret_cast<const char*>(kOverlong3Byte), sizeof(kOverlong3Byte));
    const std::string surrogate(
        reinterpret_cast<const char*>(kUtf16Surrogate),
        sizeof(kUtf16Surrogate));
    const std::string truncated_utf8(
        reinterpret_cast<const char*>(kTruncatedMultibyte),
        sizeof(kTruncatedMultibyte));

    struct PathCase {
        const char* label;
        std::string_view path;
        bool expect_valid_rel;
        bool expect_allowed_target;
    };

    const PathCase kCases[] = {
        {"len_0_empty", "", false, false},
        {"len_1_min", "a", true, true},
        {"len_240_max", max_len_path, true, true},
        {"len_241_over_max", over_max_path, false, false},
        {"embedded_nul",
         std::string_view(kEmbeddedNulBuf, sizeof(kEmbeddedNulBuf)), false,
         false},
        {"control_0x1f", "att\x1F", false, false},
        {"control_0x7f", "att\x7F", false, false},
        {"forward_slash", "sub/att01", false, false},
        {"leading_backslash", "\\att01", false, false},
        {"trailing_backslash", "att01\\", false, false},
        {"empty_segment", "sub\\\\att01", false, false},
        {"dot_segment", ".", false, false},
        {"dotdot_segment", "..", false, false},
        {"nested_dotdot", "sub\\..\\att01", false, false},
        {"nested_dot", "sub\\.\\att01", false, false},
        {"trailing_space", "att01 ", false, false},
        {"trailing_dot", "att01.", false, false},
        {"segment_trailing_space", "sub \\att01", false, false},
        {"drive_colon", "C:att01", false, false},
        {"wildcard_char", "att*01", false, false},
        {"dos_con_upper", "CON", false, false},
        {"dos_nul_with_ext", "sub\\NUL.txt", false, false},
        {"dos_com1_lower", "com1", false, false},
        {"dos_lpt9_ext", "LPT9.arc", false, false},
        {"dos_prefix_non_device", "CONSOLE", true, true},
        {"utf8_overlong_2byte", overlong_2, false, false},
        {"utf8_overlong_3byte", overlong_3, false, false},
        {"utf8_surrogate", surrogate, false, false},
        {"utf8_truncated", truncated_utf8, false, false},
        {"valid_japanese_subpath", "data\\サクラノ詩", true, true},
        {"allowed_sysgrp_arc", "sysgrp.arc", true, true},
        {"allowed_att01", "att01", true, true},
        {"allowed_select", "select01-1", true, true},
        {"forbidden_exe_lower", "BGI.exe", true, false},
        {"forbidden_exe_upper", "BGI.EXE", true, false},
        {"forbidden_dll_mixed", "winmm.Dll", true, false},
        {"forbidden_bak", "sysgrp.arc.bak", true, false},
        {"forbidden_backup_upper", "data02501.arc.BACKUP", true, false},
        {"forbidden_tmp", "sysgrp.arc.tmp", true, false},
        {"forbidden_patch_bin", "menu_patch.bin", true, false},
        {"forbidden_patch_bin_upper", "MENU_PATCH.BIN", true, false},
        {"forbidden_sentinel", "menu_patch.applied", true, false},
        {"forbidden_sentinel_mixed", "Menu_Patch.Applied", true, false},
        {"forbidden_tsv", "translations.tsv", true, false},
        {"forbidden_tsv_upper", "TRANSLATIONS.TSV", true, false},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(IsValidRelativePath(tc.path), tc.expect_valid_rel);
        EXPECT_EQ(IsAllowedTargetPath(tc.path), tc.expect_allowed_target);
    }
}

TEST(MenuPatchFormatUt, ValidatesArchiveEntryNameBoundaries) {
    const std::string max_entry(kMaxArchiveEntryNameBytes, 'E');
    const std::string over_max_entry(kMaxArchiveEntryNameBytes + 1u, 'E');
    constexpr char kEmbeddedNulEntry[] = {'a', 't', '\0', '1'};
    constexpr std::uint8_t kSurrogateBytes[] = {0xEDu, 0xA0u, 0x80u};
    const std::string surrogate_entry(
        reinterpret_cast<const char*>(kSurrogateBytes),
        sizeof(kSurrogateBytes));

    struct EntryCase {
        const char* label;
        std::string_view name;
        bool expected;
    };

    const EntryCase kCases[] = {
        {"len_0", "", false},
        {"len_1", "a", true},
        {"len_95_max", max_entry, true},
        {"len_96_over_max", over_max_entry, false},
        {"embedded_nul",
         std::string_view(kEmbeddedNulEntry, sizeof(kEmbeddedNulEntry)), false},
        {"control_byte", "SGTitle\x01", false},
        {"del_byte", "SGTitle\x7F", false},
        {"forward_slash", "dir/SGTitle990000", false},
        {"backslash", "dir\\SGTitle990000", false},
        {"single_dot", ".", false},
        {"double_dot", "..", false},
        {"invalid_utf8_surrogate", surrogate_entry, false},
        {"valid_sysgrp_entry", "SGTitle990000", true},
        {"valid_choice_entry", "select17-2", true},
        {"valid_utf8_japanese", "櫻の森", true},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(IsValidArchiveEntryName(tc.name), tc.expected);
    }
}

TEST(MenuPatchFormatUt, Utf8PathToWideEncodesSurrogatePairsAndClearsOnFailure) {
    std::wstring wide_out;
    ASSERT_TRUE(Utf8PathToWide("data\\サクラ🌸", wide_out));

    std::wstring expected = L"data\\サクラ";
    expected.push_back(kSakuraBlossomLeadSurrogate);
    expected.push_back(kSakuraBlossomTrailSurrogate);
    EXPECT_EQ(wide_out, expected);

    // Valid -> invalid -> valid seam: malformed UTF-8 (truncated 4-byte F0 9F 8C)
    // must fail and clear wide_out rather than leaving stale path characters.
    constexpr std::uint8_t kTruncated4Byte[] = {0xF0u, 0x9Fu, 0x8Cu};
    const std::string bad_utf8(
        reinterpret_cast<const char*>(kTruncated4Byte),
        sizeof(kTruncated4Byte));
    EXPECT_FALSE(Utf8PathToWide(bad_utf8, wide_out));
    EXPECT_TRUE(wide_out.empty());

    ASSERT_TRUE(Utf8PathToWide("sysgrp.arc", wide_out));
    EXPECT_EQ(wide_out, L"sysgrp.arc");
}

TEST(MenuPatchFormatUt,
     ManifestPinsWireLayoutAndRoundTripsArchiveAndStandaloneRecords) {
    constexpr std::size_t kExpectedRecordCount = 4u;

    MenuPatchRecordInput arc_shrink;
    arc_shrink.source_rel_path = "SYSGRP.ARC";
    arc_shrink.target_rel_path = "sysgrp.arc";
    arc_shrink.entry_name = "SGTitle990000";
    arc_shrink.orig_offset = 0x1000u;
    arc_shrink.orig_slice = MakePatternBytes(kMinSliceVecSize, 0x10u);
    arc_shrink.new_raw = MakePatternBytes(kShrunkPayloadBytes, 0x91u);
    arc_shrink.flags = kFlagArchiveEntry;

    MenuPatchRecordInput arc_grow;
    arc_grow.source_rel_path = "sysgrp.arc";
    arc_grow.target_rel_path = "sysgrp.arc";
    arc_grow.entry_name = "SGCnfgWnd990000";
    arc_grow.orig_offset = 0x2000u;
    arc_grow.orig_slice = MakePatternBytes(kMinSliceVecSize, 0x33u);
    arc_grow.new_raw = MakePatternBytes(kGrownPayloadBytes, 0xA5u);
    arc_grow.flags = kFlagArchiveEntry;

    MenuPatchRecordInput standalone_no_entry;
    standalone_no_entry.source_rel_path = "data02501.arc";
    standalone_no_entry.target_rel_path = "att01";
    standalone_no_entry.entry_name.clear();
    standalone_no_entry.orig_offset = 0x400u;
    standalone_no_entry.orig_slice = MakePatternBytes(kMinSliceVecSize, 0x55u);
    standalone_no_entry.new_raw = MakePatternBytes(kSmallPayloadBytes, 0xC2u);
    standalone_no_entry.flags = kFlagBackupStandalone;

    MenuPatchRecordInput standalone_with_entry;
    standalone_with_entry.source_rel_path = "data02502.arc";
    standalone_with_entry.target_rel_path = "select01-1";
    standalone_with_entry.entry_name = "select01-1";
    standalone_with_entry.orig_offset = 0x800u;
    standalone_with_entry.orig_slice =
        MakePatternBytes(kMinSliceVecSize, 0x77u);
    standalone_with_entry.new_raw =
        MakePatternBytes(kSmallPayloadBytes, 0xE4u);
    standalone_with_entry.flags = kFlagBackupStandalone;

    const std::vector<MenuPatchRecordInput> inputs = {
        arc_shrink, arc_grow, standalone_no_entry, standalone_with_entry,
    };

    std::vector<std::uint8_t> blob;
    ASSERT_EQ(SerializeMenuPatchManifest(inputs, blob), ManifestStatus::kOk);
    ASSERT_GT(blob.size(), kPatchHeaderBytes);

    const ByteSpan span(blob);
    EXPECT_EQ(std::memcmp(span.data(), kPatchMagic, kMagicBytes), 0);
    EXPECT_EQ(ReadLe32(span, kOffsetVersion), kFormatVersion);
    EXPECT_EQ(ReadLe32(span, kOffsetRecordCount),
              static_cast<std::uint32_t>(kExpectedRecordCount));
    for (std::size_t pos = kOffsetReserved;
         pos < kOffsetReserved + kPatchReservedBytes; ++pos) {
        EXPECT_EQ(span[pos], 0u);
    }

    // Pin record 0 wire offsets including entry_name and payload_offset.
    const std::size_t rec0_src_len_off = kPatchHeaderBytes;
    ASSERT_EQ(ReadLe16(span, rec0_src_len_off),
              static_cast<std::uint16_t>(arc_shrink.source_rel_path.size()));
    const std::size_t rec0_tgt_len_off =
        rec0_src_len_off + kU16Bytes + arc_shrink.source_rel_path.size();
    ASSERT_EQ(ReadLe16(span, rec0_tgt_len_off),
              static_cast<std::uint16_t>(arc_shrink.target_rel_path.size()));
    const std::size_t rec0_entry_len_off =
        rec0_tgt_len_off + kU16Bytes + arc_shrink.target_rel_path.size();
    ASSERT_EQ(ReadLe16(span, rec0_entry_len_off),
              static_cast<std::uint16_t>(arc_shrink.entry_name.size()));
    EXPECT_EQ(
        std::string_view(
            reinterpret_cast<const char*>(span.data() + rec0_entry_len_off +
                                          kU16Bytes),
            arc_shrink.entry_name.size()),
        arc_shrink.entry_name);

    const std::size_t expected_rec0_payload_off =
        rec0_entry_len_off + kU16Bytes + arc_shrink.entry_name.size() +
        kRecordFixedTailBytes;

    MenuPatchHeader parsed_header;
    ASSERT_EQ(ParseMenuPatchHeader(span.subspan(0u, kPatchHeaderBytes),
                                   parsed_header),
              ManifestStatus::kOk);
    EXPECT_EQ(parsed_header.version, kFormatVersion);
    EXPECT_EQ(parsed_header.record_count,
              static_cast<std::uint32_t>(kExpectedRecordCount));
    EXPECT_EQ(parsed_header.manifest_hash, ReadLe32(span, kOffsetManifestHash));

    MenuPatchManifest manifest;
    ASSERT_EQ(ParseMenuPatchManifest(span, manifest), ManifestStatus::kOk);
    ASSERT_EQ(manifest.records.size(), kExpectedRecordCount);
    EXPECT_EQ(manifest.records[0].payload_offset, expected_rec0_payload_off);

    for (std::size_t i = 0u; i < kExpectedRecordCount; ++i) {
        SCOPED_TRACE(i);
        const auto& rec = manifest.records[i];
        const auto& in = inputs[i];
        EXPECT_EQ(rec.source_rel_path, in.source_rel_path);
        EXPECT_EQ(rec.target_rel_path, in.target_rel_path);
        EXPECT_EQ(rec.entry_name, in.entry_name);
        EXPECT_EQ(rec.orig_offset, in.orig_offset);
        EXPECT_EQ(rec.orig_size,
                  static_cast<std::uint64_t>(in.orig_slice.size()));
        EXPECT_EQ(rec.new_size, static_cast<std::uint64_t>(in.new_raw.size()));
        EXPECT_EQ(rec.flags, in.flags);

        const ByteSpan xor_payload = span.subspan(
            rec.payload_offset, static_cast<std::size_t>(rec.new_size));
        EXPECT_FALSE(std::equal(xor_payload.begin(), xor_payload.end(),
                                in.new_raw.begin(), in.new_raw.end()));

        std::vector<std::uint8_t> unmasked;
        ASSERT_EQ(UnmaskRecordPayload(xor_payload, ByteSpan(in.orig_slice),
                                      rec.orig_slice_hash, rec.new_raw_hash,
                                      unmasked),
                  UnmaskStatus::kOk);
        EXPECT_EQ(unmasked, in.new_raw);
    }
}

TEST(MenuPatchFormatUt, ManifestRejectsMalformedHeadersAndRecordsWithExactCode) {
    std::vector<std::uint8_t> valid_blob;
    const MenuPatchRecordInput base_rec = MakeStandaloneRecordInput();
    ASSERT_EQ(SerializeMenuPatchManifest({base_rec}, valid_blob),
              ManifestStatus::kOk);

    // Header-level error codes on ParseMenuPatchHeader & ParseMenuPatchManifest.
    MenuPatchHeader hdr;
    MenuPatchManifest parsed;
    EXPECT_EQ(ParseMenuPatchHeader(ByteSpan(), hdr), ManifestStatus::kTooSmall);
    EXPECT_EQ(
        ParseMenuPatchManifest(
            ByteSpan(valid_blob).subspan(0u, kPatchHeaderBytes - 1u), parsed),
        ManifestStatus::kTooSmall);

    struct HeaderMutation {
        const char* label;
        void (*mutate)(std::vector<std::uint8_t>&);
        ManifestStatus expected;
    };

    const HeaderMutation kHeaderMutations[] = {
        {"bad_magic",
         [](std::vector<std::uint8_t>& b) { b[kMagicBytes - 1u] = '2'; },
         ManifestStatus::kBadMagic},
        {"version_zero",
         [](std::vector<std::uint8_t>& b) { WriteLe32(b, kOffsetVersion, 0u); },
         ManifestStatus::kUnsupportedVersion},
        {"version_two",
         [](std::vector<std::uint8_t>& b) { WriteLe32(b, kOffsetVersion, 2u); },
         ManifestStatus::kUnsupportedVersion},
        {"record_count_zero",
         [](std::vector<std::uint8_t>& b) {
             WriteLe32(b, kOffsetRecordCount, 0u);
         },
         ManifestStatus::kInvalidRecordCount},
        {"record_count_max_plus_one",
         [](std::vector<std::uint8_t>& b) {
             WriteLe32(b, kOffsetRecordCount, kMaxRecordCount + 1u);
         },
         ManifestStatus::kInvalidRecordCount},
        {"reserved_first_byte_nonzero",
         [](std::vector<std::uint8_t>& b) { b[kOffsetReserved] = 0x01u; },
         ManifestStatus::kNonZeroReserved},
        {"reserved_last_byte_nonzero",
         [](std::vector<std::uint8_t>& b) { b[kOffsetReservedLast] = 0x80u; },
         ManifestStatus::kNonZeroReserved},
    };

    for (const auto& tc : kHeaderMutations) {
        SCOPED_TRACE(tc.label);
        std::vector<std::uint8_t> mutated = valid_blob;
        tc.mutate(mutated);
        EXPECT_EQ(ParseMenuPatchHeader(ByteSpan(mutated), hdr), tc.expected);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(mutated), parsed),
                  tc.expected);
    }

    // Boundary check: kMaxRecordCount (4096) is accepted by ParseMenuPatchHeader.
    {
        std::vector<std::uint8_t> max_hdr = valid_blob;
        WriteLe32(max_hdr, kOffsetRecordCount, kMaxRecordCount);
        EXPECT_EQ(ParseMenuPatchHeader(
                      ByteSpan(max_hdr).subspan(0u, kPatchHeaderBytes), hdr),
                  ManifestStatus::kOk);
    }

    // Manifest hash mismatch, truncated record, trailing bytes, and >128 MiB
    // size bounds on wire.
    {
        std::vector<std::uint8_t> bad_hash = valid_blob;
        bad_hash.back() ^= 0x01u;
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(bad_hash), parsed),
                  ManifestStatus::kManifestHashMismatch);
    }
    {
        std::vector<std::uint8_t> truncated = valid_blob;
        truncated.pop_back();
        ResealManifestHash(truncated);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(truncated), parsed),
                  ManifestStatus::kTruncatedRecord);
    }
    {
        std::vector<std::uint8_t> trailing = valid_blob;
        trailing.push_back(0x00u);
        ResealManifestHash(trailing);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(trailing), parsed),
                  ManifestStatus::kTrailingBytes);
    }

    const std::size_t target_str_off =
        kPatchHeaderBytes + kU16Bytes + base_rec.source_rel_path.size() +
        kU16Bytes;
    const std::size_t fixed_fields_off =
        target_str_off + base_rec.target_rel_path.size() + kU16Bytes +
        base_rec.entry_name.size();
    const std::size_t orig_size_off = fixed_fields_off + kU64Bytes;
    const std::size_t new_size_off = orig_size_off + kU64Bytes + kU32Bytes;
    const std::size_t flags_off = new_size_off + kU64Bytes + kU32Bytes;
    {
        std::vector<std::uint8_t> bad_target = valid_blob;
        std::memcpy(bad_target.data() + target_str_off, "a.dll",
                    base_rec.target_rel_path.size());
        ResealManifestHash(bad_target);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(bad_target), parsed),
                  ManifestStatus::kInvalidPath);
    }
    {
        std::vector<std::uint8_t> small_orig = valid_blob;
        WriteLe64(small_orig, orig_size_off, kMinOrigSliceBytes - 1ULL);
        ResealManifestHash(small_orig);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(small_orig), parsed),
                  ManifestStatus::kOrigSliceSizeOutOfRange);

        std::vector<std::uint8_t> huge_orig = valid_blob;
        WriteLe64(huge_orig, orig_size_off, kMaxAssetPayloadBytes + 1ULL);
        ResealManifestHash(huge_orig);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(huge_orig), parsed),
                  ManifestStatus::kOrigSliceSizeOutOfRange);
    }
    {
        std::vector<std::uint8_t> zero_new = valid_blob;
        WriteLe64(zero_new, new_size_off, 0ULL);
        zero_new.resize(flags_off + kU32Bytes);
        ResealManifestHash(zero_new);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(zero_new), parsed),
                  ManifestStatus::kPayloadSizeOutOfRange);

        std::vector<std::uint8_t> huge_new = valid_blob;
        WriteLe64(huge_new, new_size_off, kMaxAssetPayloadBytes + 1ULL);
        ResealManifestHash(huge_new);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(huge_new), parsed),
                  ManifestStatus::kPayloadSizeOutOfRange);
    }
    {
        std::vector<std::uint8_t> bad_flags = valid_blob;
        WriteLe32(bad_flags, flags_off, kUnknownFlagBit);
        ResealManifestHash(bad_flags);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(bad_flags), parsed),
                  ManifestStatus::kUnsupportedFlags);

        WriteLe32(bad_flags, flags_off, kCombinedExclusiveFlags);
        ResealManifestHash(bad_flags);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(bad_flags), parsed),
                  ManifestStatus::kUnsupportedFlags);
    }

    // Record input validation and uniqueness checks on SerializeMenuPatchManifest
    // (including valid -> error -> valid output clearing on blob_out).
    EXPECT_EQ(SerializeMenuPatchManifest({}, valid_blob),
              ManifestStatus::kInvalidRecordCount);
    EXPECT_TRUE(valid_blob.empty());

    struct InputRejectCase {
        const char* label;
        std::vector<MenuPatchRecordInput> records;
        ManifestStatus expected;
    };

    MenuPatchRecordInput mismatch_arc = base_rec;
    mismatch_arc.source_rel_path = "sysgrp.arc";
    mismatch_arc.target_rel_path = "other.arc";
    mismatch_arc.entry_name = "SGTitle990000";
    mismatch_arc.flags = kFlagArchiveEntry;

    MenuPatchRecordInput empty_entry_arc = mismatch_arc;
    empty_entry_arc.target_rel_path = "sysgrp.arc";
    empty_entry_arc.entry_name.clear();

    MenuPatchRecordInput bad_entry_standalone = base_rec;
    bad_entry_standalone.entry_name = "bad/entry";

    MenuPatchRecordInput forbidden_target = base_rec;
    forbidden_target.target_rel_path = "winmm.dll";

    MenuPatchRecordInput orig_too_small = base_rec;
    orig_too_small.orig_slice = MakePatternBytes(kUndersizeOrigSliceBytes, 1u);

    MenuPatchRecordInput empty_payload = base_rec;
    empty_payload.new_raw.clear();

    MenuPatchRecordInput unknown_flag = base_rec;
    unknown_flag.flags = kUnknownFlagBit;

    MenuPatchRecordInput both_flags = base_rec;
    both_flags.source_rel_path = "sysgrp.arc";
    both_flags.target_rel_path = "sysgrp.arc";
    both_flags.entry_name = "SGTitle990000";
    both_flags.flags = kCombinedExclusiveFlags;

    MenuPatchRecordInput dup_standalone_upper = base_rec;
    dup_standalone_upper.target_rel_path = "ATT01";

    MenuPatchRecordInput arc_entry_lower = empty_entry_arc;
    arc_entry_lower.entry_name = "SGTitle990000";
    MenuPatchRecordInput arc_entry_upper = arc_entry_lower;
    arc_entry_upper.target_rel_path = "SYSGRP.ARC";
    arc_entry_upper.entry_name = "sgtitle990000";

    MenuPatchRecordInput standalone_on_sysgrp = base_rec;
    standalone_on_sysgrp.source_rel_path = "sysgrp.arc";
    standalone_on_sysgrp.target_rel_path = "sysgrp.arc";
    standalone_on_sysgrp.entry_name.clear();
    standalone_on_sysgrp.flags = kFlagBackupStandalone;

    const InputRejectCase kInputCases[] = {
        {"archive_source_target_mismatch",
         {mismatch_arc},
         ManifestStatus::kInvalidPath},
        {"archive_empty_entry_name",
         {empty_entry_arc},
         ManifestStatus::kInvalidPath},
        {"standalone_invalid_entry_name",
         {bad_entry_standalone},
         ManifestStatus::kInvalidPath},
        {"forbidden_dll_target",
         {forbidden_target},
         ManifestStatus::kInvalidPath},
        {"orig_slice_63_bytes",
         {orig_too_small},
         ManifestStatus::kOrigSliceSizeOutOfRange},
        {"new_payload_0_bytes",
         {empty_payload},
         ManifestStatus::kPayloadSizeOutOfRange},
        {"unknown_flag_0x04",
         {unknown_flag},
         ManifestStatus::kUnsupportedFlags},
        {"combined_0x01_0x02_flags",
         {both_flags},
         ManifestStatus::kUnsupportedFlags},
        {"duplicate_standalone_case_insensitive",
         {base_rec, dup_standalone_upper},
         ManifestStatus::kDuplicateTargetPath},
        {"duplicate_archive_entry_case_insensitive",
         {arc_entry_lower, arc_entry_upper},
         ManifestStatus::kDuplicateTargetPath},
        {"mixed_archive_then_standalone_same_arc",
         {arc_entry_lower, standalone_on_sysgrp},
         ManifestStatus::kDuplicateTargetPath},
        {"mixed_standalone_then_archive_same_arc",
         {standalone_on_sysgrp, arc_entry_lower},
         ManifestStatus::kDuplicateTargetPath},
    };

    for (const auto& tc : kInputCases) {
        SCOPED_TRACE(tc.label);
        ASSERT_EQ(SerializeMenuPatchManifest({base_rec}, valid_blob),
                  ManifestStatus::kOk);
        EXPECT_EQ(SerializeMenuPatchManifest(tc.records, valid_blob),
                  tc.expected);
        EXPECT_TRUE(valid_blob.empty());
    }
}

TEST(MenuPatchFormatUt, UnmaskRecordPayloadReportsExactErrorsAndClearsOutput) {
    const MenuPatchRecordInput rec = MakeStandaloneRecordInput();
    std::vector<std::uint8_t> blob;
    ASSERT_EQ(SerializeMenuPatchManifest({rec}, blob), ManifestStatus::kOk);

    MenuPatchManifest manifest;
    ASSERT_EQ(ParseMenuPatchManifest(ByteSpan(blob), manifest),
              ManifestStatus::kOk);
    ASSERT_EQ(manifest.records.size(), 1u);
    const MenuPatchRecord& wire_rec = manifest.records[0];
    const ByteSpan xor_payload = ByteSpan(blob).subspan(
        wire_rec.payload_offset, static_cast<std::size_t>(wire_rec.new_size));
    const ByteSpan orig_span(rec.orig_slice);

    std::vector<std::uint8_t> out;
    ASSERT_EQ(UnmaskRecordPayload(xor_payload, orig_span,
                                  wire_rec.orig_slice_hash,
                                  wire_rec.new_raw_hash, out),
              UnmaskStatus::kOk);
    EXPECT_EQ(out, rec.new_raw);

    EXPECT_EQ(UnmaskRecordPayload(
                  xor_payload, orig_span.subspan(0u, kUndersizeOrigSliceBytes),
                  wire_rec.orig_slice_hash, wire_rec.new_raw_hash, out),
              UnmaskStatus::kOrigSliceSizeOutOfRange);
    EXPECT_TRUE(out.empty());

    out = rec.new_raw;
    EXPECT_EQ(UnmaskRecordPayload(ByteSpan(), orig_span,
                                  wire_rec.orig_slice_hash,
                                  wire_rec.new_raw_hash, out),
              UnmaskStatus::kPayloadSizeOutOfRange);
    EXPECT_TRUE(out.empty());

    out = rec.new_raw;
    EXPECT_EQ(UnmaskRecordPayload(xor_payload, orig_span,
                                  wire_rec.orig_slice_hash ^ 1u,
                                  wire_rec.new_raw_hash, out),
              UnmaskStatus::kOrigHashMismatch);
    EXPECT_TRUE(out.empty());

    out = rec.new_raw;
    EXPECT_EQ(UnmaskRecordPayload(xor_payload, orig_span,
                                  wire_rec.orig_slice_hash,
                                  wire_rec.new_raw_hash ^ 1u, out),
              UnmaskStatus::kNewHashMismatch);
    EXPECT_TRUE(out.empty());

    // Recover cleanly on subsequent valid unmask call.
    ASSERT_EQ(UnmaskRecordPayload(xor_payload, orig_span,
                                  wire_rec.orig_slice_hash,
                                  wire_rec.new_raw_hash, out),
              UnmaskStatus::kOk);
    EXPECT_EQ(out, rec.new_raw);
}

TEST(MenuPatchFormatUt, AppliedSentinelPinsWireHeaderRoundTripsAndRejectsErrors) {
    AppliedSentinel sentinel;
    sentinel.manifest_hash = 0xCAFEBABEu;
    sentinel.patch_file_size = 0x0000000102030405ULL;
    sentinel.entries = {
        {"sysgrp.arc", kSampleArcSentinelSize},
        {"att01", kSampleSplashSentinelSize},
    };

    std::vector<std::uint8_t> wire;
    ASSERT_EQ(SerializeAppliedSentinel(sentinel, wire), SentinelStatus::kOk);
    ASSERT_GT(wire.size(), kMinSentinelFrameBytes);

    const ByteSpan span(wire);
    EXPECT_EQ(std::memcmp(span.data(), kSentinelMagic, kMagicBytes), 0);
    EXPECT_EQ(ReadLe32(span, kSentinelOffsetVersion), kFormatVersion);
    EXPECT_EQ(ReadLe32(span, kSentinelOffsetManifestHash),
              sentinel.manifest_hash);
    EXPECT_EQ(ReadLe64(span, kSentinelOffsetPatchSize),
              sentinel.patch_file_size);
    EXPECT_EQ(ReadLe32(span, kSentinelOffsetEntryCount),
              static_cast<std::uint32_t>(sentinel.entries.size()));

    AppliedSentinel parsed;
    ASSERT_EQ(ParseAppliedSentinel(span, parsed), SentinelStatus::kOk);
    EXPECT_EQ(parsed.manifest_hash, sentinel.manifest_hash);
    EXPECT_EQ(parsed.patch_file_size, sentinel.patch_file_size);
    ASSERT_EQ(parsed.entries.size(), sentinel.entries.size());
    for (std::size_t i = 0u; i < sentinel.entries.size(); ++i) {
        EXPECT_EQ(parsed.entries[i].rel_path, sentinel.entries[i].rel_path);
        EXPECT_EQ(parsed.entries[i].expected_new_size,
                  sentinel.entries[i].expected_new_size);
    }

    // Error paths on ParseAppliedSentinel & SerializeAppliedSentinel.
    EXPECT_EQ(ParseAppliedSentinel(
                  span.subspan(0u, kMinSentinelFrameBytes - 1u), parsed),
              SentinelStatus::kTooSmall);

    {
        std::vector<std::uint8_t> bad_hash = wire;
        bad_hash.back() ^= 0x80u;
        EXPECT_EQ(ParseAppliedSentinel(ByteSpan(bad_hash), parsed),
                  SentinelStatus::kHashMismatch);
    }
    {
        std::vector<std::uint8_t> bad_magic = wire;
        bad_magic[kMagicBytes - 1u] = '2';
        ResealSentinelTrailer(bad_magic);
        EXPECT_EQ(ParseAppliedSentinel(ByteSpan(bad_magic), parsed),
                  SentinelStatus::kBadMagic);
    }
    {
        std::vector<std::uint8_t> bad_ver = wire;
        WriteLe32(bad_ver, kSentinelOffsetVersion, 2u);
        ResealSentinelTrailer(bad_ver);
        EXPECT_EQ(ParseAppliedSentinel(ByteSpan(bad_ver), parsed),
                  SentinelStatus::kUnsupportedVersion);
    }
    {
        std::vector<std::uint8_t> zero_count = wire;
        WriteLe32(zero_count, kSentinelOffsetEntryCount, 0u);
        ResealSentinelTrailer(zero_count);
        EXPECT_EQ(ParseAppliedSentinel(ByteSpan(zero_count), parsed),
                  SentinelStatus::kInvalidEntryCount);

        std::vector<std::uint8_t> over_count = wire;
        WriteLe32(over_count, kSentinelOffsetEntryCount, kMaxRecordCount + 1u);
        ResealSentinelTrailer(over_count);
        EXPECT_EQ(ParseAppliedSentinel(ByteSpan(over_count), parsed),
                  SentinelStatus::kInvalidEntryCount);
    }
    {
        // Truncate second entry while keeping entry_count == 2 and valid hash.
        const std::size_t entry0_end = kSentinelHeaderBytes + kU16Bytes +
                                       sentinel.entries[0].rel_path.size() +
                                       kU64Bytes;
        std::vector<std::uint8_t> truncated(
            wire.begin(),
            wire.begin() + static_cast<std::ptrdiff_t>(
                               entry0_end + kSentinelTrailerHashBytes));
        ResealSentinelTrailer(truncated);
        EXPECT_EQ(ParseAppliedSentinel(ByteSpan(truncated), parsed),
                  SentinelStatus::kTruncatedEntry);
    }
    {
        // Extra byte between entries and trailer hash with entry_count == 2.
        std::vector<std::uint8_t> trailing = wire;
        trailing.insert(trailing.end() -
                            static_cast<std::ptrdiff_t>(
                                kSentinelTrailerHashBytes),
                        0x00u);
        ResealSentinelTrailer(trailing);
        EXPECT_EQ(ParseAppliedSentinel(ByteSpan(trailing), parsed),
                  SentinelStatus::kTrailingBytes);
    }

    {
        // Corrupt entry 0 path ("sysgrp.arc" -> "sysgrp/arc") and expected size
        // on wire to verify ParseAppliedSentinel kInvalidPath &
        // kInvalidExpectedSize.
        std::vector<std::uint8_t> wire_bad_path = wire;
        wire_bad_path[kSentinelHeaderBytes + kU16Bytes + 6u] = '/';
        ResealSentinelTrailer(wire_bad_path);
        EXPECT_EQ(ParseAppliedSentinel(ByteSpan(wire_bad_path), parsed),
                  SentinelStatus::kInvalidPath);

        std::vector<std::uint8_t> wire_zero_size = wire;
        const std::size_t entry0_size_off =
            kSentinelHeaderBytes + kU16Bytes + sentinel.entries[0].rel_path.size();
        WriteLe64(wire_zero_size, entry0_size_off, 0ULL);
        ResealSentinelTrailer(wire_zero_size);
        EXPECT_EQ(ParseAppliedSentinel(ByteSpan(wire_zero_size), parsed),
                  SentinelStatus::kInvalidExpectedSize);
    }

    AppliedSentinel bad_sentinel = sentinel;
    bad_sentinel.entries.clear();
    EXPECT_EQ(SerializeAppliedSentinel(bad_sentinel, wire),
              SentinelStatus::kInvalidEntryCount);

    bad_sentinel.entries = {{"..\\att01", kMinOrigSliceBytes}};
    EXPECT_EQ(SerializeAppliedSentinel(bad_sentinel, wire),
              SentinelStatus::kInvalidPath);

    bad_sentinel.entries = {{"att01", 0ULL}};
    EXPECT_EQ(SerializeAppliedSentinel(bad_sentinel, wire),
              SentinelStatus::kInvalidExpectedSize);

    bad_sentinel.entries = {{"att01", kMaxAssetPayloadBytes + 1ULL}};
    EXPECT_EQ(SerializeAppliedSentinel(bad_sentinel, wire),
              SentinelStatus::kInvalidExpectedSize);
}

TEST(MenuPatchFormatUt, FindArc20EntrySliceMatchesCaseInsensitivelyAndHardensOverflow) {
    const std::vector<SyntheticArcEntry> entries = MakeThreeEntryArcSpec();
    const std::vector<std::uint8_t> arc = BuildArc20Fixture(entries);
    const std::uint64_t data_start =
        static_cast<std::uint64_t>(kArc20HeaderBytes) +
        static_cast<std::uint64_t>(entries.size() * kArc20IndexEntryBytes);

    std::uint64_t offset = 0u;
    std::uint64_t size = 0u;
    ASSERT_EQ(FindArc20EntrySlice(ByteSpan(arc), "sgtitle990000", offset, size),
              ArcStatus::kOk);
    EXPECT_EQ(offset, data_start);
    EXPECT_EQ(size, static_cast<std::uint64_t>(entries[0].payload.size()));

    ASSERT_EQ(FindArc20EntrySlice(ByteSpan(arc), "SELECT01-1", offset, size),
              ArcStatus::kOk);
    EXPECT_EQ(offset,
              data_start +
                  static_cast<std::uint64_t>(entries[0].payload.size() +
                                             entries[1].payload.size()));
    EXPECT_EQ(size, static_cast<std::uint64_t>(entries[2].payload.size()));

    // Error paths must zero out_file_offset and out_size.
    constexpr std::uint64_t kSentinelOffset = 999u;
    constexpr std::uint64_t kSentinelSize = 777u;

    offset = kSentinelOffset;
    size = kSentinelSize;
    EXPECT_EQ(FindArc20EntrySlice(
                  ByteSpan(arc).subspan(0u, kArc20HeaderBytes - 1u), "att01",
                  offset, size),
              ArcStatus::kTooSmall);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(size, 0u);

    {
        std::vector<std::uint8_t> bad_magic = arc;
        bad_magic[kArc20MagicBytes - 2u] = '1';
        offset = kSentinelOffset;
        size = kSentinelSize;
        EXPECT_EQ(FindArc20EntrySlice(ByteSpan(bad_magic), "att01", offset,
                                      size),
                  ArcStatus::kBadMagic);
        EXPECT_EQ(offset, 0u);
        EXPECT_EQ(size, 0u);
    }

    // 32-bit count * 128 wraparound seam: 0x02000001u * 128u == 128u in 32-bit
    // arithmetic. Use a 200-byte buffer (> 16 + 128) so naive 32-bit bounds
    // math would falsely accept the truncated index.
    {
        constexpr std::size_t kWrapProbeBytes = 200u;
        constexpr std::uint32_t kOverflowEntryCount = 0x02000001u;
        std::vector<std::uint8_t> wrap_index(kWrapProbeBytes, 0u);
        std::memcpy(wrap_index.data(), kArc20Magic, kArc20MagicBytes);
        WriteLe32(wrap_index, kArc20CountOffset, kOverflowEntryCount);

        offset = kSentinelOffset;
        size = kSentinelSize;
        EXPECT_EQ(FindArc20EntrySlice(ByteSpan(wrap_index), "att01", offset,
                                      size),
                  ArcStatus::kTruncatedIndex);
        EXPECT_EQ(offset, 0u);
        EXPECT_EQ(size, 0u);
    }

    // 32-bit rel_offset + size wraparound seam: 0xFFFFFFF0u + 0x20u == 0x10u
    // in 32-bit arithmetic inside a 32-byte data region.
    {
        constexpr std::size_t kDataRegionBytes = 32u;
        constexpr std::uint32_t kWrapRelOffset = 0xFFFFFFF0u;
        constexpr std::uint32_t kWrapEntrySize = 0x20u;
        constexpr std::string_view kProbeName = "att01";
        std::vector<std::uint8_t> wrap_slice(
            kArc20HeaderBytes + kArc20IndexEntryBytes + kDataRegionBytes, 0u);
        std::memcpy(wrap_slice.data(), kArc20Magic, kArc20MagicBytes);
        WriteLe32(wrap_slice, kArc20CountOffset, 1u);
        std::memcpy(wrap_slice.data() + kArc20HeaderBytes, kProbeName.data(),
                    kProbeName.size());
        WriteLe32(wrap_slice, kArc20HeaderBytes + kArc20RelOffsetField,
                  kWrapRelOffset);
        WriteLe32(wrap_slice, kArc20HeaderBytes + kArc20SizeField,
                  kWrapEntrySize);

        offset = kSentinelOffset;
        size = kSentinelSize;
        EXPECT_EQ(FindArc20EntrySlice(ByteSpan(wrap_slice), kProbeName, offset,
                                      size),
                  ArcStatus::kEntryOutOfBounds);
        EXPECT_EQ(offset, 0u);
        EXPECT_EQ(size, 0u);
    }

    offset = kSentinelOffset;
    size = kSentinelSize;
    EXPECT_EQ(FindArc20EntrySlice(ByteSpan(arc), "missing_entry", offset, size),
              ArcStatus::kEntryNotFound);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(size, 0u);

    offset = kSentinelOffset;
    size = kSentinelSize;
    EXPECT_EQ(FindArc20EntrySlice(ByteSpan(arc), "", offset, size),
              ArcStatus::kEntryNotFound);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(size, 0u);
}

TEST(MenuPatchFormatUt,
     RepackArc20EntriesPreservesMetadataUpdatesOffsetsAndRejectsBadReplacements) {
    const std::vector<SyntheticArcEntry> orig_entries = MakeThreeEntryArcSpec();
    const std::vector<std::uint8_t> orig_arc = BuildArc20Fixture(orig_entries);

    const std::array<std::uint8_t, kThreeBytes> shrunk_entry0 = {
        0x80u, 0x00u, 0xFFu,
    };
    const std::array<std::uint8_t, kGrownArcEntryBytes> grown_entry2 = {
        0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u,
        0x07u, 0x08u, 0x09u, 0x80u, 0xFFu,
    };

    const std::vector<ArcEntryReplacement> replacements = {
        {"sgtitle990000", ByteSpan(shrunk_entry0)},
        {"SELECT01-1", ByteSpan(grown_entry2)},
    };

    std::vector<std::uint8_t> repacked;
    ASSERT_EQ(RepackArc20Entries(ByteSpan(orig_arc), replacements, repacked),
              ArcStatus::kOk);

    const std::size_t expected_data_start =
        kArc20HeaderBytes + orig_entries.size() * kArc20IndexEntryBytes;
    const std::size_t expected_total_bytes =
        expected_data_start + shrunk_entry0.size() +
        orig_entries[1].payload.size() + grown_entry2.size();
    ASSERT_EQ(repacked.size(), expected_total_bytes);

    const ByteSpan orig_span(orig_arc);
    const ByteSpan out_span(repacked);
    EXPECT_EQ(std::memcmp(out_span.data(), kArc20Magic, kArc20MagicBytes), 0);
    EXPECT_EQ(ReadLe32(out_span, kArc20CountOffset),
              static_cast<std::uint32_t>(orig_entries.size()));

    // Verify every entry's 96-byte name buffer and 24-byte extra[104..128)
    // metadata are preserved byte-identical to orig_arc.
    for (std::size_t i = 0u; i < orig_entries.size(); ++i) {
        SCOPED_TRACE(i);
        const std::size_t slot = kArc20HeaderBytes + i * kArc20IndexEntryBytes;
        EXPECT_EQ(std::memcmp(out_span.data() + slot, orig_span.data() + slot,
                              kArc20NameFieldBytes),
                  0);
        EXPECT_EQ(std::memcmp(out_span.data() + slot + kArc20ExtraFieldOffset,
                              orig_span.data() + slot + kArc20ExtraFieldOffset,
                              kArc20ExtraFieldBytes),
                  0);
    }

    // Verify recalculated contiguous rel_offset and size fields + payload data.
    const std::array<ByteSpan, kThreeArcEntries> expected_payloads = {
        ByteSpan(shrunk_entry0),
        ByteSpan(orig_entries[1].payload),
        ByteSpan(grown_entry2),
    };
    std::uint32_t expected_rel_offset = 0u;
    for (std::size_t i = 0u; i < expected_payloads.size(); ++i) {
        SCOPED_TRACE(i);
        const std::size_t slot = kArc20HeaderBytes + i * kArc20IndexEntryBytes;
        EXPECT_EQ(ReadLe32(out_span, slot + kArc20RelOffsetField),
                  expected_rel_offset);
        EXPECT_EQ(ReadLe32(out_span, slot + kArc20SizeField),
                  static_cast<std::uint32_t>(expected_payloads[i].size()));

        std::uint64_t found_off = 0u;
        std::uint64_t found_size = 0u;
        ASSERT_EQ(FindArc20EntrySlice(out_span, orig_entries[i].name, found_off,
                                      found_size),
                  ArcStatus::kOk);
        ASSERT_EQ(found_off,
                  static_cast<std::uint64_t>(expected_data_start) +
                      static_cast<std::uint64_t>(expected_rel_offset));
        ASSERT_EQ(found_size,
                  static_cast<std::uint64_t>(expected_payloads[i].size()));

        const ByteSpan actual_slice = out_span.subspan(
            static_cast<std::size_t>(found_off),
            static_cast<std::size_t>(found_size));
        EXPECT_TRUE(std::equal(actual_slice.begin(), actual_slice.end(),
                               expected_payloads[i].begin(),
                               expected_payloads[i].end()));
        expected_rel_offset +=
            static_cast<std::uint32_t>(expected_payloads[i].size());
    }

    // Duplicate replacement (case-insensitive) and missing entry must fail and
    // clear out_arc.
    const std::vector<ArcEntryReplacement> dup_replacements = {
        {"att01", ByteSpan(shrunk_entry0)},
        {"ATT01", ByteSpan(grown_entry2)},
    };
    EXPECT_EQ(RepackArc20Entries(orig_span, dup_replacements, repacked),
              ArcStatus::kDuplicateReplacement);
    EXPECT_TRUE(repacked.empty());

    repacked = orig_arc;
    const std::vector<ArcEntryReplacement> missing_replacements = {
        {"nonexistent_sprite", ByteSpan(shrunk_entry0)},
    };
    EXPECT_EQ(RepackArc20Entries(orig_span, missing_replacements, repacked),
              ArcStatus::kEntryNotFound);
    EXPECT_TRUE(repacked.empty());
}

}  // namespace
}  // namespace menu_patch_format
