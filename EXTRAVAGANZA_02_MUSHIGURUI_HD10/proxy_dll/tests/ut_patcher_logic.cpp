// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pure in-memory unit tests for proxy_dll/src/patcher_logic.{h,cpp} and
// proxy_dll/src/translator_logic.{h,cpp}: PE `.text` section bounds parsing
// (including VirtualSize vs SizeOfRawData), Enigma Protector SEH trap
// filtering, multi-edition profile detection & PrintMessage patching (HD10,
// HD10_Win7, Complete Edition), 9/12-byte ASLR trampoline construction, 22
// runtime UI fallback strings, full-string CP932 `#` sanitization, FXF
// windowTitle decoding, trailing-CRLF-normalized lookup parity, and MSGMENU1
// multi-archive XOR-delta auto-patching.
#include "menu_autopatch.h"
#include "patcher_logic.h"
#include "translator_logic.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace ma = menu_autopatch;
namespace pl = patcher_logic;
namespace tl = translator_logic;

namespace {

constexpr std::size_t kSyntheticPeHeaderSize = 0x1000;
constexpr std::uint32_t kSyntheticLfanew = 0x0080u;
constexpr std::uint16_t kDosMagicMz = 0x5A4Du;
constexpr std::uint32_t kPeMagicPe00 = 0x00004550u;
constexpr std::uint16_t kMachineI386 = 0x014Cu;
constexpr std::uint16_t kMachineAmd64 = 0x8664u;
constexpr std::uint16_t kPe32Magic = 0x010Bu;
constexpr std::uint16_t kPe32PlusMagic = 0x020Bu;
constexpr std::uint16_t kStandardOptHeaderSize = 0x00E0u;
constexpr std::uint16_t kTwoSections = 2u;

constexpr std::size_t kDosLfanewOffset = 0x3C;
constexpr std::size_t kPeSigLen = 4;
constexpr std::size_t kCoffHeaderLen = 20;
constexpr std::size_t kCoffNumSectionsOff = 2;
constexpr std::size_t kCoffTimeDateStampOff = 4;
constexpr std::size_t kCoffOptHeaderSizeOff = 16;
constexpr std::size_t kOptSizeOfImageOff = 0x38;
constexpr std::size_t kSecHeaderLen = 40;
constexpr std::size_t kSecVirtualSizeOff = 8;
constexpr std::size_t kSecVirtualAddrOff = 12;
constexpr std::size_t kSecSizeOfRawDataOff = 16;
constexpr std::size_t kSecCharacteristicsOff = 36;
constexpr std::uint32_t kImageScnMemReadData = 0x40000040u;

constexpr std::uint32_t kTestRawOnlySectionSize = 0x020000u;
constexpr std::uint32_t kTestSmallerVirtualSize = 0x018000u;
constexpr std::uint32_t kTestOversizedRawData = 0x500000u;
constexpr std::uint32_t kTestClampedSizeOfImage = 0x040000u;

constexpr std::uintptr_t kAslrBaseLow = 0x00330000u;
constexpr std::uintptr_t kAslrBaseMid = 0x00680000u;
constexpr std::uintptr_t kAslrBaseHighLow = 0x00C00000u;
constexpr std::uintptr_t kAslrBaseHighDll = 0x70000000u;

constexpr std::size_t kTestAslrImageBaseCount = 4u;
constexpr std::array<std::uintptr_t, kTestAslrImageBaseCount> kTestAslrImageBases = {{
    kAslrBaseLow,
    kAslrBaseMid,
    kAslrBaseHighLow,
    kAslrBaseHighDll,
}};

constexpr std::size_t kTestTrampolineAllocBaseCount = 2u;
constexpr std::array<std::uintptr_t, kTestTrampolineAllocBaseCount>
    kTestTrampolineAllocBases = {{
        0x03500000u,
        0x74500000u,
    }};

constexpr std::uint32_t kHd10EnigmaTrap1Rva = 0x2E4F41u;
constexpr std::uint32_t kHd10EnigmaTrap2Rva = 0x32F2D5u;
constexpr std::uint32_t kHd10EnigmaTrap3Rva = 0x32F300u;

constexpr std::uint32_t kHd10Win7EnigmaTrap1Rva = 0x2DDF41u;
constexpr std::uint32_t kHd10Win7EnigmaTrap2Rva = 0x3282D5u;
constexpr std::uint32_t kHd10Win7EnigmaTrap3Rva = 0x328300u;

constexpr std::uint32_t kCeEnigmaTrap1Rva = 0x2BEF41u;
constexpr std::uint32_t kCeEnigmaTrap2Rva = 0x3092D5u;
constexpr std::uint32_t kCeEnigmaTrap3Rva = 0x309300u;

constexpr std::uint32_t kSehAccessViolation = 0xC0000005u;
constexpr std::uint32_t kOutsideExeOffset = 0x02000000u;

constexpr std::uint32_t kShiftedRvaDelta = 0x001230u;
constexpr std::uint32_t kCeDecoyRenderCoreRva = 0x0133C0u;
constexpr std::uint32_t kDuplicateOffsetDelta = 0x002000u;
constexpr std::uint32_t kSampleAslrRelocImm32 = 0x71234567u;
constexpr std::uint32_t kRelocImm32VariantA = 0x6FEDCBA0u;
constexpr std::uint32_t kRelocImm32VariantB = 0x0045ABCDu;
constexpr std::uint8_t kFillerByte = 0xCCu;
constexpr std::uint8_t kCorruptByte = 0xFFu;
constexpr std::uint8_t kFxfXorByteMask = 0xFFu;

constexpr std::size_t kHookShapeVariantCount = 2;
constexpr std::size_t kMusicGalleryFallbackEntryCount = 18;
constexpr std::size_t kConfigNameFallbackIndex = 21;
constexpr int kMusicGalleryFontSize = 20;
constexpr int kMusicGallerySpace = 2;
constexpr int kConfigBodyFontSize = 30;
constexpr int kConfigNameFontSize = 24;
constexpr int kChoiceFontSize = 32;
constexpr int kSingleLinePositiveSpace = 1;
constexpr int kPatchedDialogueFontSize = 16;
constexpr int kPatchedDialogueSpace = -4;
constexpr int kFullMaxChars = 1000;
constexpr int kAnimatedChoiceMaxChars = 12;

constexpr int kSingleOverrideRowCount = 1;
constexpr int kExpectedSyntheticTsvRowCount = 5;
constexpr std::size_t kCp932DoubleByteCharLen = 2u;
constexpr std::size_t kCp932ColorTagBytes = 3;
constexpr std::size_t kEnigmaTrapCountPerEdition = 3;
constexpr std::size_t kTrapIntDiv0FirstIdx = 0;
constexpr std::size_t kTrapIntDiv0SecondIdx = 1;
constexpr std::size_t kTrapIllegalInsnIdx = 2;

void WriteU16Le(std::uint8_t* dst, std::uint16_t val) noexcept {
    std::memcpy(dst, &val, sizeof(val));
}

void WriteU32Le(std::uint8_t* dst, std::uint32_t val) noexcept {
    std::memcpy(dst, &val, sizeof(val));
}

std::int32_t ReadI32Le(const std::uint8_t* src) noexcept {
    std::int32_t val = 0;
    std::memcpy(&val, src, sizeof(val));
    return val;
}

std::string ToCp932(std::wstring_view wide_text) {
    std::string encoded;
    EXPECT_TRUE(tl::EncodeWideToCp932(wide_text, encoded));
    return encoded;
}

std::vector<std::uint8_t> EncodeXorFxf(std::string_view plain_fxf) {
    std::vector<std::uint8_t> encoded(plain_fxf.size());
    for (std::size_t i = 0; i < plain_fxf.size(); ++i) {
        encoded[i] = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(plain_fxf[i]) ^ kFxfXorByteMask);
    }
    return encoded;
}

bool ContainsUnvalidatedAsciiHash(const std::string& s) noexcept {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '#') {
            continue;
        }
        if (i + kCp932ColorTagBytes > s.size() ||
            !tl::HasCp932ColorPrefix(s.c_str() + i)) {
            return true;
        }
        i += kCp932ColorTagBytes - 1;
    }
    return false;
}

std::vector<std::uint8_t> BuildSyntheticPeHeader(
    std::uint32_t text_rva,
    std::uint32_t text_virtual_size,
    std::uint32_t size_of_image,
    std::uint32_t time_date_stamp,
    std::uint16_t machine = kMachineI386,
    std::uint16_t opt_magic = kPe32Magic,
    std::uint32_t sec0_chars = pl::kImageScnMemExecute,
    std::uint32_t text_raw_size = 0) {
    std::vector<std::uint8_t> hdr(kSyntheticPeHeaderSize, 0);
    WriteU16Le(hdr.data(), kDosMagicMz);
    WriteU32Le(hdr.data() + kDosLfanewOffset, kSyntheticLfanew);

    std::uint8_t* nt = hdr.data() + kSyntheticLfanew;
    WriteU32Le(nt, kPeMagicPe00);

    std::uint8_t* coff = nt + kPeSigLen;
    WriteU16Le(coff, machine);
    WriteU16Le(coff + kCoffNumSectionsOff, kTwoSections);
    WriteU32Le(coff + kCoffTimeDateStampOff, time_date_stamp);
    WriteU16Le(coff + kCoffOptHeaderSizeOff, kStandardOptHeaderSize);

    std::uint8_t* opt = coff + kCoffHeaderLen;
    WriteU16Le(opt, opt_magic);
    WriteU32Le(opt + kOptSizeOfImageOff, size_of_image);

    std::uint8_t* sec0 = opt + kStandardOptHeaderSize;
    WriteU32Le(sec0 + kSecVirtualSizeOff, text_virtual_size);
    WriteU32Le(sec0 + kSecVirtualAddrOff, text_rva);
    WriteU32Le(sec0 + kSecSizeOfRawDataOff, text_raw_size);
    WriteU32Le(sec0 + kSecCharacteristicsOff, sec0_chars);

    const std::uint32_t sec1_rva =
        text_rva + ((text_virtual_size > text_raw_size) ? text_virtual_size : text_raw_size);
    std::uint8_t* sec1 = sec0 + kSecHeaderLen;
    WriteU32Le(sec1 + kSecVirtualSizeOff, kSyntheticPeHeaderSize);
    WriteU32Le(sec1 + kSecVirtualAddrOff, sec1_rva);
    WriteU32Le(sec1 + kSecCharacteristicsOff, kImageScnMemReadData);

    return hdr;
}

void StampEditionIntoBuffer(std::vector<std::uint8_t>& image,
                            const pl::EditionProfile& profile,
                            std::uint32_t rva_shift,
                            std::uint32_t font_setup_reloc_imm32) {
    const std::uint32_t sentinel_rva = profile.sentinel_rva + rva_shift;
    std::memcpy(image.data() + sentinel_rva,
                profile.sentinel.data(),
                profile.sentinel.size());
    for (const pl::PatchDeltaSpec& patch : profile.patches) {
        std::memcpy(image.data() + sentinel_rva + patch.delta_from_sentinel,
                    patch.expected,
                    patch.len);
    }

    const std::uint32_t splitter_rva = profile.text_splitter_rva + rva_shift;
    std::memcpy(image.data() + splitter_rva,
                profile.text_splitter_prologue.data(),
                profile.text_splitter_hook_len);

    const std::uint32_t render_rva = profile.text_render_core_rva + rva_shift;
    std::memcpy(image.data() + render_rva,
                profile.text_render_core_prologue.data(),
                profile.text_render_core_hook_len);
    if (profile.has_render_core_secondary_anchor) {
        std::memcpy(image.data() + render_rva + pl::kRenderCoreSecondaryAnchorOffset,
                    profile.render_core_secondary_anchor.data(),
                    profile.render_core_secondary_anchor.size());
    }

    const std::uint32_t font_rva = profile.font_setup_rva + rva_shift;
    std::memcpy(image.data() + font_rva,
                profile.font_setup_head5.data(),
                profile.font_setup_head5.size());
    WriteU32Le(image.data() + font_rva + pl::kFontSetupRelocOffset,
               font_setup_reloc_imm32);
    std::memcpy(image.data() + font_rva + pl::kFontSetupTailOffset,
                profile.font_setup_tail6.data(),
                profile.font_setup_tail6.size());
}

std::vector<std::uint8_t> BuildSyntheticEditionImage(
    const pl::EditionProfile& profile,
    std::uint32_t rva_shift = 0,
    std::uint32_t font_setup_reloc_imm32 = kSampleAslrRelocImm32) {
    const std::size_t image_bytes =
        profile.expected_text_section.rva + profile.expected_text_section.virtual_size;
    std::vector<std::uint8_t> image(image_bytes, kFillerByte);

    const auto hdr = BuildSyntheticPeHeader(
        profile.expected_text_section.rva,
        profile.expected_text_section.virtual_size,
        profile.expected_size_of_image,
        profile.time_date_stamp);
    std::memcpy(image.data(), hdr.data(), hdr.size());

    if (profile.kind == pl::EditionKind::CompleteEdition) {
        std::memcpy(image.data() + kCeDecoyRenderCoreRva,
                    profile.text_render_core_prologue.data(),
                    profile.text_render_core_hook_len);
    }

    StampEditionIntoBuffer(image, profile, rva_shift, font_setup_reloc_imm32);
    return image;
}

}  // namespace

TEST(PatcherLogic, PeHeaderAndSehFilter_ParsesValidBoundsAndFiltersEnigmaTraps) {
    struct EditionTrapCase {
        const pl::EditionProfile* profile;
        std::array<std::uint32_t, kEnigmaTrapCountPerEdition> trap_rvas;
    };
    const std::array<EditionTrapCase, pl::kEditionCount> cases = {{
        {&pl::kHd10Profile,
         {kHd10EnigmaTrap1Rva, kHd10EnigmaTrap2Rva, kHd10EnigmaTrap3Rva}},
        {&pl::kHd10Win7Profile,
         {kHd10Win7EnigmaTrap1Rva, kHd10Win7EnigmaTrap2Rva, kHd10Win7EnigmaTrap3Rva}},
        {&pl::kCompleteEditionProfile,
         {kCeEnigmaTrap1Rva, kCeEnigmaTrap2Rva, kCeEnigmaTrap3Rva}},
    }};

    for (const EditionTrapCase& tc : cases) {
        const auto hdr = BuildSyntheticPeHeader(
            tc.profile->expected_text_section.rva,
            tc.profile->expected_text_section.virtual_size,
            tc.profile->expected_size_of_image,
            tc.profile->time_date_stamp);

        const auto bounds = pl::ParsePeTextSectionBounds(hdr.data(), hdr.size());
        ASSERT_TRUE(bounds.has_value()) << tc.profile->name;
        EXPECT_EQ(bounds->rva, tc.profile->expected_text_section.rva);
        EXPECT_EQ(bounds->virtual_size, tc.profile->expected_text_section.virtual_size);

        const auto layout = pl::ParsePeImageLayout(hdr.data(), hdr.size());
        ASSERT_TRUE(layout.has_value()) << tc.profile->name;
        EXPECT_EQ(layout->size_of_image, tc.profile->expected_size_of_image);
        EXPECT_EQ(layout->time_date_stamp, tc.profile->time_date_stamp);

        for (std::uintptr_t base : kTestAslrImageBases) {
            const std::uintptr_t text_start = base + bounds->rva;
            const std::uintptr_t text_last = base + bounds->rva + bounds->virtual_size - 1;
            const std::uintptr_t text_end = base + bounds->rva + bounds->virtual_size;

            EXPECT_FALSE(pl::IsAddressInsideSection(text_start - 1, base, *bounds));
            EXPECT_TRUE(pl::IsAddressInsideSection(text_start, base, *bounds));
            EXPECT_TRUE(pl::IsAddressInsideSection(text_last, base, *bounds));
            EXPECT_FALSE(pl::IsAddressInsideSection(text_end, base, *bounds));

            EXPECT_TRUE(pl::IsBenignPackerSehException(
                pl::kSehIntDivideByZero,
                base + tc.trap_rvas[kTrapIntDiv0FirstIdx],
                base,
                *layout));
            EXPECT_TRUE(pl::IsBenignPackerSehException(
                pl::kSehIntDivideByZero,
                base + tc.trap_rvas[kTrapIntDiv0SecondIdx],
                base,
                *layout));
            EXPECT_TRUE(pl::IsBenignPackerSehException(
                pl::kSehIllegalInstruction,
                base + tc.trap_rvas[kTrapIllegalInsnIdx],
                base,
                *layout));

            EXPECT_FALSE(pl::IsBenignPackerSehException(
                pl::kSehIntDivideByZero, text_start, base, *layout));
            EXPECT_FALSE(pl::IsBenignPackerSehException(
                kSehAccessViolation,
                base + tc.trap_rvas[kTrapIntDiv0FirstIdx],
                base,
                *layout));
            EXPECT_FALSE(pl::IsBenignPackerSehException(
                pl::kSehIllegalInstruction, base + kOutsideExeOffset, base, *layout));
        }
    }

    EXPECT_FALSE(pl::ParsePeTextSectionBounds(nullptr, kSyntheticPeHeaderSize).has_value());
    auto bad_mz = BuildSyntheticPeHeader(
        pl::kDefaultTextSectionRva,
        pl::kHd10TextVirtualSize,
        pl::kHd10SizeOfImage,
        pl::kHd10TimeDateStamp);
    bad_mz[0] = 'X';
    EXPECT_FALSE(pl::ParsePeTextSectionBounds(bad_mz.data(), bad_mz.size()).has_value());

    auto bad_pe = BuildSyntheticPeHeader(
        pl::kDefaultTextSectionRva,
        pl::kHd10TextVirtualSize,
        pl::kHd10SizeOfImage,
        pl::kHd10TimeDateStamp);
    bad_pe[kSyntheticLfanew] = 'Q';
    EXPECT_FALSE(pl::ParsePeTextSectionBounds(bad_pe.data(), bad_pe.size()).has_value());

    const auto x64_hdr = BuildSyntheticPeHeader(
        pl::kDefaultTextSectionRva,
        pl::kHd10TextVirtualSize,
        pl::kHd10SizeOfImage,
        pl::kHd10TimeDateStamp,
        kMachineAmd64,
        kPe32PlusMagic);
    EXPECT_FALSE(pl::ParsePeTextSectionBounds(x64_hdr.data(), x64_hdr.size()).has_value());

    const auto no_exec_hdr = BuildSyntheticPeHeader(
        pl::kDefaultTextSectionRva,
        pl::kHd10TextVirtualSize,
        pl::kHd10SizeOfImage,
        pl::kHd10TimeDateStamp,
        kMachineI386,
        kPe32Magic,
        kImageScnMemReadData);
    EXPECT_FALSE(pl::ParsePeTextSectionBounds(no_exec_hdr.data(), no_exec_hdr.size()).has_value());
}

TEST(PatcherLogic, PeHeader_RespectsSizeOfRawDataAndClampsToSizeOfImage) {
    // VirtualSize == 0 with non-zero SizeOfRawData resolves using SizeOfRawData.
    const auto raw_only_hdr = BuildSyntheticPeHeader(
        pl::kDefaultTextSectionRva,
        0u,
        pl::kHd10SizeOfImage,
        pl::kHd10TimeDateStamp,
        kMachineI386,
        kPe32Magic,
        pl::kImageScnMemExecute,
        kTestRawOnlySectionSize);
    const auto raw_only_bounds =
        pl::ParsePeTextSectionBounds(raw_only_hdr.data(), raw_only_hdr.size());
    ASSERT_TRUE(raw_only_bounds.has_value());
    EXPECT_EQ(raw_only_bounds->rva, pl::kDefaultTextSectionRva);
    EXPECT_EQ(raw_only_bounds->virtual_size, kTestRawOnlySectionSize);

    // VirtualSize < SizeOfRawData resolves using max(VirtualSize, SizeOfRawData).
    const auto smaller_virt_hdr = BuildSyntheticPeHeader(
        pl::kDefaultTextSectionRva,
        kTestSmallerVirtualSize,
        pl::kHd10SizeOfImage,
        pl::kHd10TimeDateStamp,
        kMachineI386,
        kPe32Magic,
        pl::kImageScnMemExecute,
        kTestRawOnlySectionSize);
    const auto smaller_virt_bounds =
        pl::ParsePeTextSectionBounds(smaller_virt_hdr.data(), smaller_virt_hdr.size());
    ASSERT_TRUE(smaller_virt_bounds.has_value());
    EXPECT_EQ(smaller_virt_bounds->virtual_size, kTestRawOnlySectionSize);

    // Oversized SizeOfRawData clamps cleanly to SizeOfImage - VirtualAddress.
    const auto clamped_hdr = BuildSyntheticPeHeader(
        pl::kDefaultTextSectionRva,
        0u,
        kTestClampedSizeOfImage,
        pl::kHd10TimeDateStamp,
        kMachineI386,
        kPe32Magic,
        pl::kImageScnMemExecute,
        kTestOversizedRawData);
    const auto clamped_bounds =
        pl::ParsePeTextSectionBounds(clamped_hdr.data(), clamped_hdr.size());
    ASSERT_TRUE(clamped_bounds.has_value());
    EXPECT_EQ(clamped_bounds->virtual_size,
              kTestClampedSizeOfImage - pl::kDefaultTextSectionRva);

    // Both VirtualSize == 0 and SizeOfRawData == 0 must be rejected.
    const auto empty_sec_hdr = BuildSyntheticPeHeader(
        pl::kDefaultTextSectionRva,
        0u,
        pl::kHd10SizeOfImage,
        pl::kHd10TimeDateStamp,
        kMachineI386,
        kPe32Magic,
        pl::kImageScnMemExecute,
        0u);
    EXPECT_FALSE(
        pl::ParsePeTextSectionBounds(empty_sec_hdr.data(), empty_sec_hdr.size()).has_value());
}

TEST(PatcherLogic, EditionProfiles_MatchExpectedRvasAndApplyAllTenPatches) {
    for (const pl::EditionProfile& profile : pl::kEditionProfiles) {
        auto image = BuildSyntheticEditionImage(profile, 0, kRelocImm32VariantA);
        const auto layout = pl::ParsePeImageLayout(image.data(), pl::kDefaultPeHeaderMaxBytes);
        ASSERT_TRUE(layout.has_value()) << profile.name;

        const auto resolved = pl::ResolveEditionTargets(
            image.data(), layout->text_section, layout->time_date_stamp);
        ASSERT_TRUE(resolved.has_value()) << profile.name;
        EXPECT_EQ(resolved->profile->kind, profile.kind);
        EXPECT_FALSE(resolved->used_scan_fallback);
        EXPECT_EQ(resolved->sentinel_rva, profile.sentinel_rva);
        EXPECT_EQ(resolved->text_splitter_rva, profile.text_splitter_rva);
        EXPECT_EQ(resolved->text_render_core_rva, profile.text_render_core_rva);
        EXPECT_EQ(resolved->font_setup_rva, profile.font_setup_rva);

        if (profile.kind == pl::EditionKind::CompleteEdition) {
            EXPECT_EQ(profile.text_render_core_hook_len, pl::kHookLen9Bytes);
            EXPECT_FALSE(pl::MatchesTextRenderCoreSignature(
                profile, image.data(), layout->text_section, kCeDecoyRenderCoreRva));
        }
        if (profile.kind != pl::EditionKind::CompleteEdition) {
            EXPECT_EQ(profile.text_render_core_hook_len, pl::kHookLen12Bytes);
        }

        const auto specs = pl::BuildPrintMessagePatchSpecs(profile, resolved->sentinel_rva);
        ASSERT_EQ(specs.size(), pl::kPrintMessagePatchCount);
        for (const pl::PatchSpec& spec : specs) {
            EXPECT_TRUE(pl::IsValidSpec(spec));
        }

        ASSERT_TRUE(pl::ApplyPrintMessagePatchesInMemory(
            image.data(), layout->text_section, profile, resolved->sentinel_rva));

        for (const pl::PatchSpec& spec : specs) {
            EXPECT_TRUE(pl::BytesMatch(image.data() + spec.rva, spec.replacement, spec.len))
                << profile.name << " failed post-patch verification at RVA 0x" << std::hex
                << spec.rva;
        }

        const auto post_patch_hooks = pl::ResolveTranslatorHooks(
            image.data(), layout->text_section, layout->time_date_stamp);
        ASSERT_TRUE(post_patch_hooks.has_value()) << profile.name;
        EXPECT_EQ(post_patch_hooks->profile->kind, profile.kind);
        EXPECT_EQ(post_patch_hooks->text_splitter_rva, profile.text_splitter_rva);
        EXPECT_EQ(post_patch_hooks->text_render_core_rva, profile.text_render_core_rva);
        EXPECT_EQ(post_patch_hooks->font_setup_rva, profile.font_setup_rva);
    }
}

TEST(PatcherLogic, EditionProfiles_ShiftedRvaScanFallbackAndMismatchRejection) {
    for (const pl::EditionProfile& profile : pl::kEditionProfiles) {
        auto shifted_image = BuildSyntheticEditionImage(
            profile, kShiftedRvaDelta, kRelocImm32VariantB);
        const auto layout = pl::ParsePeImageLayout(
            shifted_image.data(), pl::kDefaultPeHeaderMaxBytes);
        ASSERT_TRUE(layout.has_value()) << profile.name;

        const auto resolved = pl::ResolveEditionTargets(
            shifted_image.data(), layout->text_section, layout->time_date_stamp);
        ASSERT_TRUE(resolved.has_value()) << profile.name;
        EXPECT_EQ(resolved->profile->kind, profile.kind);
        EXPECT_TRUE(resolved->used_scan_fallback);
        EXPECT_EQ(resolved->sentinel_rva, profile.sentinel_rva + kShiftedRvaDelta);
        EXPECT_EQ(resolved->text_splitter_rva, profile.text_splitter_rva + kShiftedRvaDelta);
        EXPECT_EQ(resolved->text_render_core_rva,
                  profile.text_render_core_rva + kShiftedRvaDelta);
        EXPECT_EQ(resolved->font_setup_rva, profile.font_setup_rva + kShiftedRvaDelta);

        ASSERT_TRUE(pl::ApplyPrintMessagePatchesInMemory(
            shifted_image.data(), layout->text_section, profile, resolved->sentinel_rva));
    }

    auto bad_patch = BuildSyntheticEditionImage(pl::kHd10Profile);
    bad_patch[pl::kHd10SentinelRva + pl::kDelta2019FxfSpaceSkip] = kCorruptByte;
    EXPECT_FALSE(pl::ResolvePrintMessageTarget(
                     bad_patch.data(),
                     pl::kHd10Profile.expected_text_section,
                     pl::kHd10TimeDateStamp)
                     .has_value());

    auto bad_ce_anchor = BuildSyntheticEditionImage(pl::kCompleteEditionProfile);
    bad_ce_anchor[pl::kCeTextRenderCoreRva + pl::kRenderCoreSecondaryAnchorOffset] = kCorruptByte;
    EXPECT_FALSE(pl::ResolveTranslatorHooks(
                     bad_ce_anchor.data(),
                     pl::kCompleteEditionProfile.expected_text_section,
                     pl::kCeTimeDateStamp)
                     .has_value());

    auto ambiguous = BuildSyntheticEditionImage(pl::kHd10Profile, kShiftedRvaDelta);
    StampEditionIntoBuffer(
        ambiguous,
        pl::kHd10Profile,
        kShiftedRvaDelta + kDuplicateOffsetDelta,
        kSampleAslrRelocImm32);
    EXPECT_FALSE(pl::ResolveEditionTargets(
                     ambiguous.data(),
                     pl::kHd10Profile.expected_text_section,
                     pl::kHd10TimeDateStamp)
                     .has_value());
}

TEST(PatcherLogic, TrampolineBuilder_Encodes9And12ByteDetoursAcrossAslrBases) {
    struct HookShape {
        const std::uint8_t* prologue;
        std::size_t hook_len;
        std::uint32_t rva;
    };
    const std::array<HookShape, kHookShapeVariantCount> shapes = {{
        {pl::kCompleteEditionProfile.text_render_core_prologue.data(),
         pl::kHookLen9Bytes,
         pl::kCeTextRenderCoreRva},
        {pl::kHd10Profile.text_render_core_prologue.data(),
         pl::kHookLen12Bytes,
         pl::kHd10TextRenderCoreRva},
    }};

    constexpr std::uintptr_t kDetourFnVa = 0x58123450u;

    for (const HookShape& shape : shapes) {
        for (std::uintptr_t image_base : kTestAslrImageBases) {
            for (std::uintptr_t tramp_va : kTestTrampolineAllocBases) {
                const std::uintptr_t hook_va = image_base + shape.rva;

                std::vector<std::uint8_t> hook_buf(
                    shape.prologue, shape.prologue + shape.hook_len);
                std::array<std::uint8_t, pl::kMaxDetourTrampolineBytes> tramp_buf{};

                const std::size_t written = pl::BuildDetourTrampoline(
                    tramp_buf.data(), tramp_va, hook_buf.data(), hook_va, shape.hook_len);
                ASSERT_EQ(written, shape.hook_len + pl::kRelBranchInstructionSize);
                EXPECT_TRUE(pl::BytesMatch(tramp_buf.data(), shape.prologue, shape.hook_len));
                EXPECT_EQ(tramp_buf[shape.hook_len], pl::kRelJmpOpcode);

                const std::int32_t back_rel = ReadI32Le(tramp_buf.data() + shape.hook_len + 1);
                const std::uint32_t resolved_return_va =
                    static_cast<std::uint32_t>(tramp_va + shape.hook_len +
                                               pl::kRelBranchInstructionSize) +
                    static_cast<std::uint32_t>(back_rel);
                EXPECT_EQ(resolved_return_va, static_cast<std::uint32_t>(hook_va + shape.hook_len));

                ASSERT_TRUE(pl::WriteDetourJump(
                    hook_buf.data(), hook_va, shape.hook_len, kDetourFnVa));
                EXPECT_EQ(hook_buf[0], pl::kRelJmpOpcode);
                const std::int32_t detour_rel = ReadI32Le(hook_buf.data() + 1);
                const std::uint32_t resolved_detour_va =
                    static_cast<std::uint32_t>(hook_va + pl::kRelBranchInstructionSize) +
                    static_cast<std::uint32_t>(detour_rel);
                EXPECT_EQ(resolved_detour_va, static_cast<std::uint32_t>(kDetourFnVa));

                for (std::size_t idx = pl::kRelBranchInstructionSize;
                     idx < shape.hook_len;
                     ++idx) {
                    EXPECT_EQ(hook_buf[idx], pl::kNopOpcode);
                }
            }
        }
    }
}

TEST(TranslatorLogic, RuntimeUiFallbacks_All22Cp932StringsEncodeCleanlyAndRespectTsvPrecedence) {
    tl::TranslationTable table;
    EXPECT_TRUE(table.Empty());
    EXPECT_EQ(table.SeedRuntimeUiFallbacks(), tl::kRuntimeUiFallbackCount);
    EXPECT_EQ(table.ExactSize(), tl::kRuntimeUiFallbackCount);

    const auto& fallbacks = tl::GetRuntimeUiFallbackEntries();
    ASSERT_EQ(fallbacks.size(), tl::kRuntimeUiFallbackCount);

    for (std::size_t i = 0; i < fallbacks.size(); ++i) {
        std::string jp_key;
        ASSERT_TRUE(tl::EncodeWideToCp932(fallbacks[i].jp_text, jp_key))
            << "Fallback index " << i << " failed lossless CP932 encoding";
        ASSERT_FALSE(jp_key.empty()) << "Fallback index " << i;

        std::string resolved;
        ASSERT_TRUE(table.Lookup(jp_key.c_str(), true, resolved))
            << "Fallback index " << i << " failed lookup";
        EXPECT_EQ(resolved, fallbacks[i].en_text);

        const int sample_font =
            (i < kMusicGalleryFallbackEntryCount)
                ? kMusicGalleryFontSize
                : ((i == kConfigNameFallbackIndex) ? kConfigNameFontSize : kConfigBodyFontSize);
        const int sample_space =
            (i < kMusicGalleryFallbackEntryCount) ? kMusicGallerySpace : kSingleLinePositiveSpace;
        const tl::UiFontAdjustment adj = tl::ComputeUiFontAdjustment(
            resolved.c_str(), true, sample_font, sample_space, kFullMaxChars);
        EXPECT_TRUE(adj.shouldAdjust) << "Fallback index " << i;
        EXPECT_EQ(adj.newFontSize, tl::kStandardChoiceFontSize);
        EXPECT_EQ(adj.newSpace, tl::kPatchedChoiceSpace);
        EXPECT_EQ(adj.yOffset,
                  (sample_font - tl::kStandardChoiceFontSize) / tl::kVerticalCenteringDivisor);
        EXPECT_FALSE(adj.expandMaxChars);
    }

    const tl::UiFontAdjustment dlg_adj = tl::ComputeUiFontAdjustment(
        "Translated dialogue line.",
        true,
        kPatchedDialogueFontSize,
        kPatchedDialogueSpace,
        kFullMaxChars);
    EXPECT_FALSE(dlg_adj.shouldAdjust);

    const std::string long_choice =
        ToCp932(L"#黄Wait outside the cavern entrance until Yuria returns safely");
    const tl::UiFontAdjustment choice_adj = tl::ComputeUiFontAdjustment(
        long_choice.c_str(),
        true,
        kChoiceFontSize,
        kSingleLinePositiveSpace,
        kAnimatedChoiceMaxChars);
    EXPECT_TRUE(choice_adj.shouldAdjust);
    EXPECT_EQ(choice_adj.newFontSize, tl::kCompactChoiceFontSize);
    EXPECT_EQ(choice_adj.newSpace, tl::kPatchedChoiceSpace);
    EXPECT_TRUE(choice_adj.expandMaxChars);

    tl::TranslationTable override_table;
    const std::string first_track_jp = ToCp932(fallbacks[0].jp_text);
    const std::string custom_tsv = first_track_jp + "\tCustom Override Title\n";
    ASSERT_EQ(override_table.LoadFromTsvBuffer(custom_tsv.data(), custom_tsv.size()),
              kSingleOverrideRowCount);
    EXPECT_EQ(override_table.SeedRuntimeUiFallbacks(), tl::kRuntimeUiFallbackCount - 1);

    std::string override_out;
    ASSERT_TRUE(override_table.Lookup(first_track_jp.c_str(), true, override_out));
    EXPECT_EQ(override_out, "Custom Override Title");
}

TEST(TranslatorLogic, SanitizeAsciiHashInPlace_HandlesPrefixColorTagsMidlineAndTrailingHashes) {
    const std::string fullwidth_hash = ToCp932(L"＃");
    ASSERT_EQ(fullwidth_hash.size(), kCp932DoubleByteCharLen);

    // Line-leading markdown "# " without color tag is stripped cleanly.
    std::string bare_prefix = "# W-Whaaat?!";
    tl::SanitizeAsciiHashInPlace(bare_prefix);
    EXPECT_EQ(bare_prefix, "W-Whaaat?!");
    EXPECT_FALSE(ContainsUnvalidatedAsciiHash(bare_prefix));

    // Valid CP932 `#黄` followed by markdown "# " preserves `#黄` and strips "# ".
    std::string yellow_prefix = ToCp932(L"#黄# W-Whaaat?!");
    tl::SanitizeAsciiHashInPlace(yellow_prefix);
    EXPECT_EQ(yellow_prefix, ToCp932(L"#黄W-Whaaat?!"));
    EXPECT_TRUE(tl::HasCp932ColorPrefix(yellow_prefix.c_str()));
    EXPECT_FALSE(ContainsUnvalidatedAsciiHash(yellow_prefix));

    // Valid CP932 `#白` followed by markdown "# " preserves `#白` and strips "# ".
    std::string white_prefix = ToCp932(L"#白# Y-Yeah...");
    tl::SanitizeAsciiHashInPlace(white_prefix);
    EXPECT_EQ(white_prefix, ToCp932(L"#白Y-Yeah..."));
    EXPECT_TRUE(tl::HasCp932ColorPrefix(white_prefix.c_str()));
    EXPECT_FALSE(ContainsUnvalidatedAsciiHash(white_prefix));

    // Markdown "# " on a second line after `\r\n` (speaker + body) is stripped.
    std::string multiline_prefix = "Ren\r\n# W-Whaaat?!\r\n";
    tl::SanitizeAsciiHashInPlace(multiline_prefix);
    EXPECT_EQ(multiline_prefix, "Ren\r\nW-Whaaat?!\r\n");
    EXPECT_FALSE(ContainsUnvalidatedAsciiHash(multiline_prefix));

    // Mid-line ASCII '#' is converted to 2-byte fullwidth CP932 `＃` (zero raw 0x23).
    std::string midline_hash = "Room #12";
    tl::SanitizeAsciiHashInPlace(midline_hash);
    EXPECT_EQ(midline_hash, ToCp932(L"Room ＃12"));
    EXPECT_EQ(midline_hash.find('#'), std::string::npos);

    // Standalone and trailing '#' / '#1' convert to fullwidth `＃` without OOB reads.
    std::string single_hash = "#";
    tl::SanitizeAsciiHashInPlace(single_hash);
    EXPECT_EQ(single_hash, fullwidth_hash);
    EXPECT_EQ(single_hash.find('#'), std::string::npos);

    std::string trailing_hash_digit = "#1";
    tl::SanitizeAsciiHashInPlace(trailing_hash_digit);
    EXPECT_EQ(trailing_hash_digit, ToCp932(L"＃1"));
    EXPECT_EQ(trailing_hash_digit.find('#'), std::string::npos);

    std::string word_trailing_hash = "Test #";
    tl::SanitizeAsciiHashInPlace(word_trailing_hash);
    EXPECT_EQ(word_trailing_hash, ToCp932(L"Test ＃"));
    EXPECT_EQ(word_trailing_hash.find('#'), std::string::npos);

    // Leading `%` speaker control marker plus `#黄` plus `# ` preserves `%#黄` and strips `# `.
    std::string percent_yellow = ToCp932(L"%#黄# W-Whaaat?!");
    tl::SanitizeAsciiHashInPlace(percent_yellow);
    EXPECT_EQ(percent_yellow, ToCp932(L"%#黄W-Whaaat?!"));
    EXPECT_FALSE(ContainsUnvalidatedAsciiHash(percent_yellow));
}

TEST(TranslatorLogic, TranslationTable_SyntheticCrLfParityAndHashSanitizationOnLookup) {
    const std::string yellow_cave_jp = ToCp932(L"#黄洞窟");
    const std::string dialogue_hd10_jp = ToCp932(L"%蓮\r\nおはよう\r\n");
    const std::string dialogue_ce_jp = ToCp932(L"%蓮\r\nおはよう");

    tl::TranslationMap raw_map;
    const std::string raw_tsv =
        yellow_cave_jp + "\t# Waiting Outside the Yellow Cave\r\n" +
        "room_key\\r\\n\tRoom #12\\r\\n\r\n" +
        "shout_w\t# W-Whaaat?!\r\n" +
        "shout_y\t# Y-Yeah...\r\n" +
        ToCp932(L"%蓮") + "\\r\\n" + ToCp932(L"おはよう") + "\\r\\n\t%Ren\\r\\n# Good morning #1!\\r\\n\r\n";

    EXPECT_EQ(tl::ParseTsvBuffer(raw_tsv.data(), raw_tsv.size(), raw_map),
              kExpectedSyntheticTsvRowCount);
    EXPECT_EQ(raw_map["room_key\r\n"], "Room #12\r\n");

    tl::TranslationTable table;
    ASSERT_EQ(table.LoadFromTsvBuffer(raw_tsv.data(), raw_tsv.size()),
              kExpectedSyntheticTsvRowCount);

    std::string out_choice;
    ASSERT_TRUE(table.Lookup(yellow_cave_jp.c_str(), true, out_choice));
    EXPECT_EQ(out_choice, ToCp932(L"#黄Waiting Outside the Yellow Cave"));
    EXPECT_TRUE(tl::HasCp932ColorPrefix(out_choice.c_str()));
    EXPECT_FALSE(ContainsUnvalidatedAsciiHash(out_choice));

    std::string out_room_hd10;
    ASSERT_TRUE(table.Lookup("room_key\r\n", false, out_room_hd10));
    EXPECT_EQ(out_room_hd10, ToCp932(L"Room ＃12\r\n"));
    EXPECT_EQ(out_room_hd10.find('#'), std::string::npos);

    // Complete Edition query without trailing "\r\n" resolves the same entry.
    std::string out_room_ce;
    ASSERT_TRUE(table.Lookup("room_key", true, out_room_ce));
    EXPECT_EQ(out_room_ce, ToCp932(L"Room ＃12"));
    EXPECT_EQ(out_room_ce.find('#'), std::string::npos);

    // Multi-line speaker + body lookup matches both HD10 (`...\r\n`) and CE (`...`)
    // and sanitizes both line-2 `# ` and mid-line `#1`.
    std::string out_dlg_hd10;
    ASSERT_TRUE(table.Lookup(dialogue_hd10_jp.c_str(), false, out_dlg_hd10));
    EXPECT_EQ(out_dlg_hd10, ToCp932(L"%Ren\r\nGood morning ＃1!\r\n"));
    EXPECT_FALSE(ContainsUnvalidatedAsciiHash(out_dlg_hd10));

    std::string out_dlg_ce;
    ASSERT_TRUE(table.Lookup(dialogue_ce_jp.c_str(), false, out_dlg_ce));
    EXPECT_EQ(out_dlg_ce, out_dlg_hd10);
}

TEST(TranslatorLogic, ExtractFxfWindowTitle_ReturnsCleanEnglishForCp932AndVerbatimForAscii) {
    // Pristine unpatched CP932 HD10 mainControl.fxf (XOR 0xFF) -> English HD10 title.
    const std::string cp932_hd10_fxf =
        ToCp932(L"gameVersion 100\r\nwindowTitle EXTRAVAGANZA～蟲狂編～HD10\r\n");
    const auto enc_cp932_hd10 = EncodeXorFxf(cp932_hd10_fxf);
    EXPECT_EQ(tl::ExtractFxfWindowTitle(enc_cp932_hd10.data(), enc_cp932_hd10.size()),
              "EXTRAVAGANZA ~Mushigurui~ HD10");

    // Pristine unpatched CP932 Complete Edition mainControl.fxf (XOR 0xFF) -> English title.
    const std::string cp932_ce_fxf =
        ToCp932(L"gameVersion 100\r\nwindowTitle EXTRAVAGANZA～蟲狂編～\r\n");
    const auto enc_cp932_ce = EncodeXorFxf(cp932_ce_fxf);
    EXPECT_EQ(tl::ExtractFxfWindowTitle(enc_cp932_ce.data(), enc_cp932_ce.size()),
              "EXTRAVAGANZA ~Mushigurui~");

    // Patched pure ASCII mainControl.fxf (XOR 0xFF) -> returned verbatim.
    const std::string ascii_fxf =
        "gameVersion 100\r\n"
        "\twindowTitle\tEXTRAVAGANZA ~Mushigurui~ Custom\r\n";
    const auto enc_ascii = EncodeXorFxf(ascii_fxf);
    EXPECT_EQ(tl::ExtractFxfWindowTitle(enc_ascii.data(), enc_ascii.size()),
              "EXTRAVAGANZA ~Mushigurui~ Custom");

    // Plaintext ASCII mainControl.fxf -> returned verbatim.
    EXPECT_EQ(tl::ExtractFxfWindowTitle(
                  reinterpret_cast<const std::uint8_t*>(ascii_fxf.data()),
                  ascii_fxf.size()),
              "EXTRAVAGANZA ~Mushigurui~ Custom");
}

namespace {

constexpr std::size_t kExpectedOrigCharanameBytes = 104u;
constexpr std::uint32_t kExpectedOrigCharanameHash = 0x62F2B2CEu;
constexpr std::size_t kSyntheticPatchedCharanameBytes = 154u;
constexpr std::size_t kSyntheticOrigBacklogBytes = 256u;
constexpr std::size_t kSyntheticPatchedBacklogBytes = 244u;
constexpr std::size_t kSyntheticHd10MainControlBytes = 480u;
constexpr std::size_t kSyntheticPatchedHd10MainControlBytes = 500u;
constexpr std::size_t kSyntheticCeMainControlBytes = 468u;
constexpr std::size_t kSyntheticPatchedCeMainControlBytes = 488u;

constexpr std::size_t kBytesPerKilobyte = 1024u;
constexpr std::size_t kMultiChunkUntouchedKilobytes = 140u;
constexpr std::size_t kMultiChunkUntouchedBytes =
    kMultiChunkUntouchedKilobytes * kBytesPerKilobyte;
constexpr std::size_t kSmallEntryOrigBytes = 512u;
constexpr std::size_t kSmallEntryNewBytes = 768u;
constexpr std::size_t kPoolFillerBytes = 48u;
constexpr std::size_t kDeterministicByteStride = 37u;

constexpr std::uint8_t kPoolFillerSeed = 0x42u;
constexpr std::uint8_t kOrigCharanameSeed = 0x11u;
constexpr std::uint8_t kPatchedCharanameSeed = 0x22u;
constexpr std::uint8_t kOrigBacklogSeed = 0x33u;
constexpr std::uint8_t kPatchedBacklogSeed = 0x44u;
constexpr std::uint8_t kOrigHd10MainControlSeed = 0x55u;
constexpr std::uint8_t kPatchedHd10MainControlSeed = 0x66u;
constexpr std::uint8_t kOrigCeMainControlSeed = 0x77u;
constexpr std::uint8_t kPatchedCeMainControlSeed = 0x88u;
constexpr std::uint8_t kSys0ArchiveSeed = 0x10u;
constexpr std::uint8_t kTa0ArchiveSeed = 0x30u;
constexpr std::uint8_t kEv0ArchiveSeed = 0x50u;
constexpr std::uint8_t kPatchedOrigSeedOffset = 5u;
constexpr std::uint8_t kPatchedNewSeedOffset = 11u;
constexpr std::uint8_t kTrailingEntrySeedOffset = 17u;

constexpr std::size_t kMultiChunkLengthCaseCount = 6u;
constexpr std::array<std::size_t, kMultiChunkLengthCaseCount> kMultiChunkOrigEntryLengths = {{
    1u,
    2u,
    3u,
    4u,
    7u,
    19u,
}};
constexpr std::size_t kPatchedEntryLengthIncrement = 13u;
constexpr std::uint8_t kMultiChunkBaseSeed = 0x29u;
constexpr std::uint8_t kMultiChunkPatchedSeed = 0x91u;

constexpr std::uint8_t kSingleBitFlip = 0x01u;
constexpr std::uint8_t kCorruptFillByte = 0xFEu;
constexpr unsigned int kBitsPerByte = 8u;
constexpr unsigned int kBitsPerU32 = 32u;
constexpr unsigned int kBitsPerU64 = 64u;
constexpr std::uint32_t kLowByteMask = 0xFFu;
constexpr std::uint64_t kLowU32Mask = 0xFFFFFFFFull;
constexpr std::size_t kOver2GTrailerBytes = 8u;
constexpr std::uint32_t kU64EntryBytes = 8u;
constexpr std::size_t kGtbU64ZeroPadBytes = 8u;
constexpr std::size_t kMsgMenu1MagicSize = 8u;
constexpr char kMsgMenu1Magic[kMsgMenu1MagicSize] = {
    'M', 'S', 'G', 'M', 'E', 'N', 'U', '1'};
constexpr std::uint32_t kMsgMenu1Version = 1u;
constexpr std::uint32_t kSyntheticStandaloneGroupCount = 3u;
constexpr std::size_t kSyntheticArchiveCount = 3u;
constexpr std::uint32_t kPatchedEntriesPerSyntheticArchive = 1u;
constexpr std::uint32_t kMiddleArchiveEntryIndex = 1u;

constexpr std::size_t kSys0ArchiveIndex = 0u;
constexpr std::size_t kTa0ArchiveIndex = 1u;
constexpr std::size_t kEv0ArchiveIndex = 2u;
constexpr std::size_t kSingleVariantIndex = 0u;
constexpr std::size_t kHd10VariantIndex = 0u;
constexpr std::size_t kCeVariantIndex = 1u;
constexpr std::size_t kDualEditionCaseCount = 2u;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view utf8_leaf_prefix) {
        static std::uint64_t counter = 0;
        ++counter;
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::string dir_name = std::string(utf8_leaf_prefix) + "_" +
                                     std::to_string(tick) + "_" +
                                     std::to_string(counter);
        path_ = fs::temp_directory_path() / fs::u8path(dir_name);
        fs::create_directories(path_);
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

void PushU16Le(std::vector<std::uint8_t>& out, std::uint16_t val) {
    out.push_back(static_cast<std::uint8_t>(val & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((val >> kBitsPerByte) & kLowByteMask));
}

void PushU32Le(std::vector<std::uint8_t>& out, std::uint32_t val) {
    for (unsigned int s = 0; s < kBitsPerU32; s += kBitsPerByte) {
        out.push_back(static_cast<std::uint8_t>((val >> s) & kLowByteMask));
    }
}

void PushU64Le(std::vector<std::uint8_t>& out, std::uint64_t val) {
    for (unsigned int s = 0; s < kBitsPerU64; s += kBitsPerByte) {
        out.push_back(static_cast<std::uint8_t>((val >> s) & kLowByteMask));
    }
}

std::vector<std::uint8_t> MakeDeterministicBytes(std::size_t len, std::uint8_t seed) {
    std::vector<std::uint8_t> buf(len);
    for (std::size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<std::uint8_t>((i * kDeterministicByteStride + seed) & kLowByteMask);
    }
    return buf;
}

std::vector<std::uint8_t> BuildOver2GGtb(const std::vector<std::uint64_t>& offsets) {
    constexpr char kOver2GTrailer[kOver2GTrailerBytes] = {
        'o', 'v', 'e', 'r', '2', 'G', '!', '\0'};
    const auto count = static_cast<std::uint32_t>(offsets.size());
    std::vector<std::uint8_t> gtb;
    PushU32Le(gtb, count);
    for (std::uint32_t i = 0; i < count; ++i) {
        PushU32Le(gtb, (count - i) * kU64EntryBytes);
    }
    for (std::uint64_t off : offsets) {
        PushU32Le(gtb, static_cast<std::uint32_t>(off & kLowU32Mask));
    }
    const auto pool = MakeDeterministicBytes(kPoolFillerBytes, kPoolFillerSeed);
    gtb.insert(gtb.end(), pool.begin(), pool.end());
    for (std::uint64_t off : offsets) {
        PushU64Le(gtb, off);
    }
    gtb.insert(gtb.end(), kGtbU64ZeroPadBytes, 0u);
    gtb.insert(gtb.end(), kOver2GTrailer, kOver2GTrailer + sizeof(kOver2GTrailer));
    return gtb;
}

void WriteBinaryFile(const fs::path& file_path, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(file_path.parent_path());
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(static_cast<bool>(out));
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_TRUE(static_cast<bool>(out));
}

std::vector<std::uint8_t> ReadBinaryFile(const fs::path& file_path) {
    std::ifstream in(file_path, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const std::streamsize sz = in.tellg();
    if (sz <= 0) {
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
    in.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

std::size_t CountFilesWithExtension(const fs::path& root, std::wstring_view ext) {
    std::size_t count = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension().wstring() == ext) {
            ++count;
        }
    }
    return count;
}

struct SyntheticVariantSpec {
    std::vector<std::uint8_t> orig_bytes;
    std::vector<std::uint8_t> new_bytes;
};

struct SyntheticStandaloneGroupSpec {
    std::string rel_path;
    std::vector<SyntheticVariantSpec> variants;
};

struct SyntheticArchiveSpec {
    std::string stem;
    std::vector<std::uint8_t> orig_gpk;
    std::vector<std::uint8_t> new_gpk;
    std::vector<std::uint8_t> orig_gtb;
    std::vector<std::uint8_t> new_gtb;
    std::uint32_t patched_entry_idx = 0;
    std::uint64_t patched_orig_offset = 0;
    std::vector<std::uint8_t> patched_orig_entry;
    std::vector<std::uint8_t> patched_new_entry;
};

struct SyntheticPatchFixture {
    SyntheticStandaloneGroupSpec charaname;
    SyntheticStandaloneGroupSpec backlog;
    SyntheticStandaloneGroupSpec main_control;
    std::array<SyntheticArchiveSpec, kSyntheticArchiveCount> archives;
    std::vector<std::uint8_t> patch_bin;
    std::size_t last_archive_entry_payload_offset = 0;
};

SyntheticArchiveSpec MakeSyntheticArchive(std::string_view stem,
                                          std::size_t prefix_entry_bytes,
                                          std::uint8_t seed) {
    SyntheticArchiveSpec sec;
    sec.stem = std::string(stem);
    const auto e0 = MakeDeterministicBytes(prefix_entry_bytes, seed);
    sec.patched_orig_entry = MakeDeterministicBytes(
        kSmallEntryOrigBytes, static_cast<std::uint8_t>(seed + kPatchedOrigSeedOffset));
    sec.patched_new_entry = MakeDeterministicBytes(
        kSmallEntryNewBytes, static_cast<std::uint8_t>(seed + kPatchedNewSeedOffset));
    const auto e2 = MakeDeterministicBytes(
        kSmallEntryOrigBytes, static_cast<std::uint8_t>(seed + kTrailingEntrySeedOffset));

    sec.patched_entry_idx = kMiddleArchiveEntryIndex;
    sec.patched_orig_offset = e0.size();

    sec.orig_gpk = e0;
    sec.orig_gpk.insert(
        sec.orig_gpk.end(), sec.patched_orig_entry.begin(), sec.patched_orig_entry.end());
    sec.orig_gpk.insert(sec.orig_gpk.end(), e2.begin(), e2.end());

    sec.new_gpk = e0;
    sec.new_gpk.insert(
        sec.new_gpk.end(), sec.patched_new_entry.begin(), sec.patched_new_entry.end());
    sec.new_gpk.insert(sec.new_gpk.end(), e2.begin(), e2.end());

    sec.orig_gtb = BuildOver2GGtb(
        {0ull, e0.size(), e0.size() + sec.patched_orig_entry.size()});
    sec.new_gtb = BuildOver2GGtb(
        {0ull, e0.size(), e0.size() + sec.patched_new_entry.size()});
    return sec;
}

SyntheticPatchFixture BuildSyntheticMsgMenu1Fixture() {
    SyntheticPatchFixture fx;
    fx.charaname = {
        "spt\\charaname.xtx",
        {{MakeDeterministicBytes(kExpectedOrigCharanameBytes, kOrigCharanameSeed),
          MakeDeterministicBytes(kSyntheticPatchedCharanameBytes, kPatchedCharanameSeed)}}};
    fx.backlog = {
        "init2\\backlog.fxf",
        {{MakeDeterministicBytes(kSyntheticOrigBacklogBytes, kOrigBacklogSeed),
          MakeDeterministicBytes(kSyntheticPatchedBacklogBytes, kPatchedBacklogSeed)}}};
    fx.main_control = {
        "init2\\mainControl.fxf",
        {{MakeDeterministicBytes(kSyntheticHd10MainControlBytes, kOrigHd10MainControlSeed),
          MakeDeterministicBytes(
              kSyntheticPatchedHd10MainControlBytes, kPatchedHd10MainControlSeed)},
         {MakeDeterministicBytes(kSyntheticCeMainControlBytes, kOrigCeMainControlSeed),
          MakeDeterministicBytes(
              kSyntheticPatchedCeMainControlBytes, kPatchedCeMainControlSeed)}}};

    fx.archives[kSys0ArchiveIndex] =
        MakeSyntheticArchive("dwq\\sys0", kSmallEntryOrigBytes, kSys0ArchiveSeed);
    fx.archives[kTa0ArchiveIndex] =
        MakeSyntheticArchive("dwq\\ta0", kMultiChunkUntouchedBytes, kTa0ArchiveSeed);
    fx.archives[kEv0ArchiveIndex] =
        MakeSyntheticArchive("dwq\\ev0", kSmallEntryOrigBytes, kEv0ArchiveSeed);

    std::vector<std::uint8_t>& out = fx.patch_bin;
    out.insert(out.end(), kMsgMenu1Magic, kMsgMenu1Magic + kMsgMenu1MagicSize);
    PushU32Le(out, kMsgMenu1Version);
    PushU32Le(out, kSyntheticStandaloneGroupCount);
    PushU32Le(out, static_cast<std::uint32_t>(fx.archives.size()));
    PushU32Le(out, 0u);
    PushU64Le(out, 0ull);

    for (const SyntheticStandaloneGroupSpec* grp :
         {&fx.charaname, &fx.backlog, &fx.main_control}) {
        PushU16Le(out, static_cast<std::uint16_t>(grp->rel_path.size()));
        out.insert(out.end(), grp->rel_path.begin(), grp->rel_path.end());
        PushU16Le(out, static_cast<std::uint16_t>(grp->variants.size()));
        for (const SyntheticVariantSpec& var : grp->variants) {
            const std::uint32_t orig_hash =
                ma::ComputeMurmurHash2(var.orig_bytes.data(), var.orig_bytes.size());
            const std::uint32_t new_hash =
                ma::ComputeMurmurHash2(var.new_bytes.data(), var.new_bytes.size());
            PushU32Le(out, static_cast<std::uint32_t>(var.orig_bytes.size()));
            PushU32Le(out, orig_hash);
            PushU32Le(out, static_cast<std::uint32_t>(var.new_bytes.size()));
            PushU32Le(out, new_hash);
            auto masked = var.new_bytes;
            ma::XorMaskPayload(masked.data(),
                               masked.size(),
                               var.orig_bytes.data(),
                               var.orig_bytes.size(),
                               orig_hash);
            out.insert(out.end(), masked.begin(), masked.end());
        }
    }

    for (std::size_t a = 0; a < fx.archives.size(); ++a) {
        const SyntheticArchiveSpec& sec = fx.archives[a];
        PushU16Le(out, static_cast<std::uint16_t>(sec.stem.size()));
        out.insert(out.end(), sec.stem.begin(), sec.stem.end());
        PushU64Le(out, sec.orig_gpk.size());
        PushU64Le(out, sec.new_gpk.size());

        const std::uint32_t orig_gtb_hash =
            ma::ComputeMurmurHash2(sec.orig_gtb.data(), sec.orig_gtb.size());
        const std::uint32_t new_gtb_hash =
            ma::ComputeMurmurHash2(sec.new_gtb.data(), sec.new_gtb.size());
        PushU32Le(out, static_cast<std::uint32_t>(sec.orig_gtb.size()));
        PushU32Le(out, orig_gtb_hash);
        PushU32Le(out, static_cast<std::uint32_t>(sec.new_gtb.size()));
        PushU32Le(out, new_gtb_hash);
        PushU32Le(out, kPatchedEntriesPerSyntheticArchive);

        auto masked_gtb = sec.new_gtb;
        ma::XorMaskPayload(masked_gtb.data(),
                           masked_gtb.size(),
                           sec.orig_gtb.data(),
                           sec.orig_gtb.size(),
                           orig_gtb_hash);
        out.insert(out.end(), masked_gtb.begin(), masked_gtb.end());

        const std::uint32_t entry_orig_hash = ma::ComputeMurmurHash2(
            sec.patched_orig_entry.data(), sec.patched_orig_entry.size());
        const std::uint32_t entry_new_hash = ma::ComputeMurmurHash2(
            sec.patched_new_entry.data(), sec.patched_new_entry.size());
        PushU32Le(out, sec.patched_entry_idx);
        PushU64Le(out, sec.patched_orig_offset);
        PushU32Le(out, static_cast<std::uint32_t>(sec.patched_orig_entry.size()));
        PushU32Le(out, entry_orig_hash);
        PushU32Le(out, static_cast<std::uint32_t>(sec.patched_new_entry.size()));
        PushU32Le(out, entry_new_hash);

        if (a == kEv0ArchiveIndex) {
            fx.last_archive_entry_payload_offset = out.size();
        }
        auto masked_entry = sec.patched_new_entry;
        ma::XorMaskPayload(masked_entry.data(),
                           masked_entry.size(),
                           sec.patched_orig_entry.data(),
                           sec.patched_orig_entry.size(),
                           entry_orig_hash);
        out.insert(out.end(), masked_entry.begin(), masked_entry.end());
    }

    return fx;
}

void PopulateUnpatchedGameDir(const fs::path& game_dir,
                              const SyntheticPatchFixture& fx,
                              std::size_t main_control_variant_idx) {
    WriteBinaryFile(game_dir / fs::u8path("spt/charaname.xtx"),
                    fx.charaname.variants[kSingleVariantIndex].orig_bytes);
    WriteBinaryFile(game_dir / fs::u8path("init2/backlog.fxf"),
                    fx.backlog.variants[kSingleVariantIndex].orig_bytes);
    WriteBinaryFile(game_dir / fs::u8path("init2/mainControl.fxf"),
                    fx.main_control.variants[main_control_variant_idx].orig_bytes);
    for (const SyntheticArchiveSpec& sec : fx.archives) {
        const fs::path base = game_dir / fs::u8path(sec.stem);
        WriteBinaryFile(fs::path(base.wstring() + L".gpk"), sec.orig_gpk);
        WriteBinaryFile(fs::path(base.wstring() + L".gtb"), sec.orig_gtb);
    }
}

}  // namespace

TEST(MenuAutopatch, MurmurHash2AndXorMaskPayload_MatchPipelineContractAndRoundTrip) {
    EXPECT_EQ(ma::ComputeMurmurHash2(nullptr, 0u), 0u);

    const std::string orig_charaname_cp932 = ToCp932(
        L"no,名前なし\r\n"
        L"aa,美弥香\r\n"
        L"bb,レン\r\n"
        L"cc,綾佳\r\n"
        L"dd,ユーリア\r\n"
        L"ee,シオ\r\n"
        L"ff,夢美\r\n"
        L"gg,風ノ宮\r\n"
        L"hh,キョウ\r\n"
        L"ii,狂\r\n"
        L"\r\n");
    ASSERT_EQ(orig_charaname_cp932.size(), kExpectedOrigCharanameBytes);
    const std::uint32_t computed_hash = ma::ComputeMurmurHash2(
        reinterpret_cast<const std::uint8_t*>(orig_charaname_cp932.data()),
        orig_charaname_cp932.size());
    EXPECT_EQ(computed_hash, kExpectedOrigCharanameHash);

    for (const std::size_t tail_len : kMultiChunkOrigEntryLengths) {
        const auto orig = MakeDeterministicBytes(tail_len, kMultiChunkBaseSeed);
        const auto payload = MakeDeterministicBytes(
            tail_len + kPatchedEntryLengthIncrement, kMultiChunkPatchedSeed);
        const std::uint32_t orig_hash = ma::ComputeMurmurHash2(orig.data(), orig.size());

        auto masked = payload;
        ma::XorMaskPayload(
            masked.data(), masked.size(), orig.data(), orig.size(), orig_hash);
        EXPECT_NE(masked, payload);
        ma::XorMaskPayload(
            masked.data(), masked.size(), orig.data(), orig.size(), orig_hash);
        EXPECT_EQ(masked, payload);

        auto keyless = payload;
        ma::XorMaskPayload(keyless.data(), keyless.size(), nullptr, 0u, orig_hash);
        EXPECT_NE(keyless, payload);
        ma::XorMaskPayload(keyless.data(), keyless.size(), nullptr, 0u, orig_hash);
        EXPECT_EQ(keyless, payload);
    }
}

TEST(MenuAutopatch, DualEditionMultiArchive_PatchesHd10AndCeFixturesAndIsIdempotent) {
    const SyntheticPatchFixture fx = BuildSyntheticMsgMenu1Fixture();

    const ScopedTempDir hd10_dir("mgi_autopatch_hd10");
    const ScopedTempDir ce_dir("mgi_autopatch_蟲狂編_ce");

    const std::array<std::pair<const fs::path*, std::size_t>, kDualEditionCaseCount>
        edition_cases = {{
            {&hd10_dir.path(), kHd10VariantIndex},
            {&ce_dir.path(), kCeVariantIndex},
        }};

    for (const auto& [dir_ptr, variant_idx] : edition_cases) {
        const fs::path& dir = *dir_ptr;
        PopulateUnpatchedGameDir(dir, fx, variant_idx);
        WriteBinaryFile(dir / fs::u8path("menu_patch.bin"), fx.patch_bin);

        ASSERT_TRUE(ma::EnsureGameFilesPatchedInDir(dir.wstring()));
        EXPECT_EQ(CountFilesWithExtension(dir, L".tmp"), 0u);

        EXPECT_EQ(ReadBinaryFile(dir / fs::u8path("spt/charaname.xtx.bak")),
                  fx.charaname.variants[kSingleVariantIndex].orig_bytes);
        EXPECT_EQ(ReadBinaryFile(dir / fs::u8path("spt/charaname.xtx")),
                  fx.charaname.variants[kSingleVariantIndex].new_bytes);

        EXPECT_EQ(ReadBinaryFile(dir / fs::u8path("init2/backlog.fxf.bak")),
                  fx.backlog.variants[kSingleVariantIndex].orig_bytes);
        EXPECT_EQ(ReadBinaryFile(dir / fs::u8path("init2/backlog.fxf")),
                  fx.backlog.variants[kSingleVariantIndex].new_bytes);

        EXPECT_EQ(ReadBinaryFile(dir / fs::u8path("init2/mainControl.fxf.bak")),
                  fx.main_control.variants[variant_idx].orig_bytes);
        EXPECT_EQ(ReadBinaryFile(dir / fs::u8path("init2/mainControl.fxf")),
                  fx.main_control.variants[variant_idx].new_bytes);

        for (const SyntheticArchiveSpec& sec : fx.archives) {
            const fs::path base = dir / fs::u8path(sec.stem);
            EXPECT_EQ(ReadBinaryFile(fs::path(base.wstring() + L".gpk.bak")), sec.orig_gpk);
            EXPECT_EQ(ReadBinaryFile(fs::path(base.wstring() + L".gtb.bak")), sec.orig_gtb);
            EXPECT_EQ(ReadBinaryFile(fs::path(base.wstring() + L".gpk")), sec.new_gpk);
            EXPECT_EQ(ReadBinaryFile(fs::path(base.wstring() + L".gtb")), sec.new_gtb);
        }

        // Second invocation hits fast-path idempotency without altering files.
        ASSERT_TRUE(ma::EnsureGameFilesPatchedInDir(dir.wstring()));
        EXPECT_EQ(CountFilesWithExtension(dir, L".tmp"), 0u);
        EXPECT_EQ(ReadBinaryFile(dir / fs::u8path("init2/mainControl.fxf")),
                  fx.main_control.variants[variant_idx].new_bytes);
    }
}

TEST(MenuAutopatch, CorruptionAndVariantMismatch_RollsBackTempFilesWithoutTouchingOriginals) {
    const SyntheticPatchFixture fx = BuildSyntheticMsgMenu1Fixture();
    const ScopedTempDir dir("mgi_autopatch_rollback");
    PopulateUnpatchedGameDir(dir.path(), fx, kHd10VariantIndex);

    // 1. Missing menu_patch.bin returns false cleanly.
    EXPECT_FALSE(ma::EnsureGameFilesPatchedInDir(dir.path().wstring()));
    EXPECT_EQ(CountFilesWithExtension(dir.path(), L".tmp"), 0u);
    EXPECT_EQ(CountFilesWithExtension(dir.path(), L".bak"), 0u);

    // 2. Bad magic, truncated EOF, and trailing byte past EOF all fail cleanly.
    auto bad_magic = fx.patch_bin;
    bad_magic[0] = 'X';
    WriteBinaryFile(dir.path() / fs::u8path("menu_patch.bin"), bad_magic);
    EXPECT_FALSE(ma::EnsureGameFilesPatchedInDir(dir.path().wstring()));

    auto truncated = fx.patch_bin;
    truncated.pop_back();
    WriteBinaryFile(dir.path() / fs::u8path("menu_patch.bin"), truncated);
    EXPECT_FALSE(ma::EnsureGameFilesPatchedInDir(dir.path().wstring()));

    auto trailing = fx.patch_bin;
    trailing.push_back(0u);
    WriteBinaryFile(dir.path() / fs::u8path("menu_patch.bin"), trailing);
    EXPECT_FALSE(ma::EnsureGameFilesPatchedInDir(dir.path().wstring()));

    // 3. Single-bit flip in the 3rd archive (`dwq\ev0`) after earlier `.tmp` files
    // were staged rolls back every `.tmp` file without creating `.bak` files.
    auto bit_flipped = fx.patch_bin;
    bit_flipped[fx.last_archive_entry_payload_offset] ^= kSingleBitFlip;
    WriteBinaryFile(dir.path() / fs::u8path("menu_patch.bin"), bit_flipped);
    EXPECT_FALSE(ma::EnsureGameFilesPatchedInDir(dir.path().wstring()));
    EXPECT_EQ(CountFilesWithExtension(dir.path(), L".tmp"), 0u);
    EXPECT_EQ(CountFilesWithExtension(dir.path(), L".bak"), 0u);
    EXPECT_EQ(ReadBinaryFile(dir.path() / fs::u8path("init2/mainControl.fxf")),
              fx.main_control.variants[kHd10VariantIndex].orig_bytes);
    EXPECT_EQ(ReadBinaryFile(dir.path() / fs::u8path("dwq/sys0.gpk")),
              fx.archives[kSys0ArchiveIndex].orig_gpk);
    EXPECT_EQ(ReadBinaryFile(dir.path() / fs::u8path("dwq/ta0.gpk")),
              fx.archives[kTa0ArchiveIndex].orig_gpk);
    EXPECT_EQ(ReadBinaryFile(dir.path() / fs::u8path("dwq/ev0.gpk")),
              fx.archives[kEv0ArchiveIndex].orig_gpk);

    // 4. Unknown mainControl.fxf variant hash fails without staging or modifying files.
    WriteBinaryFile(dir.path() / fs::u8path("menu_patch.bin"), fx.patch_bin);
    WriteBinaryFile(dir.path() / fs::u8path("init2/mainControl.fxf"),
                    MakeDeterministicBytes(kSyntheticHd10MainControlBytes, kCorruptFillByte));
    EXPECT_FALSE(ma::EnsureGameFilesPatchedInDir(dir.path().wstring()));
    EXPECT_EQ(CountFilesWithExtension(dir.path(), L".tmp"), 0u);
    EXPECT_EQ(CountFilesWithExtension(dir.path(), L".bak"), 0u);
}
