// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Component tests for proxy_dll/src/patcher_logic.{h,cpp} and
// proxy_dll/src/menu_autopatch.{h,cpp}: byte matching, NOP filling, relative
// branch encoding, EXTRA_VA_MIZUNA_CE hook/patch constants, and MIZMENU1 v1
// delta auto-patching.
#include "patcher_logic.h"

#include "menu_autopatch.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pl = patcher_logic;
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kAsciiWordSpt = 0x00747073;
constexpr std::uint32_t kNonAsciiWord = 0x82A082A2;
constexpr std::size_t kTestNopBufferSize = 32;
constexpr std::uintptr_t kSampleBranchOrigin = 0x004FF140;
constexpr std::uintptr_t kSampleBranchTarget = 0x00500000;
constexpr std::size_t kBranchInstrBytes = 5;
constexpr std::int32_t kExpectedRel32 =
    static_cast<std::int32_t>(kSampleBranchTarget - (kSampleBranchOrigin + kBranchInstrBytes));

constexpr std::size_t kMagicBytesLen = 8;
constexpr std::uint32_t kFormatVersionV1 = 1u;
constexpr std::uint32_t kByteShift1 = 8u;
constexpr std::uint32_t kByteShift2 = 16u;
constexpr std::uint32_t kByteShift3 = 24u;
constexpr std::uint32_t kByteMask = 0xFFu;
constexpr std::size_t kGpkTableEntryCount = 2;
constexpr std::size_t kGtbTableWordBytes = 4;
constexpr std::size_t kGtbWordsPerEntry = 3;
constexpr std::uint32_t kSingleRecordCount = 1u;
constexpr std::uint32_t kTargetGpkEntryIndex = 1u;
constexpr std::size_t kCorruptedLastByteBackOffset = 1;
constexpr std::uint8_t kCorruptXorMask = 0xFFu;

void AppendU16Le(std::vector<std::uint8_t>& buf, std::uint16_t val) {
    buf.push_back(static_cast<std::uint8_t>(val & kByteMask));
    buf.push_back(static_cast<std::uint8_t>((val >> kByteShift1) & kByteMask));
}

void AppendU32Le(std::vector<std::uint8_t>& buf, std::uint32_t val) {
    buf.push_back(static_cast<std::uint8_t>(val & kByteMask));
    buf.push_back(static_cast<std::uint8_t>((val >> kByteShift1) & kByteMask));
    buf.push_back(static_cast<std::uint8_t>((val >> kByteShift2) & kByteMask));
    buf.push_back(static_cast<std::uint8_t>((val >> kByteShift3) & kByteMask));
}

void WriteBytes(const fs::path& path, const std::vector<std::uint8_t>& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
}

std::vector<std::uint8_t> ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.is_open());
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> BuildGtb(const std::vector<std::uint32_t>& offsets) {
    std::vector<std::uint8_t> gtb;
    const auto count = static_cast<std::uint32_t>(offsets.size());
    AppendU32Le(gtb, count);
    for (std::uint32_t i = 0; i < count; ++i) {
        AppendU32Le(gtb, i * static_cast<std::uint32_t>(kGtbTableWordBytes));
    }
    for (std::uint32_t off : offsets) {
        AppendU32Le(gtb, off);
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        gtb.push_back(static_cast<std::uint8_t>('e'));
        gtb.push_back(static_cast<std::uint8_t>('0' + i));
        gtb.push_back(0);
        gtb.push_back(0);
    }
    return gtb;
}

}  // namespace

TEST(PatcherLogic, looks_like_ascii_identifies_printable_dwords) {
    EXPECT_TRUE(pl::LooksLikeAscii(kAsciiWordSpt));
    EXPECT_FALSE(pl::LooksLikeAscii(kNonAsciiWord));
    EXPECT_FALSE(pl::LooksLikeAscii(0));
}

TEST(PatcherLogic, bytes_match_and_fill_nops) {
    std::vector<std::uint8_t> buf(kTestNopBufferSize, 0x00);
    pl::FillNops(buf.data(), kTestNopBufferSize);
    for (std::uint8_t b : buf) {
        EXPECT_EQ(b, pl::kNopOpcode);
    }
    EXPECT_TRUE(pl::BytesMatch(buf.data(), buf.data(), kTestNopBufferSize));
    EXPECT_FALSE(pl::BytesMatch(nullptr, buf.data(), kTestNopBufferSize));
}

TEST(PatcherLogic, emit_rel_call_and_jmp_encode_exact_5_byte_branches) {
    std::uint8_t callBuf[pl::kRelBranchInstructionSize] = {};
    std::uint8_t* callEnd = pl::EmitRelCall32(callBuf, kSampleBranchTarget);
    EXPECT_EQ(callEnd, callBuf + pl::kRelBranchInstructionSize);
    EXPECT_EQ(callBuf[0], pl::kRelCallOpcode);

    std::uint8_t jmpBuf[pl::kRelBranchInstructionSize] = {};
    std::uint8_t* jmpEnd = pl::EmitRelJmp32(jmpBuf, kSampleBranchTarget);
    EXPECT_EQ(jmpEnd, jmpBuf + pl::kRelBranchInstructionSize);
    EXPECT_EQ(jmpBuf[0], pl::kRelJmpOpcode);

    EXPECT_EQ(pl::Rel32Offset(kSampleBranchOrigin + pl::kRelBranchInstructionSize,
                              kSampleBranchTarget),
                              kExpectedRel32);
}

TEST(PatcherLogic, mizuna_ce_rva_and_sentinel_constants_match_contract) {
    EXPECT_EQ(pl::kTextSplitterRva, 0x0FF140u);
    EXPECT_EQ(pl::kTextSplitterSentinelLen, 6u);
    EXPECT_EQ(pl::kTextSplitterHookLen, 9u);
    EXPECT_TRUE(pl::BytesMatch(pl::kTextSplitterPrologue,
                               pl::kTextSplitterSentinel,
                               pl::kTextSplitterSentinelLen));

    EXPECT_EQ(pl::kPrintMessageSentinelRva, 0x100D11u);
    EXPECT_EQ(pl::kPrintMessageSentinelLen, 10u);
    EXPECT_EQ(pl::kPatch1FontSizeRva, 0x100D17u);
    EXPECT_EQ(pl::kOrigFontSize, 0x18u);
    EXPECT_EQ(pl::kPatchedFontSize, 0x0Eu);
    EXPECT_EQ(pl::kPatch2NextYAddendRva, 0x100D45u);
    EXPECT_EQ(pl::kPatch3FxfFontSizeRva, 0x100D1Bu);
    EXPECT_EQ(pl::kPatch3FxfFontSizeNopLen, 28u);
    EXPECT_EQ(pl::kPatch4FxfNextYRva, 0x100DA8u);
    EXPECT_EQ(pl::kPatch4FxfNextYNopLen, 29u);
    EXPECT_EQ(pl::kPatch5FxfLNextYRva, 0x100E97u);
    EXPECT_EQ(pl::kPatch5FxfLNextYNopLen, 28u);
    EXPECT_EQ(pl::kPatch6SpaceDefaultRva, 0x100DD1u);
    EXPECT_EQ(pl::kPatch7FxfSpaceRva, 0x100DD5u);
    EXPECT_EQ(pl::kPatch7FxfSpaceNopLen, 28u);
    EXPECT_EQ(pl::kPatch8FxfNextXRva, 0x100D8Cu);
    EXPECT_EQ(pl::kPatch8FxfNextXNopLen, 28u);
    EXPECT_EQ(pl::kPatch9WindowMsgNumRva, 0x1010D5u);
    EXPECT_EQ(pl::kOrigWindowMsgNum, 0x04u);
    EXPECT_EQ(pl::kPatchedWindowMsgNum, 0x07u);
    EXPECT_EQ(pl::kPatch10FxfWindowMsgNumRva, 0x101104u);
    EXPECT_EQ(pl::kPatch10FxfWindowMsgNumNopLen, 28u);

    EXPECT_EQ(pl::kTextRenderCoreRva, 0x03B870u);
    EXPECT_EQ(pl::kAltTextRenderCoreRva, 0x02D480u);
    EXPECT_EQ(pl::kFontSetupRva, 0x03E890u);
    EXPECT_EQ(pl::kAltFontSetupRva, 0x031790u);
    EXPECT_EQ(pl::kFontMgrActiveFlagOffset, 0x130u);

    const std::uint8_t familyAPrologue[pl::kTextRenderCoreHookLen] = {
        0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xDC, 0x01, 0x00, 0x00};
    const std::uint8_t familyBPrologue[pl::kTextRenderCoreHookLen] = {
        0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xFC, 0x01, 0x00, 0x00};
    const std::uint8_t badPrologue[pl::kTextRenderCoreHookLen] = {
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x00, 0x00, 0x00};
    EXPECT_TRUE(pl::MatchesTextRenderCorePrologue(familyAPrologue));
    EXPECT_TRUE(pl::MatchesTextRenderCorePrologue(familyBPrologue));
    EXPECT_FALSE(pl::MatchesTextRenderCorePrologue(badPrologue));
    EXPECT_FALSE(pl::MatchesTextRenderCorePrologue(nullptr));
}

TEST(MenuAutopatch, MurmurHash2AndXorMaskPayloadRoundTrip) {
    const std::vector<std::uint8_t> orig = {'O', 'R', 'I', 'G', 'D', 'A', 'T', 'A'};
    const std::vector<std::uint8_t> patched = {'P', 'A', 'T', 'C', 'H', 'E', 'D', '!', '!'};
    const std::uint32_t orig_hash =
        menu_autopatch::ComputeMurmurHash2(orig.data(), orig.size());
    const std::uint32_t patched_hash =
        menu_autopatch::ComputeMurmurHash2(patched.data(), patched.size());
    EXPECT_NE(orig_hash, patched_hash);

    std::vector<std::uint8_t> buf = patched;
    menu_autopatch::XorMaskPayload(
        buf.data(), buf.size(), orig.data(), orig.size(), orig_hash);
    EXPECT_NE(buf, patched);

    menu_autopatch::XorMaskPayload(
        buf.data(), buf.size(), orig.data(), orig.size(), orig_hash);
    EXPECT_EQ(buf, patched);
}

TEST(MenuAutopatch, EnsureGameFilesPatchedInDirAppliesCreatesBackupsAndIsIdempotent) {
    const fs::path temp_dir =
        fs::temp_directory_path() / "mizuna_ce_ut_menu_autopatch_ok";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir);

    const std::vector<std::uint8_t> orig_fxf = {0x91, 0x8E, 0x93, 0x9A};
    const std::vector<std::uint8_t> patched_fxf = {0xB1, 0xAE, 0xB3, 0xBA, 0xBC};

    const std::vector<std::uint8_t> orig_e0 = {'E', 'N', 'T', 'R', 'Y', '_', '0'};
    const std::vector<std::uint8_t> orig_e1 = {'O', 'L', 'D', '_', 'E', 'N', 'T', 'R', 'Y', '1'};
    const std::vector<std::uint8_t> patched_e1 = {
        'N', 'E', 'W', '_', 'P', 'A', 'T', 'C', 'H', 'E', 'D', '_', 'E', '1'};

    std::vector<std::uint8_t> orig_gpk = orig_e0;
    orig_gpk.insert(orig_gpk.end(), orig_e1.begin(), orig_e1.end());

    std::vector<std::uint8_t> expected_gpk = orig_e0;
    expected_gpk.insert(expected_gpk.end(), patched_e1.begin(), patched_e1.end());

    const std::vector<std::uint8_t> orig_gtb =
        BuildGtb({0u, static_cast<std::uint32_t>(orig_e0.size())});
    const std::vector<std::uint8_t> expected_gtb =
        BuildGtb({0u, static_cast<std::uint32_t>(orig_e0.size())});
    ASSERT_EQ(orig_gtb.size(),
              static_cast<std::size_t>(1 + kGpkTableEntryCount * kGtbWordsPerEntry) *
                  kGtbTableWordBytes);

    WriteBytes(temp_dir / "init2" / "mainControl.fxf", orig_fxf);
    WriteBytes(temp_dir / "dwq" / "sys0.gtb", orig_gtb);
    WriteBytes(temp_dir / "dwq" / "sys0.gpk", orig_gpk);

    std::vector<std::uint8_t> patch_bin;
    const char kMagic[kMagicBytesLen + 1] = "MIZMENU1";
    patch_bin.insert(patch_bin.end(), kMagic, kMagic + kMagicBytesLen);
    const std::uint32_t orig_gtb_hash =
        menu_autopatch::ComputeMurmurHash2(orig_gtb.data(), orig_gtb.size());
    const std::uint32_t expected_gtb_hash =
        menu_autopatch::ComputeMurmurHash2(expected_gtb.data(), expected_gtb.size());
    AppendU32Le(patch_bin, kFormatVersionV1);
    AppendU32Le(patch_bin, kSingleRecordCount);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(orig_gpk.size()));
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(expected_gpk.size()));
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(orig_gtb.size()));
    AppendU32Le(patch_bin, orig_gtb_hash);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(expected_gtb.size()));
    AppendU32Le(patch_bin, expected_gtb_hash);
    AppendU32Le(patch_bin, kSingleRecordCount);

    const std::string rel_path = "init2/mainControl.fxf";
    AppendU16Le(patch_bin, static_cast<std::uint16_t>(rel_path.size()));
    patch_bin.insert(patch_bin.end(), rel_path.begin(), rel_path.end());
    const std::uint32_t orig_fxf_hash =
        menu_autopatch::ComputeMurmurHash2(orig_fxf.data(), orig_fxf.size());
    const std::uint32_t patched_fxf_hash =
        menu_autopatch::ComputeMurmurHash2(patched_fxf.data(), patched_fxf.size());
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(orig_fxf.size()));
    AppendU32Le(patch_bin, orig_fxf_hash);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(patched_fxf.size()));
    AppendU32Le(patch_bin, patched_fxf_hash);
    std::vector<std::uint8_t> enc_fxf = patched_fxf;
    menu_autopatch::XorMaskPayload(enc_fxf.data(), enc_fxf.size(),
                                   orig_fxf.data(), orig_fxf.size(), orig_fxf_hash);
    patch_bin.insert(patch_bin.end(), enc_fxf.begin(), enc_fxf.end());

    std::vector<std::uint8_t> enc_gtb = expected_gtb;
    menu_autopatch::XorMaskPayload(enc_gtb.data(), enc_gtb.size(),
                                   orig_gtb.data(), orig_gtb.size(), orig_gtb_hash);
    patch_bin.insert(patch_bin.end(), enc_gtb.begin(), enc_gtb.end());

    const std::uint32_t orig_e1_hash =
        menu_autopatch::ComputeMurmurHash2(orig_e1.data(), orig_e1.size());
    const std::uint32_t patched_e1_hash =
        menu_autopatch::ComputeMurmurHash2(patched_e1.data(), patched_e1.size());
    AppendU32Le(patch_bin, kTargetGpkEntryIndex);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(orig_e0.size()));
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(orig_e1.size()));
    AppendU32Le(patch_bin, orig_e1_hash);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(patched_e1.size()));
    AppendU32Le(patch_bin, patched_e1_hash);
    std::vector<std::uint8_t> enc_e1 = patched_e1;
    menu_autopatch::XorMaskPayload(enc_e1.data(), enc_e1.size(),
                                   orig_e1.data(), orig_e1.size(), orig_e1_hash);
    patch_bin.insert(patch_bin.end(), enc_e1.begin(), enc_e1.end());

    WriteBytes(temp_dir / "menu_patch.bin", patch_bin);

    EXPECT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(temp_dir));

    EXPECT_EQ(ReadBytes(temp_dir / "init2" / "mainControl.fxf"), patched_fxf);
    EXPECT_EQ(ReadBytes(temp_dir / "init2" / "mainControl.fxf.bak"), orig_fxf);
    EXPECT_EQ(ReadBytes(temp_dir / "dwq" / "sys0.gpk"), expected_gpk);
    EXPECT_EQ(ReadBytes(temp_dir / "dwq" / "sys0.gpk.bak"), orig_gpk);
    EXPECT_EQ(ReadBytes(temp_dir / "dwq" / "sys0.gtb"), expected_gtb);
    EXPECT_EQ(ReadBytes(temp_dir / "dwq" / "sys0.gtb.bak"), orig_gtb);

    EXPECT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(temp_dir));
    EXPECT_EQ(ReadBytes(temp_dir / "init2" / "mainControl.fxf.bak"), orig_fxf);
    EXPECT_EQ(ReadBytes(temp_dir / "dwq" / "sys0.gpk.bak"), orig_gpk);

    fs::remove_all(temp_dir, ec);
}

TEST(MenuAutopatch, CorruptionOrBadMagicLeavesOriginalFilesUntouched) {
    const fs::path temp_dir =
        fs::temp_directory_path() / "mizuna_ce_ut_menu_autopatch_fail";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir);

    const std::vector<std::uint8_t> orig_fxf = {0x10, 0x20, 0x30, 0x40};
    WriteBytes(temp_dir / "init2" / "mainControl.fxf", orig_fxf);

    const std::vector<std::uint8_t> bad_magic = {'B', 'A', 'D', 'M', 'A', 'G', 'I', 'C'};
    WriteBytes(temp_dir / "menu_patch.bin", bad_magic);
    EXPECT_FALSE(menu_autopatch::EnsureGameFilesPatchedInDir(temp_dir));
    EXPECT_EQ(ReadBytes(temp_dir / "init2" / "mainControl.fxf"), orig_fxf);
    EXPECT_FALSE(fs::exists(temp_dir / "init2" / "mainControl.fxf.bak"));

    std::vector<std::uint8_t> corrupt_patch;
    const char kMagic[kMagicBytesLen + 1] = "MIZMENU1";
    corrupt_patch.insert(corrupt_patch.end(), kMagic, kMagic + kMagicBytesLen);
    AppendU32Le(corrupt_patch, kFormatVersionV1);
    AppendU32Le(corrupt_patch, kSingleRecordCount);
    AppendU32Le(corrupt_patch, static_cast<std::uint32_t>(orig_fxf.size()));
    AppendU32Le(corrupt_patch, static_cast<std::uint32_t>(orig_fxf.size()));
    AppendU32Le(corrupt_patch, static_cast<std::uint32_t>(orig_fxf.size()));
    AppendU32Le(corrupt_patch, 0u);
    AppendU32Le(corrupt_patch, static_cast<std::uint32_t>(orig_fxf.size()));
    AppendU32Le(corrupt_patch, 0u);
    AppendU32Le(corrupt_patch, 0u);

    const std::string rel_path = "init2/mainControl.fxf";
    AppendU16Le(corrupt_patch, static_cast<std::uint16_t>(rel_path.size()));
    corrupt_patch.insert(corrupt_patch.end(), rel_path.begin(), rel_path.end());
    const std::uint32_t orig_hash =
        menu_autopatch::ComputeMurmurHash2(orig_fxf.data(), orig_fxf.size());
    const std::vector<std::uint8_t> patched_fxf = {0x50, 0x60, 0x70, 0x80};
    const std::uint32_t patched_hash =
        menu_autopatch::ComputeMurmurHash2(patched_fxf.data(), patched_fxf.size());
    AppendU32Le(corrupt_patch, static_cast<std::uint32_t>(orig_fxf.size()));
    AppendU32Le(corrupt_patch, orig_hash);
    AppendU32Le(corrupt_patch, static_cast<std::uint32_t>(patched_fxf.size()));
    AppendU32Le(corrupt_patch, patched_hash);
    std::vector<std::uint8_t> enc_fxf = patched_fxf;
    menu_autopatch::XorMaskPayload(enc_fxf.data(), enc_fxf.size(),
                                   orig_fxf.data(), orig_fxf.size(), orig_hash);
    enc_fxf[enc_fxf.size() - kCorruptedLastByteBackOffset] ^= kCorruptXorMask;
    corrupt_patch.insert(corrupt_patch.end(), enc_fxf.begin(), enc_fxf.end());
    corrupt_patch.insert(corrupt_patch.end(), orig_fxf.begin(), orig_fxf.end());

    WriteBytes(temp_dir / "menu_patch.bin", corrupt_patch);
    EXPECT_FALSE(menu_autopatch::EnsureGameFilesPatchedInDir(temp_dir));
    EXPECT_EQ(ReadBytes(temp_dir / "init2" / "mainControl.fxf"), orig_fxf);
    EXPECT_FALSE(fs::exists(temp_dir / "init2" / "mainControl.fxf.bak"));

    fs::remove_all(temp_dir, ec);
}
