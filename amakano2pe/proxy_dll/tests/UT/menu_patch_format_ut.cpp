// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — "AMK2MENU" v1 wire container parsing, canonical MurmurHash2 vectors,
// XOR unmasking, relative path validation, and applied-sentinel verification.

#include "menu_patch_format.h"

#include "menu_patch_test_encoder.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using menu_patch::AppliedSentinel;
using menu_patch::AppliedTarget;
using menu_patch::ByteView;
using menu_patch::IsForbiddenTarget;
using menu_patch::IsSafeRelativePath;
using menu_patch::MenuPatchHeader;
using menu_patch::MenuPatchManifest;
using menu_patch::MurmurHash2;
using menu_patch::ParseAppliedSentinel;
using menu_patch::ParseMenuPatchContainer;
using menu_patch::ParseMenuPatchHeader;
using menu_patch::ParseStatus;
using menu_patch::SerializeAppliedSentinel;
using menu_patch::UnmaskRecordPayload;
using menu_patch::UnmaskStatus;
using menu_patch_test::Bytes;
using menu_patch_test::EncodeContainer;
using menu_patch_test::PseudoRandomBytes;
using menu_patch_test::SpecEntry;

constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kHeaderVersionOffset = 8;
constexpr std::size_t kHeaderReservedOffset = 20;
constexpr std::size_t kLengthPrefixBytes = 2;
constexpr std::size_t kFlagsBytes = 1;
constexpr std::size_t kReservedFlagBytes = 3;
constexpr std::size_t kU64Bytes = 8;
constexpr std::size_t kU32Bytes = 4;
constexpr std::size_t kTrailingU32Fields = 4;
constexpr std::size_t kRecordFixedTailBytes =
    kReservedFlagBytes + kU64Bytes + kTrailingU32Fields * kU32Bytes;
constexpr std::size_t kNewRawHashFromTailEnd = kU32Bytes;
constexpr std::size_t kSingleByteTrim = 1;

constexpr std::size_t kSliceSize = 96;
constexpr std::size_t kTooSmallSliceSize = 63;
constexpr std::size_t kLargeNewRawSize = 1000;
constexpr std::size_t kSmallNewRawSize = 40;
constexpr std::uint32_t kSeedA = 0x1234u;
constexpr std::uint32_t kSeedB = 0x9876u;
constexpr std::uint32_t kSeedC = 0x5555u;
constexpr std::uint32_t kSeedD = 0xABCDu;
constexpr std::uint64_t kOrigOffset = 0x1234567890ull;
constexpr std::uint32_t kUnsupportedVersion = 2;
constexpr std::uint8_t kUnknownFlag = 0x02;
constexpr std::uint8_t kFlipMask = 0x01;
constexpr std::uint64_t kSentinelPatchSize = 123456789ull;
constexpr std::uint32_t kSentinelManifestHash = 0xDEADBEEFu;
constexpr std::uint64_t kSentinelTargetSizeA = 4096;
constexpr std::uint64_t kSentinelTargetSizeB = 77;
constexpr std::size_t kExpectedTwoRecords = 2;
constexpr std::size_t kEmbeddedNullPathLen = 13;

// Canonical 32-bit MurmurHash2 (m = 0x5BD1E995, r = 24, seed = 0) digests
// covering all 4 tail alignments (len % 4 == 0, 1, 2, 3).
constexpr std::uint32_t kExpectedHashEmpty = 0x00000000u;
constexpr std::uint32_t kExpectedHashTail1 = 0x92685F5Eu;  // "a"
constexpr std::uint32_t kExpectedHashTail2 = 0x1AA14063u;  // "ab"
constexpr std::uint32_t kExpectedHashTail3 = 0x13577C9Bu;  // "abc"
constexpr std::uint32_t kExpectedHashWord4 = 0x26873021u;  // "abcd"

SpecEntry LooseEntry() {
    SpecEntry e;
    e.source = "image01.int";
    e.target = "image\\sys_title.hg3";
    e.flags = 0;
    e.orig_offset = kOrigOffset;
    e.orig_slice = PseudoRandomBytes(kSliceSize, kSeedA);
    e.new_raw = PseudoRandomBytes(kLargeNewRawSize, kSeedB);
    return e;
}

SpecEntry StandaloneEntry() {
    SpecEntry e;
    e.source = "boot.hg3";
    e.target = "boot.hg3";
    e.flags = menu_patch_test::kSpecFlagBackupStandalone;
    e.orig_offset = 0;
    e.orig_slice = PseudoRandomBytes(kSliceSize, kSeedC);
    e.new_raw = PseudoRandomBytes(kSmallNewRawSize, kSeedD);
    return e;
}

ByteView ViewOf(const Bytes& bytes) {
    return ByteView{bytes.data(), bytes.size()};
}

ParseStatus Parse(const Bytes& container) {
    MenuPatchManifest manifest;
    return ParseMenuPatchContainer(ViewOf(container), manifest);
}

ParseStatus ParseSingle(const SpecEntry& entry) {
    return Parse(EncodeContainer({entry}));
}

std::size_t FirstRecordFlagsOffset(const SpecEntry& e) {
    return kHeaderSize + kLengthPrefixBytes + e.source.size() +
            kLengthPrefixBytes + e.target.size();
}

std::uint32_t HashAscii(std::string_view text) noexcept {
    return MurmurHash2(reinterpret_cast<const std::uint8_t*>(text.data()),
                       text.size());
}

TEST(MenuPatchFormat, MurmurHash2MatchesCanonicalKnownAnswerVectors) {
    EXPECT_EQ(MurmurHash2(nullptr, 0), kExpectedHashEmpty);
    EXPECT_EQ(HashAscii("a"), kExpectedHashTail1);
    EXPECT_EQ(HashAscii("ab"), kExpectedHashTail2);
    EXPECT_EQ(HashAscii("abc"), kExpectedHashTail3);
    EXPECT_EQ(HashAscii("abcd"), kExpectedHashWord4);
}

TEST(MenuPatchFormat,
     ContainerRoundTripsLooseAndStandaloneRecordsAndDetectsPayloadTampering) {
    const SpecEntry loose = LooseEntry();
    const SpecEntry standalone = StandaloneEntry();
    const Bytes container = EncodeContainer({loose, standalone});

    MenuPatchManifest manifest;
    ASSERT_EQ(ParseMenuPatchContainer(ViewOf(container), manifest),
              ParseStatus::kOk);
    ASSERT_EQ(manifest.records.size(), kExpectedTwoRecords);
    EXPECT_EQ(manifest.header.record_count, kExpectedTwoRecords);

    const auto& r0 = manifest.records[0];
    EXPECT_EQ(r0.source_rel_path, loose.source);
    EXPECT_EQ(r0.target_rel_path, loose.target);
    EXPECT_FALSE(menu_patch::IsBackupStandalone(r0));
    EXPECT_EQ(r0.orig_offset, kOrigOffset);
    EXPECT_EQ(r0.orig_size, kSliceSize);
    EXPECT_EQ(r0.new_raw_size, kLargeNewRawSize);

    const auto& r1 = manifest.records[kFlagsBytes];
    EXPECT_TRUE(menu_patch::IsBackupStandalone(r1));

    Bytes out0;
    ASSERT_EQ(UnmaskRecordPayload(ViewOf(container), r0,
                                  ViewOf(loose.orig_slice), out0),
              UnmaskStatus::kOk);
    EXPECT_EQ(out0, loose.new_raw);

    Bytes out1;
    ASSERT_EQ(UnmaskRecordPayload(ViewOf(container), r1,
                                  ViewOf(standalone.orig_slice), out1),
              UnmaskStatus::kOk);
    EXPECT_EQ(out1, standalone.new_raw);

    Bytes wrong_slice = loose.orig_slice;
    wrong_slice[0] ^= kFlipMask;
    Bytes rejected_out;
    EXPECT_EQ(UnmaskRecordPayload(ViewOf(container), r0, ViewOf(wrong_slice),
                                  rejected_out),
              UnmaskStatus::kOrigSliceHashMismatch);

    const Bytes short_slice(loose.orig_slice.begin(),
                            loose.orig_slice.end() - kSingleByteTrim);
    EXPECT_EQ(UnmaskRecordPayload(ViewOf(container), r0, ViewOf(short_slice),
                                  rejected_out),
              UnmaskStatus::kOrigSliceSizeMismatch);

    Bytes payload_tampered = container;
    payload_tampered[r0.payload_offset] ^= kFlipMask;
    EXPECT_EQ(UnmaskRecordPayload(ViewOf(payload_tampered), r0,
                                  ViewOf(loose.orig_slice), rejected_out),
              UnmaskStatus::kNewRawHashMismatch);
    EXPECT_TRUE(rejected_out.empty());
}

TEST(MenuPatchFormat,
     ContainerRejectsMalformedHeadersTruncatedRecordsAndManifestTampering) {
    const SpecEntry entry = LooseEntry();
    const Bytes good = EncodeContainer({entry});

    EXPECT_EQ(
        Parse(Bytes(good.begin(), good.begin() + kHeaderSize - kSingleByteTrim)),
        ParseStatus::kTooSmall);

    Bytes bad_magic = good;
    bad_magic[0] = 'X';
    EXPECT_EQ(Parse(bad_magic), ParseStatus::kBadMagic);

    Bytes bad_version = good;
    bad_version[kHeaderVersionOffset] =
        static_cast<std::uint8_t>(kUnsupportedVersion);
    EXPECT_EQ(Parse(bad_version), ParseStatus::kBadVersion);

    Bytes reserved_hdr = good;
    reserved_hdr[kHeaderReservedOffset] = kFlipMask;
    EXPECT_EQ(Parse(reserved_hdr), ParseStatus::kReservedNotZero);

    MenuPatchHeader header;
    EXPECT_EQ(ParseMenuPatchHeader(ViewOf(good), header), ParseStatus::kOk);
    EXPECT_EQ(header.record_count, kFlagsBytes);

    EXPECT_EQ(Parse(Bytes(good.begin(), good.end() - kSingleByteTrim)),
              ParseStatus::kTruncated);

    Bytes trailing = good;
    trailing.push_back(0);
    EXPECT_EQ(Parse(trailing), ParseStatus::kTrailingBytes);

    const std::size_t flags_offset = FirstRecordFlagsOffset(entry);
    Bytes unknown_flag = good;
    unknown_flag[flags_offset] = kUnknownFlag;
    EXPECT_EQ(Parse(unknown_flag), ParseStatus::kUnknownFlags);

    Bytes reserved_rec = good;
    reserved_rec[flags_offset + kFlagsBytes] = kFlipMask;
    EXPECT_EQ(Parse(reserved_rec), ParseStatus::kReservedNotZero);

    Bytes tampered = good;
    const std::size_t new_hash_offset = flags_offset + kFlagsBytes +
                                        kRecordFixedTailBytes -
                                        kNewRawHashFromTailEnd;
    tampered[new_hash_offset] ^= kFlipMask;
    EXPECT_EQ(Parse(tampered), ParseStatus::kManifestHashMismatch);

    SpecEntry small_slice = entry;
    small_slice.orig_slice = PseudoRandomBytes(kTooSmallSliceSize, kSeedA);
    EXPECT_EQ(ParseSingle(small_slice), ParseStatus::kOrigSliceTooSmall);

    SpecEntry empty_payload = entry;
    empty_payload.new_raw.clear();
    EXPECT_EQ(ParseSingle(empty_payload), ParseStatus::kEmptyPayload);
}

TEST(MenuPatchFormat,
     PathValidationRejectsTraversalForbiddenTargetsAndSentinelTampering) {
    EXPECT_TRUE(IsSafeRelativePath("image\\sys_title.hg3"));
    EXPECT_TRUE(IsSafeRelativePath("fes\\config_basic.fes"));
    EXPECT_TRUE(IsSafeRelativePath("config\\startup.xml"));
    EXPECT_TRUE(IsSafeRelativePath("export\\cs2.zt"));
    EXPECT_FALSE(IsSafeRelativePath("..\\cs2_evil.hg3"));
    EXPECT_FALSE(IsSafeRelativePath("image\\..\\..\\evil.hg3"));
    EXPECT_FALSE(IsSafeRelativePath("C:\\evil.hg3"));
    EXPECT_FALSE(IsSafeRelativePath("image/evil.hg3"));
    EXPECT_FALSE(IsSafeRelativePath("image\\\xC0\xAF.hg3"));
    EXPECT_FALSE(
        IsSafeRelativePath(std::string("image\\a\0b.hg3", kEmbeddedNullPathLen)));

    EXPECT_TRUE(IsForbiddenTarget("image01.int"));
    EXPECT_TRUE(IsForbiddenTarget("cs2.exe"));
    EXPECT_TRUE(IsForbiddenTarget("winmm.dll"));
    EXPECT_TRUE(IsForbiddenTarget("boot.hg3.bak"));
    EXPECT_TRUE(IsForbiddenTarget("menu_patch.bin"));
    EXPECT_TRUE(IsForbiddenTarget("menu_patch.applied"));
    EXPECT_FALSE(IsForbiddenTarget("image\\sys_title.hg3"));

    SpecEntry traversal = LooseEntry();
    traversal.target = "..\\cs2_evil.hg3";
    EXPECT_EQ(ParseSingle(traversal), ParseStatus::kUnsafePath);

    SpecEntry standalone_mismatch = StandaloneEntry();
    standalone_mismatch.target = "image\\boot.hg3";
    EXPECT_EQ(ParseSingle(standalone_mismatch), ParseStatus::kFlagPathMismatch);

    SpecEntry forbidden = LooseEntry();
    forbidden.target = "cs2.exe";
    EXPECT_EQ(ParseSingle(forbidden), ParseStatus::kForbiddenTarget);

    SpecEntry dup_first = LooseEntry();
    SpecEntry dup_second = LooseEntry();
    dup_second.target = "IMAGE\\SYS_TITLE.HG3";
    EXPECT_EQ(Parse(EncodeContainer({dup_first, dup_second})),
              ParseStatus::kDuplicateTarget);

    AppliedSentinel sentinel;
    sentinel.manifest_hash = kSentinelManifestHash;
    sentinel.patch_file_size = kSentinelPatchSize;
    sentinel.targets = {
        AppliedTarget{"image\\sys_title.hg3", kSentinelTargetSizeA},
        AppliedTarget{"boot.hg3", kSentinelTargetSizeB},
    };
    const Bytes bytes = SerializeAppliedSentinel(sentinel);

    AppliedSentinel parsed;
    ASSERT_TRUE(ParseAppliedSentinel(ViewOf(bytes), parsed));
    EXPECT_EQ(parsed.manifest_hash, sentinel.manifest_hash);
    EXPECT_EQ(parsed.patch_file_size, sentinel.patch_file_size);
    ASSERT_EQ(parsed.targets.size(), kExpectedTwoRecords);

    Bytes tampered = bytes;
    tampered[0] ^= kFlipMask;
    EXPECT_FALSE(ParseAppliedSentinel(ViewOf(tampered), parsed));
    EXPECT_FALSE(ParseAppliedSentinel(
        ByteView{bytes.data(), bytes.size() - kSingleByteTrim}, parsed));
}

}  // namespace
