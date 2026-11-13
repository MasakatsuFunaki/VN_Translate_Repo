// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pure synthetic unit tests for the PE32 image reader and x86 disassembler.
#include "common/util.h"
#include "pe/disasm.h"
#include "pe/pe_image.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>

using namespace ksh;

namespace {

constexpr std::uint32_t kPageAlign = 0x1000u;
constexpr std::uint32_t kSectorAlign = 0x0200u;
constexpr std::uint32_t kTinyAlign = 0x0100u;
constexpr std::uint32_t kUnalignedPageVal = 0x1234u;
constexpr std::uint32_t kUnalignedSubPageVal = 0x0250u;
constexpr std::uint32_t kUnalignedTinyVal = 0x01FFu;

constexpr std::size_t kNonPeZeroBytes = 64;
constexpr std::size_t kSyntheticPeFileBytes = 0x0400;
constexpr std::size_t kExpectedMappedBytes = 0x1200;

constexpr std::size_t kDosLfanewOff = 0x3C;
constexpr std::uint32_t kPeHeaderOffset = 0x40u;
constexpr std::size_t kFileHeaderOff = 0x44;
constexpr std::size_t kOptHeaderOff = 0x58;
constexpr std::size_t kSectionHeaderOff = 0x138;

constexpr std::uint16_t kImageFileMachineI386 = 0x014Cu;
constexpr std::uint16_t kSyntheticSectionCount = 1u;
constexpr std::uint32_t kSyntheticTimestamp = 0x4458CC07u;
constexpr std::uint16_t kPe32OptHeaderSize = 0x00E0u;
constexpr std::uint16_t kSyntheticCharacteristics = 0x010Fu;
constexpr std::uint16_t kPe32Magic = 0x010Bu;
constexpr std::uint16_t kPe32PlusMagic = 0x020Bu;

constexpr std::uint32_t kEntryPointRva = 0x1000u;
constexpr std::uint32_t kSyntheticImageBase = 0x00400000u;
constexpr std::uint32_t kSyntheticSizeOfImage = 0x2000u;
constexpr std::uint16_t kSyntheticDllCharacteristics = 0x0140u;
constexpr std::uint32_t kStandardDataDirCount = 16u;

constexpr std::size_t kFhMachineOff = 0;
constexpr std::size_t kFhNumSectionsOff = 2;
constexpr std::size_t kFhTimestampOff = 4;
constexpr std::size_t kFhOptSizeOff = 16;
constexpr std::size_t kFhCharacteristicsOff = 18;

constexpr std::size_t kOhMagicOff = 0;
constexpr std::size_t kOhEntryPointOff = 16;
constexpr std::size_t kOhImageBaseOff = 28;
constexpr std::size_t kOhSectionAlignOff = 32;
constexpr std::size_t kOhFileAlignOff = 36;
constexpr std::size_t kOhSizeOfImageOff = 56;
constexpr std::size_t kOhDllCharOff = 70;
constexpr std::size_t kOhNumRvaOff = 92;
constexpr std::size_t kOhDataDirOff = 96;
constexpr std::size_t kDataDirEntryBytes = 8;
constexpr std::size_t kImportDirIndex = 1;
constexpr std::size_t kTlsDirIndex = 9;

constexpr std::size_t kSecVirtualSizeOff = 8;
constexpr std::size_t kSecVirtualAddrOff = 12;
constexpr std::size_t kSecRawSizeOff = 16;
constexpr std::size_t kSecRawPtrOff = 20;
constexpr std::size_t kSecCharacteristicsOff = 36;

constexpr std::uint32_t kTextRawOffset = 0x0200u;
constexpr std::uint32_t kTextRawSize = 0x0200u;
constexpr std::uint32_t kTextCharacteristics = 0x60000020u;

constexpr std::uint32_t kImportDirRva = 0x1010u;
constexpr std::uint32_t kImportDirSize = 40u;
constexpr std::uint32_t kThunkArrayRva = 0x1040u;
constexpr std::uint32_t kDllNameRva = 0x1050u;
constexpr std::uint32_t kHintNameRva = 0x1060u;
constexpr std::uint32_t kOrdinalFlag32 = 0x80000000u;
constexpr std::uint16_t kImportedOrdinal = 7u;

constexpr std::uint32_t kTlsDirRva = 0x1080u;
constexpr std::uint32_t kTlsDirSize = 24u;
constexpr std::uint32_t kTlsRawStartVa = 0x00401100u;
constexpr std::uint32_t kTlsRawEndVa = 0x00401110u;
constexpr std::uint32_t kTlsIndexVa = 0x00401114u;
constexpr std::uint32_t kTlsCallbacksVa = 0x00401118u;

constexpr std::size_t kImpNameOff = 12;
constexpr std::size_t kImpFirstThunkOff = 16;
constexpr std::size_t kHintBytes = 2;
constexpr std::size_t kPeSignatureBytes = 4;
constexpr std::size_t kTlsIndexWord = 2;
constexpr std::size_t kTlsCallbacksWord = 3;
constexpr std::size_t kExpectedImportedSymbolCount = 2;
constexpr std::size_t kExpectedInstructionCount = 4;

// x86 prologue + epilogue: push ebp; mov ebp, esp; xor eax, eax; pop ebp; ret
constexpr std::uint8_t code[] = {0x55u, 0x89u, 0xE5u, 0x31u, 0xC0u, 0x5Du, 0xC3u};

constexpr unsigned int kShift8 = 8;
constexpr unsigned int kShift16 = 16;
constexpr unsigned int kShift24 = 24;
constexpr std::uint32_t kByteMask = 0xFFu;

void wr_u16(Bytes& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v & kByteMask);
    b[off + 1] = static_cast<std::uint8_t>((v >> kShift8) & kByteMask);
}

void wr_u32(Bytes& b, std::size_t off, std::uint32_t v) {
    b[off] = static_cast<std::uint8_t>(v & kByteMask);
    b[off + 1] = static_cast<std::uint8_t>((v >> kShift8) & kByteMask);
    b[off + 2] = static_cast<std::uint8_t>((v >> kShift16) & kByteMask);
    b[off + 3] = static_cast<std::uint8_t>((v >> kShift24) & kByteMask);
}

void wr_ascii(Bytes& b, std::size_t off, std::string_view s) {
    std::memcpy(b.data() + off, s.data(), s.size());
}

std::size_t rva_in_text(std::uint32_t rva) {
    return kTextRawOffset + (rva - kEntryPointRva);
}

Bytes build_synthetic_pe32(std::uint16_t opt_magic = kPe32Magic) {
    Bytes b(kSyntheticPeFileBytes, 0);
    b[0] = 'M';
    b[1] = 'Z';
    wr_u32(b, kDosLfanewOff, kPeHeaderOffset);
    wr_ascii(b, kPeHeaderOffset, std::string_view("PE\0\0", kPeSignatureBytes));

    wr_u16(b, kFileHeaderOff + kFhMachineOff, kImageFileMachineI386);
    wr_u16(b, kFileHeaderOff + kFhNumSectionsOff, kSyntheticSectionCount);
    wr_u32(b, kFileHeaderOff + kFhTimestampOff, kSyntheticTimestamp);
    wr_u16(b, kFileHeaderOff + kFhOptSizeOff, kPe32OptHeaderSize);
    wr_u16(b, kFileHeaderOff + kFhCharacteristicsOff, kSyntheticCharacteristics);

    wr_u16(b, kOptHeaderOff + kOhMagicOff, opt_magic);
    wr_u32(b, kOptHeaderOff + kOhEntryPointOff, kEntryPointRva);
    wr_u32(b, kOptHeaderOff + kOhImageBaseOff, kSyntheticImageBase);
    wr_u32(b, kOptHeaderOff + kOhSectionAlignOff, kPageAlign);
    wr_u32(b, kOptHeaderOff + kOhFileAlignOff, kSectorAlign);
    wr_u32(b, kOptHeaderOff + kOhSizeOfImageOff, kSyntheticSizeOfImage);
    wr_u16(b, kOptHeaderOff + kOhDllCharOff, kSyntheticDllCharacteristics);
    wr_u32(b, kOptHeaderOff + kOhNumRvaOff, kStandardDataDirCount);

    const std::size_t imp_dir_off =
        kOptHeaderOff + kOhDataDirOff + kImportDirIndex * kDataDirEntryBytes;
    wr_u32(b, imp_dir_off, kImportDirRva);
    wr_u32(b, imp_dir_off + sizeof(std::uint32_t), kImportDirSize);

    const std::size_t tls_dir_off =
        kOptHeaderOff + kOhDataDirOff + kTlsDirIndex * kDataDirEntryBytes;
    wr_u32(b, tls_dir_off, kTlsDirRva);
    wr_u32(b, tls_dir_off + sizeof(std::uint32_t), kTlsDirSize);

    wr_ascii(b, kSectionHeaderOff, ".text");
    wr_u32(b, kSectionHeaderOff + kSecVirtualSizeOff, kTextRawSize);
    wr_u32(b, kSectionHeaderOff + kSecVirtualAddrOff, kEntryPointRva);
    wr_u32(b, kSectionHeaderOff + kSecRawSizeOff, kTextRawSize);
    wr_u32(b, kSectionHeaderOff + kSecRawPtrOff, kTextRawOffset);
    wr_u32(b, kSectionHeaderOff + kSecCharacteristicsOff, kTextCharacteristics);

    std::memcpy(b.data() + rva_in_text(kEntryPointRva), code, sizeof(code));

    const std::size_t imp0 = rva_in_text(kImportDirRva);
    wr_u32(b, imp0, kThunkArrayRva);
    wr_u32(b, imp0 + kImpNameOff, kDllNameRva);
    wr_u32(b, imp0 + kImpFirstThunkOff, kThunkArrayRva);

    const std::size_t thunks = rva_in_text(kThunkArrayRva);
    wr_u32(b, thunks, kHintNameRva);
    wr_u32(b, thunks + sizeof(std::uint32_t), kOrdinalFlag32 | kImportedOrdinal);

    wr_ascii(b, rva_in_text(kDllNameRva), "KERNEL32.dll");
    wr_ascii(b, rva_in_text(kHintNameRva) + kHintBytes, "ExitProcess");

    const std::size_t tls = rva_in_text(kTlsDirRva);
    wr_u32(b, tls, kTlsRawStartVa);
    wr_u32(b, tls + sizeof(std::uint32_t), kTlsRawEndVa);
    wr_u32(b, tls + kTlsIndexWord * sizeof(std::uint32_t), kTlsIndexVa);
    wr_u32(b, tls + kTlsCallbacksWord * sizeof(std::uint32_t), kTlsCallbacksVa);
    return b;
}

}  // namespace

// The rule is the sub-page fallback plus the round-down, and nothing else: an
// extra "FileAlignment < 0x200" early-out would shift the whole mapped layout.
TEST(PeImage, adjust_section_alignment_is_fallback_then_round_down) {
    EXPECT_EQ(pe::adjust_section_alignment(kPageAlign, kPageAlign, kSectorAlign), kPageAlign);
    EXPECT_EQ(pe::adjust_section_alignment(kUnalignedPageVal, kPageAlign, kSectorAlign),
              kPageAlign);
    EXPECT_EQ(pe::adjust_section_alignment(kUnalignedSubPageVal, kSectorAlign, kSectorAlign),
              kSectorAlign);
    EXPECT_EQ(pe::adjust_section_alignment(kUnalignedTinyVal, kTinyAlign, kTinyAlign), kTinyAlign);
    EXPECT_EQ(pe::adjust_section_alignment(kUnalignedPageVal, 0, 0), kUnalignedPageVal);
}

TEST(PeImage, rejects_non_pe_input) {
    EXPECT_THROW(pe::Image(Bytes(kNonPeZeroBytes, 0)), std::runtime_error);
    EXPECT_THROW(pe::Image(Bytes{}), std::runtime_error);
    EXPECT_THROW(pe::Image(build_synthetic_pe32(kPe32PlusMagic)), std::runtime_error);
}

TEST(PeImage, parses_synthetic_pe32_headers_sections_imports_tls_and_mapped_view) {
    const pe::Image pe(build_synthetic_pe32());
    EXPECT_EQ(pe.machine(), kImageFileMachineI386);
    EXPECT_EQ(pe.number_of_sections(), kSyntheticSectionCount);
    EXPECT_EQ(pe.time_date_stamp(), kSyntheticTimestamp);
    EXPECT_EQ(pe.characteristics(), kSyntheticCharacteristics);
    EXPECT_EQ(pe.image_base(), kSyntheticImageBase);
    EXPECT_EQ(pe.size_of_image(), kSyntheticSizeOfImage);
    EXPECT_EQ(pe.address_of_entry_point(), kEntryPointRva);
    EXPECT_EQ(pe.dll_characteristics(), kSyntheticDllCharacteristics);
    EXPECT_EQ(pe.section_alignment(), kPageAlign);
    EXPECT_EQ(pe.file_alignment(), kSectorAlign);

    ASSERT_EQ(pe.sections().size(), kSyntheticSectionCount);
    EXPECT_EQ(pe.sections()[0].name, ".text");
    EXPECT_EQ(pe.sections()[0].virtual_address, kEntryPointRva);
    EXPECT_EQ(pe.sections()[0].pointer_to_raw_data, kTextRawOffset);

    const Bytes& mapped = pe.memory_mapped_image();
    const Bytes& raw = pe.raw();
    ASSERT_EQ(mapped.size(), kExpectedMappedBytes);
    for (std::size_t i = 0; i < kTextRawSize; ++i) {
        EXPECT_EQ(mapped[kEntryPointRva + i], raw[kTextRawOffset + i]);
    }

    const auto& imps = pe.imports();
    ASSERT_EQ(imps.size(), 1u);
    EXPECT_EQ(imps[0].dll, "KERNEL32.dll");
    ASSERT_EQ(imps[0].imports.size(), kExpectedImportedSymbolCount);
    EXPECT_EQ(imps[0].imports[0].name, "ExitProcess");
    EXPECT_EQ(imps[0].imports[1].name, "");
    EXPECT_EQ(imps[0].imports[1].ordinal, kImportedOrdinal);

    const auto& tls = pe.tls();
    ASSERT_TRUE(tls.has_value());
    EXPECT_EQ(tls->start_address_of_raw_data, kTlsRawStartVa);
    EXPECT_EQ(tls->end_address_of_raw_data, kTlsRawEndVa);
    EXPECT_EQ(tls->address_of_index, kTlsIndexVa);
    EXPECT_EQ(tls->address_of_callbacks, kTlsCallbacksVa);

    const pe::Disassembler disasm;
    const auto ins = disasm.disasm(mapped.data() + kEntryPointRva, sizeof(code),
                                   static_cast<std::uint64_t>(kSyntheticImageBase) + kEntryPointRva);
    ASSERT_GE(ins.size(), kExpectedInstructionCount);
    EXPECT_EQ(ins.front().mnemonic, "push");
    EXPECT_EQ(ins.back().mnemonic, "ret");
}
