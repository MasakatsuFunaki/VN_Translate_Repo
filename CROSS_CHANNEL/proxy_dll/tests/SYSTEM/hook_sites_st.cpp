// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Hook-site verification system tests against unpacked cc_dumped.exe.
//
// Every hook installer in translator.cpp targets a specific RVA in cc.exe and
// expects exact bytes and imm32 values there.  When the local unpacked runtime
// snapshot (analysys/cc_dumped.exe) is present on disk, these tests assert the
// expected bytes match.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "bytecode_hooks.h"
#include "chrome_text_logic.h"
#include "engine_ui_hooks.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef CC_DUMPED_EXE_PATH
#define CC_DUMPED_EXE_PATH ""
#endif

namespace {

constexpr std::size_t kDefaultProbeBytes = 16u;
constexpr std::size_t kHexByteFormatLen = 4u;
constexpr std::size_t kPrologueLength = 6u;
constexpr std::uint32_t kLzssRva = 0x45910u;
constexpr std::uint32_t kTextboxRva = 0x2A7A0u;
constexpr std::uint32_t kTextboxA1Offset = 474u;
constexpr std::uint32_t kTextboxA3Offset = 493u;
constexpr std::uint32_t kNarrationRva = 0x2BCC0u;
constexpr std::uint32_t kNarrationA1Offset = 468u;
constexpr std::uint32_t kNarrationA3Offset = 491u;
constexpr std::uint8_t kMovEaxFromImm32Opcode = 0xA1u;
constexpr std::uint8_t kMovImm32FromEaxOpcode = 0xA3u;
constexpr std::uint32_t kDialogueTextGlobal = 0x06D92B78u;

const std::vector<std::uint8_t>& DumpedExe() {
    static const std::vector<std::uint8_t> bytes = [] {
        std::vector<std::uint8_t> out;
        const char* path = CC_DUMPED_EXE_PATH;
        if (path == nullptr || *path == '\0') {
            return out;
        }
        try {
            std::ifstream f(std::filesystem::u8path(path),
                            std::ios::binary | std::ios::ate);
            if (!f) {
                return out;
            }
            const std::streamsize sz = f.tellg();
            if (sz <= 0) {
                return out;
            }
            f.seekg(0, std::ios::beg);
            out.resize(static_cast<std::size_t>(sz));
            f.read(reinterpret_cast<char*>(out.data()), sz);
            if (!f) {
                out.clear();
            }
        } catch (const std::exception&) {
            out.clear();
        }
        return out;
    }();
    return bytes;
}

const std::uint8_t* AtRva(std::uint32_t rva, std::size_t need = kDefaultProbeBytes) {
    const auto& b = DumpedExe();
    if (b.empty() || rva + need > b.size()) {
        return nullptr;
    }
    return b.data() + rva;
}

std::uint32_t ReadLeU32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

::testing::AssertionResult BytesMatch(const char* what,
                                      const std::uint8_t* actual,
                                      const std::uint8_t* expected,
                                      std::size_t len) {
    if (std::memcmp(actual, expected, len) == 0) {
        return ::testing::AssertionSuccess();
    }
    auto fail = ::testing::AssertionFailure() << what << " mismatch.\n  expected: ";
    for (std::size_t i = 0; i < len; ++i) {
        char buf[kHexByteFormatLen];
        std::snprintf(buf, sizeof(buf), "%02X ", expected[i]);
        fail << buf;
    }
    fail << "\n  actual:   ";
    for (std::size_t i = 0; i < len; ++i) {
        char buf[kHexByteFormatLen];
        std::snprintf(buf, sizeof(buf), "%02X ", actual[i]);
        fail << buf;
    }
    return fail;
}

}  // namespace

TEST(HookSites, LzssDecompressorPrologue) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(kLzssRva);
    ASSERT_NE(p, nullptr);
    constexpr std::uint8_t kExpected[kPrologueLength] = {
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C};
    EXPECT_TRUE(BytesMatch("LZSS prologue (RVA 0x45910)", p, kExpected, kPrologueLength));
}

TEST(HookSites, TextboxRendererPrologue) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(kTextboxRva);
    ASSERT_NE(p, nullptr);
    constexpr std::uint8_t kExpected[kPrologueLength] = {
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24};
    EXPECT_TRUE(BytesMatch("textbox prologue (RVA 0x2A7A0)", p, kExpected, kPrologueLength));
}

TEST(HookSites, TextboxA1ReadAt474) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(kTextboxRva + kTextboxA1Offset);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(kMovEaxFromImm32Opcode, p[0]) << "expected MOV EAX,[imm32] (A1) at +474";
    EXPECT_EQ(kDialogueTextGlobal, ReadLeU32(p + 1))
        << "imm32 should reference text-ptr global";
}

TEST(HookSites, TextboxA3WriteAt493) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(kTextboxRva + kTextboxA3Offset);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(kMovImm32FromEaxOpcode, p[0]) << "expected MOV [imm32],EAX (A3) at +493";
    EXPECT_EQ(kDialogueTextGlobal, ReadLeU32(p + 1));
}

TEST(HookSites, NarrationRendererPrologue) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(kNarrationRva);
    ASSERT_NE(p, nullptr);
    constexpr std::uint8_t kExpected[kPrologueLength] = {
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18};
    EXPECT_TRUE(BytesMatch("narration prologue (RVA 0x2BCC0)", p, kExpected, kPrologueLength));
}

TEST(HookSites, NarrationA1ReadAt468) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(kNarrationRva + kNarrationA1Offset);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(kMovEaxFromImm32Opcode, p[0]);
    EXPECT_EQ(kDialogueTextGlobal, ReadLeU32(p + 1));
}

TEST(HookSites, NarrationA3WriteAt491) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(kNarrationRva + kNarrationA3Offset);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(kMovImm32FromEaxOpcode, p[0]);
    EXPECT_EQ(kDialogueTextGlobal, ReadLeU32(p + 1));
}

TEST(HookSites, ChoiceHandlersReadTextPtrGlobalAtPlus3) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    constexpr std::uint32_t kChoiceRvas[] = {0x260A0u, 0x26500u, 0x26AC0u};
    for (std::uint32_t rva : kChoiceRvas) {
        const std::uint8_t* p = AtRva(rva + 3u, 5u);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(kMovEaxFromImm32Opcode, p[0]) << "choice RVA 0x" << std::hex << rva;
        EXPECT_EQ(kDialogueTextGlobal, ReadLeU32(p + 1))
            << "choice RVA 0x" << std::hex << rva;
    }
}

TEST(HookSites, Op86SilentBacklogHandlerSignature) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* p = AtRva(bytecode_hooks::kOp86HandlerRva,
                                  bytecode_hooks::kOp86HandlerMinBytes);
    ASSERT_NE(p, nullptr);
    std::uint32_t resolved_global = 0u;
    ASSERT_TRUE(bytecode_hooks::ValidateOp86HandlerBytes(
        p, bytecode_hooks::kOp86HandlerMinBytes, &resolved_global));
    EXPECT_EQ(resolved_global, bytecode_hooks::kPreferredDialogueTextPtrVa);
}

TEST(HookSites, ModalDialogRendererAndUiGlyphScaleSites) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    const std::uint8_t* modal = AtRva(engine_ui_hooks::kModalDialogHookRva,
                                      engine_ui_hooks::kModalDialogStolenLen);
    ASSERT_NE(modal, nullptr);
    EXPECT_TRUE(BytesMatch(
        "modal renderer prologue (RVA 0x1F5B0)", modal,
        engine_ui_hooks::kModalDialogExpectedPrologue.data(),
        engine_ui_hooks::kModalDialogStolenLen));

    // Verify FUN_0082f5b0's first text-draw CALL (+0xE1 = RVA 0x1F691) targets
    // kDrawUiTextHelperRva (0x0E6F0), and verify 0x0E6F0+6's body instructions
    // (0x0E6F0..0x0E6F5 in cc_dumped.exe held a capture-time probe JMP).
    const std::uint8_t* call_site =
        AtRva(engine_ui_hooks::kModalDialogHookRva + 0xE1u, 5u);
    ASSERT_NE(call_site, nullptr);
    EXPECT_EQ(call_site[0], 0xE8u);
    const auto call_rel = static_cast<std::int32_t>(ReadLeU32(call_site + 1));
    const std::uint32_t call_target_rva =
        static_cast<std::uint32_t>(static_cast<std::int32_t>(
            engine_ui_hooks::kModalDialogHookRva + 0xE1u + 5u) + call_rel);
    EXPECT_EQ(call_target_rva, engine_ui_hooks::kDrawUiTextHelperRva);

    const std::uint8_t* draw_body =
        AtRva(engine_ui_hooks::kDrawUiTextHelperRva + 6u, 9u);
    ASSERT_NE(draw_body, nullptr);
    constexpr std::uint8_t kExpectedDrawBody[9] = {
        0xD9u, 0x00u,              // fld  dword ptr [eax] (xy[0])
        0x53u,                     // push ebx
        0x8Bu, 0x5Du, 0x08u,       // mov  ebx, [ebp + 8]  (rgba)
        0xD9u, 0x5Du, 0xF4u,       // fstp dword ptr [ebp - 0x0C]
    };
    EXPECT_TRUE(BytesMatch("draw UI helper body (RVA 0x0E6F6)", draw_body,
                           kExpectedDrawBody, sizeof(kExpectedDrawBody)));

    const std::uint8_t* scale_bytes =
        AtRva(engine_ui_hooks::kUiGlyphScaleFloatRva, sizeof(float));
    ASSERT_NE(scale_bytes, nullptr);
    float scale_val = 0.0f;
    std::memcpy(&scale_val, scale_bytes, sizeof(scale_val));
    EXPECT_NEAR(scale_val, 6.0f / 7.0f, 1e-5f);
}

TEST(HookSites, EngineUiConfigTooltipTableAll20EntriesTranslate) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }
    constexpr std::uint32_t kPreferredBase = 0x00810000u;
    int translated_count = 0;
    for (std::uint32_t rva = 0x0C84A0u; rva <= 0x0C84ECu; rva += 4u) {
        const std::uint8_t* slot = AtRva(rva, 4u);
        ASSERT_NE(slot, nullptr);
        const std::uint32_t target_va = ReadLeU32(slot);
        ASSERT_GT(target_va, kPreferredBase);
        const std::uint32_t str_rva = target_va - kPreferredBase;
        const std::uint8_t* str_ptr = AtRva(str_rva, 1u);
        ASSERT_NE(str_ptr, nullptr);
        const std::string_view jp_cp932(reinterpret_cast<const char*>(str_ptr));
        const auto en = chrome_text_logic::TranslateConfigTooltipCp932(jp_cp932);
        EXPECT_TRUE(en.has_value()) << "Untranslated tooltip at slot RVA 0x"
                                    << std::hex << rva;
        if (en.has_value()) {
            ++translated_count;
            std::size_t line_count = 0u;
            std::size_t pos = 0u;
            while (pos <= en->size()) {
                const std::size_t next = en->find("%N", pos);
                const std::string_view line =
                    (next == std::string::npos)
                        ? std::string_view(*en).substr(pos)
                        : std::string_view(*en).substr(pos, next - pos);
                ++line_count;
                EXPECT_LE(chrome_text_logic::MeasureEngineDisplayUnits(line),
                          chrome_text_logic::kDefaultTooltipMaxCols)
                    << "Tooltip line exceeds 28-col budget at RVA 0x"
                    << std::hex << rva << ": '" << line << "'";
                if (next == std::string::npos) {
                    break;
                }
                pos = next + 2u;
            }
            EXPECT_LE(line_count, 18u)
                << "Tooltip exceeds 18-line panel budget at RVA 0x"
                << std::hex << rva;
        }
    }
    EXPECT_EQ(translated_count, 20);
}

namespace {

class ScopedVirtualBuffer {
public:
    explicit ScopedVirtualBuffer(std::size_t size)
        : ptr_(static_cast<std::uint8_t*>(
              ::VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE))) {}

    ~ScopedVirtualBuffer() {
        if (ptr_ != nullptr) {
            ::VirtualFree(ptr_, 0u, MEM_RELEASE);
        }
    }

    ScopedVirtualBuffer(const ScopedVirtualBuffer&) = delete;
    ScopedVirtualBuffer& operator=(const ScopedVirtualBuffer&) = delete;

    std::uint8_t* get() const noexcept { return ptr_; }

private:
    std::uint8_t* ptr_ = nullptr;
};

void ApplyHighlowRelocsInSlice(std::uint8_t* image_slice,
                               std::size_t slice_bytes,
                               const std::uint8_t* reloc_sec,
                               std::size_t reloc_bytes,
                               std::uint32_t delta,
                               std::uint32_t skip_rva = 0xFFFFFFFFu) {
    std::size_t pos = 0u;
    while (pos + 8u <= reloc_bytes) {
        const std::uint32_t page_rva = ReadLeU32(reloc_sec + pos);
        const std::uint32_t block_size = ReadLeU32(reloc_sec + pos + 4u);
        if (page_rva == 0u || block_size < 8u || pos + block_size > reloc_bytes) {
            break;
        }
        const std::size_t entry_count = (block_size - 8u) / 2u;
        for (std::size_t i = 0u; i < entry_count; ++i) {
            std::uint16_t entry = 0u;
            std::memcpy(&entry, reloc_sec + pos + 8u + i * 2u, sizeof(entry));
            const std::uint16_t type = static_cast<std::uint16_t>(entry >> 12u);
            const std::uint32_t rva = page_rva + static_cast<std::uint32_t>(entry & 0x0FFFu);
            if (type == IMAGE_REL_BASED_HIGHLOW && rva != skip_rva &&
                rva + sizeof(std::uint32_t) <= slice_bytes) {
                std::uint32_t cur = ReadLeU32(image_slice + rva);
                cur += delta;
                std::memcpy(image_slice + rva, &cur, sizeof(cur));
            }
        }
        pos += block_size;
    }
}

}  // namespace

TEST(HookSites, PackerStageReadinessAndAslrEngineUiInstallAndShutdown) {
    if (DumpedExe().empty()) {
        GTEST_SKIP() << "cc_dumped.exe not available at " << CC_DUMPED_EXE_PATH;
    }

    constexpr std::size_t kSliceBytes = 0x0CC000u;
    constexpr std::size_t kRdataStart = 0x09C000u;
    constexpr std::uint32_t kRelocSecRva = 0x0661F000u;
    constexpr std::size_t kRelocSecBytes = 0x049000u;
    constexpr std::uint32_t kDumpedImageBase = 0x00810000u;
    constexpr std::uint32_t kPackedImageBase = 0x00400000u;

    const std::uint8_t* dumped_slice = AtRva(0u, kSliceBytes);
    const std::uint8_t* reloc_sec = AtRva(kRelocSecRva, kRelocSecBytes);
    ASSERT_NE(dumped_slice, nullptr);
    ASSERT_NE(reloc_sec, nullptr);

    ScopedVirtualBuffer scratch(kSliceBytes);
    ASSERT_NE(scratch.get(), nullptr);
    const auto scratch_base =
        reinterpret_cast<std::uintptr_t>(scratch.get());
    const auto scratch_base_u32 = static_cast<std::uint32_t>(scratch_base);

    // Reconstruct the exact pre-.reloc (0x00400000-based) image produced by
    // cc.exe's packer before Section[4] base relocations run.
    std::vector<std::uint8_t> pre_reloc(dumped_slice, dumped_slice + kSliceBytes);
    ApplyHighlowRelocsInSlice(pre_reloc.data(), kSliceBytes, reloc_sec,
                              kRelocSecBytes,
                              kPackedImageBase - kDumpedImageBase);

    engine_ui_hooks::EngineUiController ctrl;

    // Stage 1 (100 ms race): .text unpacked, .rdata/.data still zeroed, pre-.reloc.
    std::memcpy(scratch.get(), pre_reloc.data(), kRdataStart);
    std::memset(scratch.get() + kRdataStart, 0, kSliceBytes - kRdataStart);
    EXPECT_FALSE(engine_ui_hooks::IsEngineImageUnpackedAndRelocated(scratch_base));
    EXPECT_FALSE(ctrl.Install(scratch_base));
    EXPECT_FALSE(ctrl.IsInstalled());
    EXPECT_EQ(ctrl.PatchedPointerCount(), 0u);
    EXPECT_EQ(ctrl.PatchedRdataSlotCount(), 0u);

    // Stage 2: .text/.rdata/.data unpacked, but .reloc pass has not run yet.
    std::memcpy(scratch.get(), pre_reloc.data(), kSliceBytes);
    EXPECT_FALSE(engine_ui_hooks::IsEngineImageUnpackedAndRelocated(scratch_base));
    EXPECT_FALSE(ctrl.Install(scratch_base));
    EXPECT_FALSE(ctrl.IsInstalled());

    // Mid-Stage-3: .reloc applied everywhere EXCEPT the final HIGHLOW entry (0x0CB828).
    ApplyHighlowRelocsInSlice(scratch.get(), kSliceBytes, reloc_sec,
                              kRelocSecBytes,
                              scratch_base_u32 - kPackedImageBase,
                              engine_ui_hooks::kLastHighlowRelocRva);
    EXPECT_FALSE(engine_ui_hooks::IsEngineImageUnpackedAndRelocated(scratch_base));

    // Stage 3 complete: relocate the final entry at RVA 0x0CB828.
    {
        std::uint32_t last_val =
            ReadLeU32(scratch.get() + engine_ui_hooks::kLastHighlowRelocRva);
        last_val += (scratch_base_u32 - kPackedImageBase);
        std::memcpy(scratch.get() + engine_ui_hooks::kLastHighlowRelocRva,
                    &last_val, sizeof(last_val));
    }
    const std::vector<std::uint8_t> clean_relocated(
        scratch.get(), scratch.get() + kSliceBytes);

    ASSERT_TRUE(engine_ui_hooks::IsEngineImageUnpackedAndRelocated(scratch_base));
    ASSERT_TRUE(ctrl.Install(scratch_base));
    EXPECT_TRUE(ctrl.IsInstalled());
    EXPECT_EQ(ctrl.PatchedPointerCount(), 156u);
    EXPECT_EQ(ctrl.PatchedRdataSlotCount(), 14u);
    EXPECT_TRUE(engine_ui_hooks::IsEngineImageUnpackedAndRelocated(scratch_base));

    // Shutdown() restores all .text, .rdata, and .data bytes bit-for-bit.
    ctrl.Shutdown();
    EXPECT_FALSE(ctrl.IsInstalled());
    EXPECT_EQ(std::memcmp(scratch.get(), clean_relocated.data(), kSliceBytes), 0);
}
