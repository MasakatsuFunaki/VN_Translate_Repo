// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Specification-driven unit tests for Adieu's "ADIEUMEN" v1 container codec,
// "ADIEUAPL" v1 applied-sentinel codec, canonical 32-bit MurmurHash2, keyless
// XOR unmasking, and relative-path security validation.

#include "menu_patch_format.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINGDI_) || \
    defined(__wincodec_h__)
#error "menu_patch_format.h must remain free of Windows headers"
#endif

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

using menu_patch_format::AppliedSentinel;
using menu_patch_format::ByteSpan;
using menu_patch_format::ComputeXorMaskByte;
using menu_patch_format::IsAllowedTargetPath;
using menu_patch_format::IsValidRelativePath;
using menu_patch_format::kFlagBackupStandalone;
using menu_patch_format::kFormatVersion;
using menu_patch_format::kMaxAssetPayloadBytes;
using menu_patch_format::kMaxRecordCount;
using menu_patch_format::kMaxRelativePathBytes;
using menu_patch_format::kMinOrigSliceBytes;
using menu_patch_format::kPatchHeaderBytes;
using menu_patch_format::kSentinelHeaderBytes;
using menu_patch_format::kSentinelTrailerHashBytes;
using menu_patch_format::ManifestStatus;
using menu_patch_format::MenuPatchHeader;
using menu_patch_format::MenuPatchManifest;
using menu_patch_format::MenuPatchRecordInput;
using menu_patch_format::MurmurHash2;
using menu_patch_format::ParseAppliedSentinel;
using menu_patch_format::ParseMenuPatchHeader;
using menu_patch_format::ParseMenuPatchManifest;
using menu_patch_format::SentinelStatus;
using menu_patch_format::SerializeAppliedSentinel;
using menu_patch_format::SerializeMenuPatchManifest;
using menu_patch_format::UnmaskRecordPayload;
using menu_patch_format::UnmaskStatus;
using menu_patch_format::Utf8PathToWide;

void AppendLe16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8u) & 0xFFu));
}

void AppendLe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (unsigned int i = 0u; i < 4u; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8u)) & 0xFFu));
    }
}

void AppendLe64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (unsigned int i = 0u; i < 8u; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8u)) & 0xFFu));
    }
}

void WriteLe32At(std::vector<std::uint8_t>& buf,
                 std::size_t offset,
                 std::uint32_t v) {
    for (unsigned int i = 0u; i < 4u; ++i) {
        buf[offset + i] = static_cast<std::uint8_t>((v >> (i * 8u)) & 0xFFu);
    }
}

void WriteLe64At(std::vector<std::uint8_t>& buf,
                 std::size_t offset,
                 std::uint64_t v) {
    for (unsigned int i = 0u; i < 8u; ++i) {
        buf[offset + i] = static_cast<std::uint8_t>((v >> (i * 8u)) & 0xFFu);
    }
}

std::vector<std::uint8_t> MakeDeterministicOrigSlice(std::size_t len,
                                                     std::uint8_t seed_byte) {
    std::vector<std::uint8_t> bytes(len);
    for (std::size_t i = 0u; i < len; ++i) {
        bytes[i] = static_cast<std::uint8_t>((i * 29u + seed_byte) & 0xFFu);
    }
    return bytes;
}

}  // namespace

TEST(MenuPatchFormat, MurmurHash2MatchesHandCalculatedCanonicalVectors) {
    // Independent hand-computed 32-bit MurmurHash2 vectors (m=0x5BD1E995, r=24)
    // covering empty, seeded-empty, tail lengths 1..3 with sign-bit bytes
    // (0x80, 0xFF), 4-byte block boundary, and 8-byte "ADIEUMEN" magic.
    EXPECT_EQ(MurmurHash2(nullptr, 0u, 0u), 0x00000000u);
    EXPECT_EQ(MurmurHash2(nullptr, 0u, 1u), 0x5BD15E36u);

    constexpr std::uint8_t kTail1SignBit[] = {0x80u};
    EXPECT_EQ(MurmurHash2(ByteSpan(kTail1SignBit), 0u), 0x9B891BB0u);

    constexpr std::uint8_t kTail2[] = {0x01u, 0x02u};
    EXPECT_EQ(MurmurHash2(ByteSpan(kTail2), 0u), 0x281C8461u);

    constexpr std::uint8_t kTail3SignBit[] = {0x01u, 0x02u, 0xFFu};
    EXPECT_EQ(MurmurHash2(ByteSpan(kTail3SignBit), 0u), 0xB19A37BFu);

    constexpr std::uint8_t kFourZeroBytes[] = {0x00u, 0x00u, 0x00u, 0x00u};
    EXPECT_EQ(MurmurHash2(ByteSpan(kFourZeroBytes), 0u), 0xB469B2CCu);

    constexpr std::uint8_t kAdieuMagic[] = {
        'A', 'D', 'I', 'E', 'U', 'M', 'E', 'N',
    };
    EXPECT_EQ(MurmurHash2(ByteSpan(kAdieuMagic), 0u), 0x4006AB44u);
}

TEST(MenuPatchFormat, ComputeXorMaskByteMatchesPinnedBytePositionsAndWrap) {
    // Hand-calculated mask bytes for orig_slice_hash = 0xA1B2C3D4 across
    // indices 0..4 (verifying little-endian hash-byte rotation and index=4
    // wrap back to shift 0 without UB shift-by-32).
    constexpr std::uint32_t kPinnedOrigHash = 0xA1B2C3D4u;
    EXPECT_EQ(ComputeXorMaskByte(0x10u, kPinnedOrigHash, 0u), 0xF3u);
    EXPECT_EQ(ComputeXorMaskByte(0x20u, kPinnedOrigHash, 1u), 0x36u);
    EXPECT_EQ(ComputeXorMaskByte(0x30u, kPinnedOrigHash, 2u), 0xF1u);
    EXPECT_EQ(ComputeXorMaskByte(0x80u, kPinnedOrigHash, 3u), 0x30u);
    EXPECT_EQ(ComputeXorMaskByte(0xFFu, kPinnedOrigHash, 4u), 0x84u);
}

TEST(MenuPatchFormat, RelativePathAndAllowedTargetValidatorsEnforceBoundaries) {
    struct PathCase {
        std::string_view path;
        bool valid_rel;
        bool allowed_target;
        const char* reason;
    };

    const std::string max_len_path =
        "g00\\" + std::string(kMaxRelativePathBytes - 8u, 'a') + ".g00";
    const std::string over_max_path = max_len_path + "x";
    const std::string embedded_nul("g00\0ME_02.g00", 13u);

    const PathCase cases[] = {
        {"Gameexe.dat", true, true, "root Gameexe.dat"},
        {"g00\\ME_02.g00", true, true, "subdir g00 sprite"},
        {"g00\\_name_input_ok.g00", true, true, "underscore g00 sprite"},
        {"g00\\さよなら.g00", true, true, "valid UTF-8 Japanese filename"},
        {"a", true, true, "minimum 1-byte relative path"},
        {max_len_path, true, true, "exact 240-byte upper bound"},
        {"", false, false, "empty path"},
        {over_max_path, false, false, "241-byte path exceeds max"},
        {"\\g00\\ME_02.g00", false, false, "leading backslash"},
        {"/g00/ME_02.g00", false, false, "leading forward slash"},
        {"g00/ME_02.g00", false, false, "interior forward slash"},
        {"g00\\", false, false, "trailing backslash"},
        {"C:\\Gameexe.dat", false, false, "drive letter prefix"},
        {"Gameexe.dat:ads", false, false, "NTFS alternate data stream colon"},
        {"g00\\\\ME_02.g00", false, false, "empty path segment"},
        {".", false, false, "single dot segment"},
        {"..", false, false, "parent traversal root"},
        {"g00\\..\\Gameexe.dat", false, false, "interior parent traversal"},
        {"g00\\.\\ME_02.g00", false, false, "interior current-dir dot"},
        {embedded_nul, false, false, "embedded NUL byte"},
        {"g00\\\x1FME_02.g00", false, false, "ASCII control byte"},
        {"g00\\*.g00", false, false, "wildcard character"},
        {"g00\\ME_02.g00 ", false, false, "trailing space on segment"},
        {"g00\\ME_02.g00.", false, false, "trailing dot on segment"},
        {"CON", false, false, "DOS reserved device CON"},
        {"g00\\nul.g00", false, false, "DOS reserved device with extension"},
        {"g00\\com9.dat", false, false, "DOS reserved COM9 port"},
        {"\xC0\xAF.g00", false, false, "overlong UTF-8"},
        {"\xED\xA0\x80.g00", false, false, "UTF-8 surrogate half"},
        {"\xE3\x81", false, false, "truncated multi-byte UTF-8"},
        {"SiglusEngine.exe", true, false, "executable target forbidden"},
        {"winmm.dll", true, false, "DLL target forbidden"},
        {"Gameexe.dat.bak", true, false, "backup target forbidden"},
        {"g00\\ME_02.g00.tmp", true, false, "temp target forbidden"},
        {"menu_patch.bin", true, false, "patch bundle target forbidden"},
        {"MENU_PATCH.APPLIED", true, false, "sentinel target forbidden"},
    };

    for (const auto& tc : cases) {
        SCOPED_TRACE(tc.reason);
        EXPECT_EQ(IsValidRelativePath(tc.path), tc.valid_rel);
        EXPECT_EQ(IsAllowedTargetPath(tc.path), tc.allowed_target);
    }

    std::wstring wide;
    ASSERT_TRUE(Utf8PathToWide("g00\\さよなら.g00", wide));
    EXPECT_EQ(wide, L"g00\\さよなら.g00");
    EXPECT_FALSE(Utf8PathToWide("\xC0\xAF", wide));
    EXPECT_TRUE(wide.empty());
}

TEST(MenuPatchFormat, AdieumenWireLayoutMatchesExactSpecByteOffsetsAndUnmasks) {
    // Verify exact byte-by-byte wire layout from product_spec.md §3.2 on a
    // concrete 1-record container, then verify multi-record round-trip with
    // payload wrap-around (new_size > orig_size) and binary seam bytes.
    std::vector<std::uint8_t> orig_slice(kMinOrigSliceBytes, 0x00u);
    orig_slice[0] = 0x10u;
    orig_slice[1] = 0x20u;
    orig_slice[2] = 0x30u;
    orig_slice[3] = 0x80u;
    orig_slice[63] = 0xFFu;

    const std::vector<std::uint8_t> new_raw = {0x00u, 0x7Fu, 0x80u, 0xFFu};
    const std::uint32_t expected_orig_hash = MurmurHash2(ByteSpan(orig_slice));
    const std::uint32_t expected_new_hash = MurmurHash2(ByteSpan(new_raw));

    MenuPatchRecordInput input;
    input.source_rel_path = "a";
    input.target_rel_path = "b";
    input.orig_offset = 0x0102030405060708ULL;
    input.orig_slice = orig_slice;
    input.new_raw = new_raw;
    input.flags = kFlagBackupStandalone;

    std::vector<std::uint8_t> serialized;
    ASSERT_EQ(SerializeMenuPatchManifest({input}, serialized),
              ManifestStatus::kOk);

    // Expected total size:
    //   32 (header) + 2 + 1 (src) + 2 + 1 (dst) + 8 (orig_offset) + 8 (orig_size)
    //   + 4 (orig_hash) + 8 (new_size) + 4 (new_hash) + 4 (flags) + 4 (payload)
    //   = 78 bytes (payload starts at +0x4A = 74).
    ASSERT_EQ(serialized.size(), 78u);
    EXPECT_EQ(std::memcmp(serialized.data(), "ADIEUMEN", 8u), 0);
    // +0x08 version = 1, +0x0C record_count = 1
    EXPECT_EQ(serialized[0x08], 1u);
    EXPECT_EQ(serialized[0x0C], 1u);
    for (std::size_t r = 0x14u; r < 0x20u; ++r) {
        EXPECT_EQ(serialized[r], 0u) << "reserved byte at " << r;
    }
    // +0x20: [u16=1]['a'][u16=1]['b']
    EXPECT_EQ(serialized[0x20], 1u);
    EXPECT_EQ(serialized[0x21], 0u);
    EXPECT_EQ(serialized[0x22], static_cast<std::uint8_t>('a'));
    EXPECT_EQ(serialized[0x23], 1u);
    EXPECT_EQ(serialized[0x24], 0u);
    EXPECT_EQ(serialized[0x25], static_cast<std::uint8_t>('b'));
    // +0x26..+0x2D: orig_offset = 0x0102030405060708 LE
    EXPECT_EQ(serialized[0x26], 0x08u);
    EXPECT_EQ(serialized[0x2D], 0x01u);
    // +0x2E..+0x35: orig_size = 64 LE
    EXPECT_EQ(serialized[0x2E], 64u);
    EXPECT_EQ(serialized[0x2F], 0u);
    // +0x3A..+0x41: new_size = 4 LE
    EXPECT_EQ(serialized[0x3A], 4u);
    EXPECT_EQ(serialized[0x3B], 0u);
    // +0x46..+0x49: flags = 1 LE
    EXPECT_EQ(serialized[0x46], 1u);
    EXPECT_EQ(serialized[0x47], 0u);

    MenuPatchHeader hdr{};
    ASSERT_EQ(ParseMenuPatchHeader(ByteSpan(serialized.data(), kPatchHeaderBytes),
                                   hdr),
              ManifestStatus::kOk);
    EXPECT_EQ(hdr.version, kFormatVersion);
    EXPECT_EQ(hdr.record_count, 1u);
    EXPECT_EQ(hdr.manifest_hash,
              MurmurHash2(ByteSpan(serialized).subspan(kPatchHeaderBytes)));

    MenuPatchManifest parsed{};
    ASSERT_EQ(ParseMenuPatchManifest(ByteSpan(serialized), parsed),
              ManifestStatus::kOk);
    ASSERT_EQ(parsed.records.size(), 1u);
    const auto& rec = parsed.records[0];
    EXPECT_EQ(rec.source_rel_path, "a");
    EXPECT_EQ(rec.target_rel_path, "b");
    EXPECT_EQ(rec.orig_offset, 0x0102030405060708ULL);
    EXPECT_EQ(rec.orig_size, kMinOrigSliceBytes);
    EXPECT_EQ(rec.orig_slice_hash, expected_orig_hash);
    EXPECT_EQ(rec.new_size, 4u);
    EXPECT_EQ(rec.new_raw_hash, expected_new_hash);
    EXPECT_EQ(rec.flags, kFlagBackupStandalone);
    EXPECT_EQ(rec.payload_offset, 74u);

    std::vector<std::uint8_t> unmasked;
    ASSERT_EQ(UnmaskRecordPayload(
                  ByteSpan(serialized).subspan(rec.payload_offset,
                                               static_cast<std::size_t>(rec.new_size)),
                  ByteSpan(orig_slice), rec.orig_slice_hash, rec.new_raw_hash,
                  unmasked),
              UnmaskStatus::kOk);
    EXPECT_EQ(unmasked, new_raw);
}

TEST(MenuPatchFormat, RejectsMalformedTamperedAndOverflowingAdieumenContainers) {
    MenuPatchRecordInput valid_rec;
    valid_rec.source_rel_path = "g00\\ME_02.g00";
    valid_rec.target_rel_path = "g00\\ME_02.g00";
    valid_rec.orig_slice = MakeDeterministicOrigSlice(64u, 0x11u);
    valid_rec.new_raw = MakeDeterministicOrigSlice(150u, 0x93u);
    valid_rec.flags = kFlagBackupStandalone;

    std::vector<std::uint8_t> base_blob;
    ASSERT_EQ(SerializeMenuPatchManifest({valid_rec}, base_blob),
              ManifestStatus::kOk);

    MenuPatchManifest out{};

    // 1. Truncated header (< 32 bytes)
    EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(base_blob.data(), 31u), out),
              ManifestStatus::kTooSmall);

    // 2. Bad magic
    {
        auto bad = base_blob;
        bad[0] = 'X';
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(bad), out),
                  ManifestStatus::kBadMagic);
    }

    // 3. Unsupported versions (0 and 2)
    for (const std::uint32_t ver : {0u, 2u}) {
        auto bad = base_blob;
        WriteLe32At(bad, 0x08u, ver);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(bad), out),
                  ManifestStatus::kUnsupportedVersion);
    }

    // 4. Out-of-range record_count (0 and 4097)
    for (const std::uint32_t cnt : {0u, kMaxRecordCount + 1u}) {
        auto bad = base_blob;
        WriteLe32At(bad, 0x0Cu, cnt);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(bad), out),
                  ManifestStatus::kInvalidRecordCount);
    }

    // 5. Non-zero header reserved byte
    {
        auto bad = base_blob;
        bad[0x1Fu] = 0x01u;
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(bad), out),
                  ManifestStatus::kNonZeroReserved);
    }

    // 6. Manifest hash mismatch
    {
        auto bad = base_blob;
        bad.back() ^= 0x01u;
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(bad), out),
                  ManifestStatus::kManifestHashMismatch);
    }

    // Helper that re-signs manifest_hash so record-level validators are reached.
    auto resign = [](std::vector<std::uint8_t>& blob) {
        const std::uint32_t h =
            MurmurHash2(ByteSpan(blob).subspan(kPatchHeaderBytes));
        WriteLe32At(blob, 0x10u, h);
    };

    // In base_blob, src="g00\ME_02.g00" (13B) and dst="g00\ME_02.g00" (13B):
    // +0x20: u16(13) + 13B = 15B -> +0x2F
    // +0x2F: u16(13) + 13B = 15B -> +0x3E
    // +0x3E: orig_offset (8B), +0x46: orig_size (8B), +0x4E: orig_hash (4B),
    // +0x52: new_size (8B), +0x5A: new_hash (4B), +0x5E: flags (4B)
    constexpr std::size_t kOrigSizeOffset = 0x46u;
    constexpr std::size_t kNewSizeOffset = 0x52u;
    constexpr std::size_t kFlagsOffset = 0x5Eu;

    // 7. orig_size < 64, > 128 MiB, and 64-bit > 4 GiB (32-bit truncation trap)
    for (const std::uint64_t bad_orig_sz :
         {std::uint64_t{63u}, kMaxAssetPayloadBytes + 1u,
          0x0000000100000040ULL}) {
        auto bad = base_blob;
        WriteLe64At(bad, kOrigSizeOffset, bad_orig_sz);
        resign(bad);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(bad), out),
                  ManifestStatus::kOrigSliceSizeOutOfRange);
    }

    // 8. new_size == 0, > 128 MiB, and 64-bit > 4 GiB (32-bit truncation trap)
    for (const std::uint64_t bad_new_sz :
         {std::uint64_t{0u}, kMaxAssetPayloadBytes + 1u,
          0x0000000100000096ULL}) {
        auto bad = base_blob;
        WriteLe64At(bad, kNewSizeOffset, bad_new_sz);
        resign(bad);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(bad), out),
                  ManifestStatus::kPayloadSizeOutOfRange);
    }

    // 9. Unknown flag bit
    {
        auto bad = base_blob;
        WriteLe32At(bad, kFlagsOffset, 0x02u);
        resign(bad);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(bad), out),
                  ManifestStatus::kUnsupportedFlags);
    }

    // 10. Truncated payload & trailing garbage byte
    {
        auto truncated = base_blob;
        truncated.pop_back();
        resign(truncated);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(truncated), out),
                  ManifestStatus::kTruncatedRecord);

        auto trailing = base_blob;
        trailing.push_back(0xAAu);
        resign(trailing);
        EXPECT_EQ(ParseMenuPatchManifest(ByteSpan(trailing), out),
                  ManifestStatus::kTrailingBytes);
    }

    // 11. Case-insensitive duplicate target paths
    {
        MenuPatchRecordInput dup = valid_rec;
        dup.target_rel_path = "G00\\me_02.g00";
        std::vector<std::uint8_t> dup_blob;
        EXPECT_EQ(SerializeMenuPatchManifest({valid_rec, dup}, dup_blob),
                  ManifestStatus::kDuplicateTargetPath);
    }
}

TEST(MenuPatchFormat, UnmaskRecordPayloadDetectsBitFlipsAndClearsOutputOnError) {
    const auto orig = MakeDeterministicOrigSlice(64u, 0x42u);
    const auto expected_new = MakeDeterministicOrigSlice(205u, 0x7Eu);

    MenuPatchRecordInput rec;
    rec.source_rel_path = "Gameexe.dat";
    rec.target_rel_path = "Gameexe.dat";
    rec.orig_slice = orig;
    rec.new_raw = expected_new;

    std::vector<std::uint8_t> blob;
    ASSERT_EQ(SerializeMenuPatchManifest({rec}, blob), ManifestStatus::kOk);

    MenuPatchManifest parsed{};
    ASSERT_EQ(ParseMenuPatchManifest(ByteSpan(blob), parsed),
              ManifestStatus::kOk);
    ASSERT_EQ(parsed.records.size(), 1u);
    const auto& r = parsed.records[0];
    const ByteSpan xor_span = ByteSpan(blob).subspan(
        r.payload_offset, static_cast<std::size_t>(r.new_size));

    std::vector<std::uint8_t> out = {0xDEu, 0xADu};

    // Valid unmask across >3 cycles of 64-byte orig_slice
    ASSERT_EQ(UnmaskRecordPayload(xor_span, ByteSpan(orig), r.orig_slice_hash,
                                  r.new_raw_hash, out),
              UnmaskStatus::kOk);
    EXPECT_EQ(out, expected_new);

    // 1-bit flip in orig_slice -> kOrigHashMismatch and output cleared
    auto corrupt_orig = orig;
    corrupt_orig[63] ^= 0x80u;
    EXPECT_EQ(UnmaskRecordPayload(xor_span, ByteSpan(corrupt_orig),
                                  r.orig_slice_hash, r.new_raw_hash, out),
              UnmaskStatus::kOrigHashMismatch);
    EXPECT_TRUE(out.empty());

    // 1-bit flip in xor_payload -> kNewHashMismatch and output cleared
    std::vector<std::uint8_t> corrupt_xor(xor_span.begin(), xor_span.end());
    corrupt_xor[204] ^= 0x01u;
    out = {0xBEu, 0xEFu};
    EXPECT_EQ(UnmaskRecordPayload(ByteSpan(corrupt_xor), ByteSpan(orig),
                                  r.orig_slice_hash, r.new_raw_hash, out),
              UnmaskStatus::kNewHashMismatch);
    EXPECT_TRUE(out.empty());

    // Undersized orig_slice (< 64 bytes) and empty payload
    EXPECT_EQ(UnmaskRecordPayload(xor_span, ByteSpan(orig.data(), 63u),
                                  r.orig_slice_hash, r.new_raw_hash, out),
              UnmaskStatus::kOrigSliceSizeOutOfRange);
    EXPECT_EQ(UnmaskRecordPayload(ByteSpan(), ByteSpan(orig), r.orig_slice_hash,
                                  r.new_raw_hash, out),
              UnmaskStatus::kPayloadSizeOutOfRange);
}

TEST(MenuPatchFormat, AppliedSentinelAdieuaplV1MatchesWireSpecAndRejectsTamper) {
    AppliedSentinel sentinel;
    sentinel.manifest_hash = 0x12345678u;
    sentinel.patch_file_size = 0x0000000000ABCDEFULL;
    sentinel.entries = {
        {"Gameexe.dat", 9876ULL},
        {"g00\\ME_02.g00", 54321ULL},
    };

    std::vector<std::uint8_t> raw;
    ASSERT_EQ(SerializeAppliedSentinel(sentinel, raw), SentinelStatus::kOk);

    // Verify exact header layout from product_spec.md §3.3:
    // +0x00 "ADIEUAPL", +0x08 version=1, +0x0C manifest_hash, +0x10 patch_size,
    // +0x18 entry_count=2, +0x1C entries..., trailing u32 sentinel_hash.
    ASSERT_GE(raw.size(), kSentinelHeaderBytes + kSentinelTrailerHashBytes);
    EXPECT_EQ(std::memcmp(raw.data(), "ADIEUAPL", 8u), 0);
    EXPECT_EQ(raw[0x08], 1u);
    EXPECT_EQ(raw[0x0C], 0x78u);
    EXPECT_EQ(raw[0x0F], 0x12u);
    EXPECT_EQ(raw[0x10], 0xEFu);
    EXPECT_EQ(raw[0x11], 0xCDu);
    EXPECT_EQ(raw[0x12], 0xABu);
    EXPECT_EQ(raw[0x18], 2u);

    const std::size_t trailer_pos = raw.size() - kSentinelTrailerHashBytes;
    const std::uint32_t expected_trailer =
        MurmurHash2(ByteSpan(raw.data(), trailer_pos), 0u);
    std::uint32_t actual_trailer = 0u;
    for (unsigned int i = 0u; i < 4u; ++i) {
        actual_trailer |= static_cast<std::uint32_t>(raw[trailer_pos + i])
                          << (i * 8u);
    }
    EXPECT_EQ(actual_trailer, expected_trailer);

    AppliedSentinel decoded{};
    ASSERT_EQ(ParseAppliedSentinel(ByteSpan(raw), decoded), SentinelStatus::kOk);
    EXPECT_EQ(decoded.manifest_hash, sentinel.manifest_hash);
    EXPECT_EQ(decoded.patch_file_size, sentinel.patch_file_size);
    ASSERT_EQ(decoded.entries.size(), 2u);
    EXPECT_EQ(decoded.entries[0].rel_path, "Gameexe.dat");
    EXPECT_EQ(decoded.entries[0].expected_new_size, 9876ULL);
    EXPECT_EQ(decoded.entries[1].rel_path, "g00\\ME_02.g00");
    EXPECT_EQ(decoded.entries[1].expected_new_size, 54321ULL);

    // Tamper checks
    EXPECT_EQ(ParseAppliedSentinel(ByteSpan(raw.data(), 27u), decoded),
              SentinelStatus::kTooSmall);

    {
        auto bad_magic = raw;
        bad_magic[7] = 'X';
        EXPECT_EQ(ParseAppliedSentinel(ByteSpan(bad_magic), decoded),
                  SentinelStatus::kBadMagic);
    }
    {
        auto bad_hash = raw;
        bad_hash[0x0Cu] ^= 0x01u;
        EXPECT_EQ(ParseAppliedSentinel(ByteSpan(bad_hash), decoded),
                  SentinelStatus::kHashMismatch);
    }
    {
        auto bad_ver = raw;
        WriteLe32At(bad_ver, 0x08u, 2u);
        WriteLe32At(bad_ver, trailer_pos,
                    MurmurHash2(ByteSpan(bad_ver.data(), trailer_pos), 0u));
        EXPECT_EQ(ParseAppliedSentinel(ByteSpan(bad_ver), decoded),
                  SentinelStatus::kUnsupportedVersion);
    }
    {
        auto bad_cnt = raw;
        WriteLe32At(bad_cnt, 0x18u, 0u);
        WriteLe32At(bad_cnt, trailer_pos,
                    MurmurHash2(ByteSpan(bad_cnt.data(), trailer_pos), 0u));
        EXPECT_EQ(ParseAppliedSentinel(ByteSpan(bad_cnt), decoded),
                  SentinelStatus::kInvalidEntryCount);
    }
    {
        std::vector<std::uint8_t> handmade;
        handmade.insert(handmade.end(), {'A', 'D', 'I', 'E', 'U', 'A', 'P', 'L'});
        AppendLe32(handmade, 1u);
        AppendLe32(handmade, 0x11u);
        AppendLe64(handmade, 100ULL);
        AppendLe32(handmade, 1u);
        AppendLe16(handmade, 6u);
        handmade.insert(handmade.end(), {'.', '.', '\\', 'a', '.', 'g'});
        AppendLe64(handmade, 64ULL);
        AppendLe32(handmade, MurmurHash2(ByteSpan(handmade), 0u));
        EXPECT_EQ(ParseAppliedSentinel(ByteSpan(handmade), decoded),
                  SentinelStatus::kInvalidPath);
    }
}
