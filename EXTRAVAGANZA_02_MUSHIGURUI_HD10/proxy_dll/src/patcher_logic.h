// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pure, Win32-free PE header bounds parsing, x86 detour trampoline helpers,
// and table-driven multi-edition hook/patch descriptors for Mushigurui
// (HD10, HD10_Win7, and Complete Edition).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace patcher_logic {

constexpr std::uint8_t kNopOpcode = 0x90;
constexpr std::uint8_t kRelCallOpcode = 0xE8;
constexpr std::uint8_t kRelJmpOpcode = 0xE9;
constexpr std::size_t kRelBranchInstructionSize = 5;

constexpr std::size_t kHookLen9Bytes = 9;
constexpr std::size_t kHookLen12Bytes = 12;
constexpr std::size_t kMaxHookPrologueLen = 12;
constexpr std::size_t kMaxDetourTrampolineBytes =
    kMaxHookPrologueLen + kRelBranchInstructionSize;

constexpr std::size_t kDefaultPeHeaderMaxBytes = 0x1000;
constexpr std::uint32_t kDefaultTextSectionRva = 0x001000;
constexpr std::uint32_t kImageScnMemExecute = 0x20000000u;

// Structured exception codes raised intentionally by Enigma Protector inside
// non-.text unpacker/VM sections during startup anti-debug probing.
constexpr std::uint32_t kSehIllegalInstruction = 0xC000001Du;
constexpr std::uint32_t kSehIntDivideByZero = 0xC0000094u;
constexpr std::uint32_t kSehPrivInstruction = 0xC0000096u;

// CMyFont member offsets verified across HD10, HD10_Win7, and Complete Edition.
constexpr std::size_t kFontMgrActiveFlagOffset = 0x150;
constexpr std::size_t kFontMgrCachedFontSizeOffset = 0x168;

// TextRenderCore (__thiscall, ret 0x3C = 15 stack DWORD arguments).
constexpr std::size_t kTextRenderCoreStackArgCount = 15;
constexpr std::size_t kTextSplitterStackArgCount = 4;

constexpr std::size_t kPrintMessageSentinelLen = 10;
constexpr std::size_t kPrintMessagePatchCount = 10;
constexpr std::size_t kMaxPatchSiteBytes = 4;

constexpr std::size_t kFontSetupHeadLen = 5;
constexpr std::size_t kFontSetupRelocOffset = 5;
constexpr std::size_t kFontSetupRelocBytes = 4;
constexpr std::size_t kFontSetupTailOffset = kFontSetupRelocOffset + kFontSetupRelocBytes;
constexpr std::size_t kFontSetupTailLen = 6;
constexpr std::size_t kFontSetupSignatureSpan = kFontSetupTailOffset + kFontSetupTailLen;

constexpr std::size_t kRenderCoreSecondaryAnchorOffset = 0x18;
constexpr std::size_t kRenderCoreSecondaryAnchorLen = 5;
constexpr std::size_t kRenderCoreSignatureSpan =
    kRenderCoreSecondaryAnchorOffset + kRenderCoreSecondaryAnchorLen;

constexpr std::size_t kEditionCount = 3;

// Per-edition PE metadata & RVA constants.
constexpr std::uint32_t kHd10TimeDateStamp = 0x5DB572FDu;
constexpr std::uint32_t kHd10TextVirtualSize = 0x150000u;
constexpr std::uint32_t kHd10SizeOfImage = 0x00B23000u;
constexpr std::uint32_t kHd10SentinelRva = 0x05E681u;
constexpr std::uint32_t kHd10TextSplitterRva = 0x063850u;
constexpr std::uint32_t kHd10TextRenderCoreRva = 0x0D7DF0u;
constexpr std::uint32_t kHd10FontSetupRva = 0x0D3E70u;

constexpr std::uint32_t kHd10Win7TimeDateStamp = 0x5DB57310u;
constexpr std::uint32_t kHd10Win7TextVirtualSize = 0x14D000u;
constexpr std::uint32_t kHd10Win7SizeOfImage = 0x00B30000u;
constexpr std::uint32_t kHd10Win7SentinelRva = 0x05E541u;
constexpr std::uint32_t kHd10Win7TextSplitterRva = 0x063710u;
constexpr std::uint32_t kHd10Win7TextRenderCoreRva = 0x0D7400u;
constexpr std::uint32_t kHd10Win7FontSetupRva = 0x0D3480u;

constexpr std::uint32_t kCeTimeDateStamp = 0x537DF74Au;
constexpr std::uint32_t kCeTextVirtualSize = 0x11E000u;
constexpr std::uint32_t kCeSizeOfImage = 0x00B9E000u;
constexpr std::uint32_t kCeSentinelRva = 0x0448CFu;
constexpr std::uint32_t kCeTextSplitterRva = 0x049040u;
constexpr std::uint32_t kCeTextRenderCoreRva = 0x0B0350u;
constexpr std::uint32_t kCeFontSetupRva = 0x0B3A00u;

// PrintMessage patch site deltas relative to sentinel_rva on 2019 builds
// (HD10 and HD10_Win7).
constexpr std::uint32_t kDelta2019FontSize = 0x006u;
constexpr std::uint32_t kDelta2019NextYAddend = 0x066u;
constexpr std::uint32_t kDelta2019FxfFontSizeSkip = 0x012u;
constexpr std::uint32_t kDelta2019FxfNextXSkip = 0x0F8u;
constexpr std::uint32_t kDelta2019FxfNextYSkip = 0x137u;
constexpr std::uint32_t kDelta2019SpaceDefault = 0x1A4u;
constexpr std::uint32_t kDelta2019FxfSpaceSkip = 0x1B0u;
constexpr std::uint32_t kDelta2019FxfLNextYSkip = 0x3E4u;
constexpr std::uint32_t kDelta2019WindowMsgNum = 0xBF8u;
constexpr std::uint32_t kDelta2019FxfWindowMsgNumSkip = 0xC0Au;

// PrintMessage patch site deltas relative to sentinel_rva on Complete Edition.
constexpr std::uint32_t kDeltaCeFontSize = 0x006u;
constexpr std::uint32_t kDeltaCeNextYAddend = 0x063u;
constexpr std::uint32_t kDeltaCeFxfFontSizeSkip = 0x012u;
constexpr std::uint32_t kDeltaCeFxfNextXSkip = 0x0F1u;
constexpr std::uint32_t kDeltaCeFxfNextYSkip = 0x12Eu;
constexpr std::uint32_t kDeltaCeSpaceDefault = 0x199u;
constexpr std::uint32_t kDeltaCeFxfSpaceSkip = 0x1A5u;
constexpr std::uint32_t kDeltaCeFxfLNextYSkip = 0x2D1u;
constexpr std::uint32_t kDeltaCeWindowMsgNum = 0xA72u;
constexpr std::uint32_t kDeltaCeFxfWindowMsgNumSkip = 0xA8Au;

// Shared PrintMessage immediate / opcode bytes.
constexpr std::size_t kSingleBytePatchLen = 1;
constexpr std::size_t kDwordPatchLen = 4;

inline constexpr std::uint8_t kOrigFontSizeByte[kSingleBytePatchLen] = {0x18};
inline constexpr std::uint8_t kPatchedFontSizeByte[kSingleBytePatchLen] = {0x10};
inline constexpr std::uint8_t kOrigNextYAddendByte[kSingleBytePatchLen] = {0x06};
inline constexpr std::uint8_t kPatchedNextYAddendByte[kSingleBytePatchLen] = {0x04};
inline constexpr std::uint8_t kOrigJeRel8Opcode[kSingleBytePatchLen] = {0x74};
inline constexpr std::uint8_t kPatchedJmpRel8Opcode[kSingleBytePatchLen] = {0xEB};
inline constexpr std::uint8_t kOrigSpaceImmediate[kDwordPatchLen] = {0x01, 0x00, 0x00, 0x00};
inline constexpr std::uint8_t kPatchedSpaceImmediate[kDwordPatchLen] = {0xFC, 0xFF, 0xFF, 0xFF};
inline constexpr std::uint8_t kOrigWindowMsgNumByte[kSingleBytePatchLen] = {0x04};
inline constexpr std::uint8_t kPatchedWindowMsgNumByte[kSingleBytePatchLen] = {0x06};

struct SectionSpan {
    std::uint32_t rva = 0;
    std::uint32_t virtual_size = 0;
};

struct PeImageLayout {
    SectionSpan text_section{};
    std::uint32_t size_of_image = 0;
    std::uint32_t time_date_stamp = 0;
};

struct PatchSpec {
    std::uint32_t rva = 0;
    const unsigned char* expected = nullptr;
    const unsigned char* replacement = nullptr;  // null => fill with kNopOpcode
    std::size_t len = 0;
};

struct PatchDeltaSpec {
    std::uint32_t delta_from_sentinel = 0;
    const std::uint8_t* expected = nullptr;
    const std::uint8_t* replacement = nullptr;
    std::size_t len = 0;
};

enum class EditionKind : std::uint8_t {
    HD10 = 0,
    HD10_Win7 = 1,
    CompleteEdition = 2,
};

struct EditionProfile {
    EditionKind kind = EditionKind::HD10;
    const char* name = "";
    std::uint32_t time_date_stamp = 0;
    SectionSpan expected_text_section{};
    std::uint32_t expected_size_of_image = 0;

    std::uint32_t sentinel_rva = 0;
    std::array<std::uint8_t, kPrintMessageSentinelLen> sentinel{};
    std::array<PatchDeltaSpec, kPrintMessagePatchCount> patches{};

    std::uint32_t text_splitter_rva = 0;
    std::size_t text_splitter_hook_len = 0;
    std::array<std::uint8_t, kHookLen9Bytes> text_splitter_prologue{};

    std::uint32_t text_render_core_rva = 0;
    std::size_t text_render_core_hook_len = 0;
    std::array<std::uint8_t, kMaxHookPrologueLen> text_render_core_prologue{};
    bool has_render_core_secondary_anchor = false;
    std::array<std::uint8_t, kRenderCoreSecondaryAnchorLen> render_core_secondary_anchor{};

    std::uint32_t font_setup_rva = 0;
    std::array<std::uint8_t, kFontSetupHeadLen> font_setup_head5{};
    std::array<std::uint8_t, kFontSetupTailLen> font_setup_tail6{};
};

struct ResolvedPrintMessageTarget {
    const EditionProfile* profile = nullptr;
    std::uint32_t sentinel_rva = 0;
    bool used_scan_fallback = false;
};

struct ResolvedTranslatorHooks {
    const EditionProfile* profile = nullptr;
    std::uint32_t text_splitter_rva = 0;
    std::uint32_t text_render_core_rva = 0;
    std::uint32_t font_setup_rva = 0;
    bool used_scan_fallback = false;
};

struct ResolvedEditionTargets {
    const EditionProfile* profile = nullptr;
    std::uint32_t sentinel_rva = 0;
    std::uint32_t text_splitter_rva = 0;
    std::uint32_t text_render_core_rva = 0;
    std::uint32_t font_setup_rva = 0;
    bool used_scan_fallback = false;
};

inline constexpr std::array<PatchDeltaSpec, kPrintMessagePatchCount> kPatches2019 = {{
    {kDelta2019FontSize, kOrigFontSizeByte, kPatchedFontSizeByte, kSingleBytePatchLen},
    {kDelta2019NextYAddend, kOrigNextYAddendByte, kPatchedNextYAddendByte, kSingleBytePatchLen},
    {kDelta2019FxfFontSizeSkip, kOrigJeRel8Opcode, kPatchedJmpRel8Opcode, kSingleBytePatchLen},
    {kDelta2019FxfNextXSkip, kOrigJeRel8Opcode, kPatchedJmpRel8Opcode, kSingleBytePatchLen},
    {kDelta2019FxfNextYSkip, kOrigJeRel8Opcode, kPatchedJmpRel8Opcode, kSingleBytePatchLen},
    {kDelta2019SpaceDefault, kOrigSpaceImmediate, kPatchedSpaceImmediate, sizeof(kOrigSpaceImmediate)},
    {kDelta2019FxfSpaceSkip, kOrigJeRel8Opcode, kPatchedJmpRel8Opcode, kSingleBytePatchLen},
    {kDelta2019FxfLNextYSkip, kOrigJeRel8Opcode, kPatchedJmpRel8Opcode, kSingleBytePatchLen},
    {kDelta2019WindowMsgNum, kOrigWindowMsgNumByte, kPatchedWindowMsgNumByte, kSingleBytePatchLen},
    {kDelta2019FxfWindowMsgNumSkip, kOrigJeRel8Opcode, kPatchedJmpRel8Opcode, kSingleBytePatchLen},
}};

inline constexpr std::array<PatchDeltaSpec, kPrintMessagePatchCount> kPatchesCe = {{
    {kDeltaCeFontSize, kOrigFontSizeByte, kPatchedFontSizeByte, kSingleBytePatchLen},
    {kDeltaCeNextYAddend, kOrigNextYAddendByte, kPatchedNextYAddendByte, kSingleBytePatchLen},
    {kDeltaCeFxfFontSizeSkip, kOrigJeRel8Opcode, kPatchedJmpRel8Opcode, kSingleBytePatchLen},
    {kDeltaCeFxfNextXSkip, kOrigJeRel8Opcode, kPatchedJmpRel8Opcode, kSingleBytePatchLen},
    {kDeltaCeFxfNextYSkip, kOrigJeRel8Opcode, kPatchedJmpRel8Opcode, kSingleBytePatchLen},
    {kDeltaCeSpaceDefault, kOrigSpaceImmediate, kPatchedSpaceImmediate, sizeof(kOrigSpaceImmediate)},
    {kDeltaCeFxfSpaceSkip, kOrigJeRel8Opcode, kPatchedJmpRel8Opcode, kSingleBytePatchLen},
    {kDeltaCeFxfLNextYSkip, kOrigJeRel8Opcode, kPatchedJmpRel8Opcode, kSingleBytePatchLen},
    {kDeltaCeWindowMsgNum, kOrigWindowMsgNumByte, kPatchedWindowMsgNumByte, kSingleBytePatchLen},
    {kDeltaCeFxfWindowMsgNumSkip, kOrigJeRel8Opcode, kPatchedJmpRel8Opcode, kSingleBytePatchLen},
}};

inline constexpr EditionProfile kHd10Profile = {
    EditionKind::HD10,
    "HD10",
    kHd10TimeDateStamp,
    SectionSpan{kDefaultTextSectionRva, kHd10TextVirtualSize},
    kHd10SizeOfImage,
    kHd10SentinelRva,
    {0xC7, 0x87, 0x80, 0x46, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00},
    kPatches2019,
    kHd10TextSplitterRva,
    kHookLen9Bytes,
    {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x3C, 0x05, 0x00, 0x00},
    kHd10TextRenderCoreRva,
    kHookLen12Bytes,
    {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x81, 0xEC, 0x74, 0x02, 0x00, 0x00},
    false,
    {0x00, 0x00, 0x00, 0x00, 0x00},
    kHd10FontSetupRva,
    {0x55, 0x8B, 0xEC, 0x8B, 0x15},
    {0xB8, 0x1F, 0x85, 0xEB, 0x51, 0x56},
};

inline constexpr EditionProfile kHd10Win7Profile = {
    EditionKind::HD10_Win7,
    "HD10_Win7",
    kHd10Win7TimeDateStamp,
    SectionSpan{kDefaultTextSectionRva, kHd10Win7TextVirtualSize},
    kHd10Win7SizeOfImage,
    kHd10Win7SentinelRva,
    {0xC7, 0x87, 0x80, 0x46, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00},
    kPatches2019,
    kHd10Win7TextSplitterRva,
    kHookLen9Bytes,
    {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x3C, 0x05, 0x00, 0x00},
    kHd10Win7TextRenderCoreRva,
    kHookLen12Bytes,
    {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x81, 0xEC, 0x74, 0x02, 0x00, 0x00},
    false,
    {0x00, 0x00, 0x00, 0x00, 0x00},
    kHd10Win7FontSetupRva,
    {0x55, 0x8B, 0xEC, 0x8B, 0x15},
    {0xB8, 0x1F, 0x85, 0xEB, 0x51, 0x56},
};

inline constexpr EditionProfile kCompleteEditionProfile = {
    EditionKind::CompleteEdition,
    "CompleteEdition",
    kCeTimeDateStamp,
    SectionSpan{kDefaultTextSectionRva, kCeTextVirtualSize},
    kCeSizeOfImage,
    kCeSentinelRva,
    {0xC7, 0x86, 0x74, 0x46, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00},
    kPatchesCe,
    kCeTextSplitterRva,
    kHookLen9Bytes,
    {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x34, 0x05, 0x00, 0x00},
    kCeTextRenderCoreRva,
    kHookLen9Bytes,
    {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x48, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00},
    true,
    {0x8B, 0xD1, 0x8B, 0x4D, 0x18},
    kCeFontSetupRva,
    {0x55, 0x8B, 0xEC, 0x8B, 0x15},
    {0x56, 0x8B, 0xF1, 0x8B, 0x4D, 0x08},
};

inline constexpr std::array<EditionProfile, kEditionCount> kEditionProfiles = {{
    kHd10Profile,
    kHd10Win7Profile,
    kCompleteEditionProfile,
}};

const EditionProfile& GetEditionProfile(EditionKind kind) noexcept;

bool BytesMatch(const unsigned char* addr,
                const unsigned char* expected,
                std::size_t len);

void FillNops(unsigned char* dst, std::size_t len);

std::int32_t ComputeRel32(std::uintptr_t instr_start,
                          std::uintptr_t target);

unsigned char* EmitRelOp32(unsigned char* out,
                           unsigned char opcode,
                           std::uintptr_t target);

inline unsigned char* EmitRelCall32(unsigned char* out,
                                    std::uintptr_t target) {
    return EmitRelOp32(out, kRelCallOpcode, target);
}

inline unsigned char* EmitRelJmp32(unsigned char* out,
                                   std::uintptr_t target) {
    return EmitRelOp32(out, kRelJmpOpcode, target);
}

unsigned char* EmitBytes(unsigned char* out,
                         const unsigned char* src,
                         std::size_t len);

bool LooksLikeAscii(std::uint32_t val);

bool IsValidSpec(const PatchSpec& s);

// Emit a displaced-prologue trampoline (`hook_len` bytes copied from
// `hook_site`, followed by a 5-byte `E9 rel32` jump to `hook_site_va + hook_len`).
// Supports both 9-byte and 12-byte prologues across arbitrary ASLR ImageBase
// addresses. Returns the number of bytes written (`hook_len + 5`), or 0 on
// invalid arguments.
std::size_t BuildDetourTrampoline(std::uint8_t* trampoline_buf,
                                  std::uintptr_t trampoline_va,
                                  const std::uint8_t* hook_site_bytes,
                                  std::uintptr_t hook_site_va,
                                  std::size_t hook_len);

// Overwrite `hook_site_buf[0..hook_len)` with `E9 rel32` to `detour_target_va`
// followed by `hook_len - 5` NOPs (`0x90`). `hook_site_va` is the runtime
// virtual address of `hook_site_buf` (decoupled from the buffer pointer so
// arbitrary ASLR virtual addresses can be tested in memory).
bool WriteDetourJump(std::uint8_t* hook_site_buf,
                     std::uintptr_t hook_site_va,
                     std::size_t hook_len,
                     std::uintptr_t detour_target_va);

std::optional<PeImageLayout> ParsePeImageLayout(const std::uint8_t* image_base,
                                                std::size_t max_header_bytes);

std::optional<SectionSpan> ParsePeTextSectionBounds(const std::uint8_t* image_base,
                                                    std::size_t max_header_bytes);

bool IsAddressInsideSection(std::uintptr_t addr,
                            std::uintptr_t image_base,
                            SectionSpan span) noexcept;

// Returns true for Enigma Protector startup SEH probe exceptions
// (INT_DIVIDE_BY_ZERO, ILLEGAL_INSTRUCTION, PRIV_INSTRUCTION) raised inside the
// main executable image outside `.text` (e.g. sec[7] VM/unpacker section).
// Real crashes inside `.text`, inside `winmm.dll`, or ACCESS_VIOLATION return
// false so CrashHandler logs them in full.
bool IsBenignPackerSehException(std::uint32_t exception_code,
                                std::uintptr_t fault_addr,
                                std::uintptr_t image_base,
                                const PeImageLayout& pe_layout) noexcept;

bool MatchesPrintMessageCluster(const EditionProfile& profile,
                                const std::uint8_t* image_base,
                                SectionSpan text_span,
                                std::uint32_t sentinel_rva) noexcept;

bool MatchesTextSplitterSignature(const EditionProfile& profile,
                                  const std::uint8_t* image_base,
                                  SectionSpan text_span,
                                  std::uint32_t candidate_rva) noexcept;

bool MatchesTextRenderCoreSignature(const EditionProfile& profile,
                                    const std::uint8_t* image_base,
                                    SectionSpan text_span,
                                    std::uint32_t candidate_rva) noexcept;

bool MatchesFontSetupSignature(const EditionProfile& profile,
                               const std::uint8_t* image_base,
                               SectionSpan text_span,
                               std::uint32_t candidate_rva) noexcept;

std::array<PatchSpec, kPrintMessagePatchCount> BuildPrintMessagePatchSpecs(
    const EditionProfile& profile,
    std::uint32_t sentinel_rva) noexcept;

// Fast-path O(1) expected-RVA check followed by unique `.text` scan fallback.
std::optional<ResolvedPrintMessageTarget> ResolvePrintMessageTarget(
    const std::uint8_t* image_base,
    SectionSpan text_span,
    std::uint32_t time_date_stamp = 0) noexcept;

std::optional<ResolvedTranslatorHooks> ResolveTranslatorHooks(
    const std::uint8_t* image_base,
    SectionSpan text_span,
    std::uint32_t time_date_stamp = 0) noexcept;

std::optional<ResolvedEditionTargets> ResolveEditionTargets(
    const std::uint8_t* image_base,
    SectionSpan text_span,
    std::uint32_t time_date_stamp = 0) noexcept;

// Pure in-memory PrintMessage patch transaction for unit testing against
// synthetic or dumped `.text` buffers without calling VirtualProtect.
bool ApplyPrintMessagePatchesInMemory(std::uint8_t* image_base,
                                      SectionSpan text_span,
                                      const EditionProfile& profile,
                                      std::uint32_t sentinel_rva) noexcept;

}  // namespace patcher_logic
