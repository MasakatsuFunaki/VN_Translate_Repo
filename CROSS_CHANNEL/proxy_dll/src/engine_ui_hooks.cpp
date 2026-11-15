// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "engine_ui_hooks.h"

#include "chrome_text_logic.h"
#include "log.h"
#include "translator_logic.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace engine_ui_hooks {

namespace {

// Unpacked .rdata bounds in cc.exe (Ghidra SECTION.1: RVA 0x09C000..0x0C6000).
constexpr std::uint32_t kRdataStartRva = 0x09C000u;
constexpr std::uint32_t kRdataEndRva = 0x0C6000u;
constexpr std::size_t kMaxSafeCStringBytes = 512u;

// Writable .data pointer table RVA ranges (inclusive start, inclusive last slot).
struct PointerTableRange {
    std::uint32_t first_rva;
    std::uint32_t last_rva;
    bool is_config_tooltip;
};

constexpr PointerTableRange kPointerTableRanges[] = {
    // 20 Config help descriptions (FUN_008226b0)
    {0x0C84A0u, 0x0C84ECu, true},
    // 37 Scene/Diary titles + primary Save/Load/System modal table
    {0x0C82BCu, 0x0C843Cu, false},
    // 6 Playing Log stat labels + 10 Extra Story / Ending names
    {0x0CA194u, 0x0CA1D0u, false},
    // Secondary Save/Load/Error modal descriptor table
    {0x0CB710u, 0x0CB7FCu, false},
};

// Direct-xref short UI strings in .rdata where English fits in the native
// 4/8/16-byte aligned null-padded slot.
struct InPlaceRdataSlot {
    std::uint32_t rva;
    std::size_t capacity;
    const char* expected_utf8;
};

constexpr InPlaceRdataSlot kInPlaceRdataSlots[] = {
    {0x0B361Cu, 24u, "？？？？？？？？？？"},
    {0x0C2A1Cu, 20u, "？？？？？？？？"},
    {0x0C4340u, 16u, "－－－－－－"},
    {0x0C4388u, 8u, "はい"},
    {0x0C4390u, 4u, "／"},
    {0x0C4394u, 8u, "いいえ"},
    {0x0C439Cu, 12u, "保存中…"},
    {0x0C4408u, 24u, "実績解除【%s : %dG】"},
    {0x0C442Cu, 12u, "SCENE：%s"},
    {0x0C44BCu, 20u, "♪：－－－－－－"},
    {0x0C44D0u, 8u, "♪：%s"},
    {0x0C44D8u, 28u, "シーン　　：－－－－－－"},
    {0x0C44F4u, 16u, "シーン　　：%s"},
    {0x0C4504u, 12u, "時間切れ"},
};

// Native modal text colors in FUN_0082f5b0 (little-endian RGBA bytes):
//   white:     FF FF FF FF
//   highlight: FF FF 7F 7F (0x7F7FFFFFu)
//   dimmed:    FF 5F 5F 5F (0x5F5F5FFFu)
constexpr std::uint32_t kModalColorWhite = 0xFFFFFFFFu;
constexpr std::uint32_t kModalColorHighlight = 0x7F7FFFFFu;
constexpr std::uint32_t kModalColorDim = 0x5F5F5FFFu;

constexpr int kModalLineSlotCount = 3;
constexpr int kModalYesFocusedMode = 1;
constexpr int kModalNoFocusedMode = 2;

constexpr float kFallbackGlyphScale = 0.6f;
constexpr float kMinValidGlyphScale = 0.3f;
constexpr float kMaxValidGlyphScale = 1.2f;
constexpr float kMinReadyGlyphScale = 0.55f;
constexpr float kMaxReadyGlyphScale = 0.90f;
constexpr std::size_t kMaxUnscaledModalUnits = 54u;

constexpr std::uintptr_t kPreferredPeImageBase = 0x00400000u;
constexpr std::size_t kA1InstructionLen = 5u;
constexpr std::size_t kRel32InstructionLen = 5u;
constexpr std::uint8_t kMovEaxFromMoffs32Opcode = 0xA1u;
constexpr std::uint8_t kCallRel32Opcode = 0xE8u;
constexpr std::uint8_t kJmpRel32Opcode = 0xE9u;
constexpr std::uint8_t kNopOpcode = 0x90u;
constexpr std::size_t kMinShortYesBytes = 2u;
constexpr std::size_t kMaxShortYesBytes = 8u;
constexpr std::uint8_t kMinPrintableAscii = 0x21u;
constexpr std::uint8_t kMaxPrintableAscii = 0x7Eu;

constexpr std::size_t kThunkPageBytes = 256u;
constexpr std::size_t kOrigTrampolineOffset = 64u;
constexpr std::size_t kEntryDispatcherOffset = 128u;

using CallDrawUiTextThunkFn = void(__cdecl*)(const char* text,
                                             int layer_id,
                                             float* pos_xy,
                                             const std::uint32_t* rgba,
                                             float scale_x,
                                             float scale_y,
                                             float step_x);

struct ModalHookRuntimeState {
    std::uintptr_t exe_base = 0u;
    CallDrawUiTextThunkFn draw_ui_text = nullptr;
};

ModalHookRuntimeState g_modal_runtime;

float ReadLiveModalGlyphScale(std::uintptr_t exe_base) noexcept {
    if (exe_base == 0u) {
        return kFallbackGlyphScale;
    }
    const auto* scale_ptr =
        reinterpret_cast<const float*>(exe_base + kUiGlyphScaleFloatRva);
    float val = kFallbackGlyphScale;
    __try {
        val = *scale_ptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kFallbackGlyphScale;
    }
    if (val < kMinValidGlyphScale || val > kMaxValidGlyphScale) {
        return kFallbackGlyphScale;
    }
    return val;
}

bool SafeReadBoundedString(const char* ptr, std::size_t& out_len) noexcept {
    out_len = 0u;
    if (ptr == nullptr) {
        return false;
    }
    std::size_t len = 0u;
    __try {
        while (len < kMaxSafeCStringBytes && ptr[len] != '\0') {
            ++len;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (len == 0u || len >= kMaxSafeCStringBytes) {
        return false;
    }
    out_len = len;
    return true;
}

bool SafeReadModalOrigin(const std::int16_t* modal_xy,
                         int& out_x,
                         int& out_y) noexcept {
    if (modal_xy == nullptr) {
        return false;
    }
    __try {
        out_x = static_cast<int>(modal_xy[0]);
        out_y = static_cast<int>(modal_xy[1]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadPointerSlot(const char* const* slot,
                         const char*& out_ptr) noexcept {
    out_ptr = nullptr;
    if (slot == nullptr) {
        return false;
    }
    __try {
        out_ptr = *slot;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadBytes(const void* src, void* dst, std::size_t len) noexcept {
    if (src == nullptr || dst == nullptr) {
        return false;
    }
    __try {
        std::memcpy(dst, src, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadUintptr(std::uintptr_t addr, std::uintptr_t& out_val) noexcept {
    out_val = 0u;
    return SafeReadBytes(reinterpret_cast<const void*>(addr), &out_val,
                         sizeof(out_val));
}

bool SafeReadFloat(std::uintptr_t addr, float& out_val) noexcept {
    out_val = 0.0f;
    return SafeReadBytes(reinterpret_cast<const void*>(addr), &out_val,
                         sizeof(out_val));
}

template <std::size_t N>
bool IsPrologueOrDetourReady(
    std::uintptr_t site_addr,
    const std::array<std::uint8_t, N>& expected_prologue) noexcept {
    std::array<std::uint8_t, N> actual{};
    if (!SafeReadBytes(reinterpret_cast<const void*>(site_addr), actual.data(),
                       actual.size())) {
        return false;
    }
    if (actual == expected_prologue) {
        return true;
    }
    return actual[0] == kJmpRel32Opcode;
}

bool IsTextRendererRelocated(std::uintptr_t exe_base) noexcept {
    const std::uintptr_t a1_site =
        exe_base + kTextRendererHookRva + kTextRendererA1OpcodeOffset;
    std::array<std::uint8_t, kA1InstructionLen> insn{};
    if (!SafeReadBytes(reinterpret_cast<const void*>(a1_site), insn.data(),
                       insn.size())) {
        return false;
    }
    if (insn[0] == kCallRel32Opcode) {
        return true;
    }
    if (insn[0] != kMovEaxFromMoffs32Opcode) {
        return false;
    }
    std::uint32_t imm32 = 0u;
    std::memcpy(&imm32, insn.data() + 1u, sizeof(imm32));
    const auto expected_va =
        static_cast<std::uint32_t>(exe_base + kDialogueTextPtrTargetRva);
    return imm32 == expected_va;
}

bool IsDataStringSlotReady(std::uintptr_t exe_base,
                           std::uint32_t slot_rva,
                           std::uint32_t target_rva) noexcept {
    std::uintptr_t ptr_val = 0u;
    if (!SafeReadUintptr(exe_base + slot_rva, ptr_val) || ptr_val == 0u) {
        return false;
    }
    if (ptr_val == exe_base + target_rva) {
        return true;
    }
    if (exe_base != kPreferredPeImageBase &&
        ptr_val == kPreferredPeImageBase + target_rva) {
        return false;
    }
    std::size_t str_len = 0u;
    if (!SafeReadBoundedString(reinterpret_cast<const char*>(ptr_val),
                               str_len)) {
        return false;
    }
    std::uint8_t first_byte = 0u;
    if (!SafeReadBytes(reinterpret_cast<const void*>(ptr_val), &first_byte,
                       sizeof(first_byte))) {
        return false;
    }
    return first_byte >= kMinPrintableAscii && first_byte <= kMaxPrintableAscii;
}

extern "C" void __cdecl C_RenderLocalizedModal(const std::int16_t* modal_xy,
                                               const char* line1,
                                               int bg_sprite_id,
                                               const char* line2,
                                               const char* line3,
                                               int button_mode) {
    if (g_modal_runtime.draw_ui_text == nullptr) {
        return;
    }

    int offset_x = 0;
    int offset_y = 0;
    if (!SafeReadModalOrigin(modal_xy, offset_x, offset_y)) {
        return;
    }

    const char* raw_lines[kModalLineSlotCount] = {line1, line2, line3};
    std::string translated_storage[kModalLineSlotCount];
    const char* final_lines[kModalLineSlotCount] = {nullptr, nullptr, nullptr};
    int line_count = 0;

    for (int i = 0; i < kModalLineSlotCount; ++i) {
        if (raw_lines[i] == nullptr) {
            continue;
        }
        ++line_count;
        std::size_t raw_len = 0u;
        if (SafeReadBoundedString(raw_lines[i], raw_len)) {
            const std::string_view raw_view(raw_lines[i], raw_len);
            if (auto en = chrome_text_logic::TranslateEngineUiCp932(raw_view)) {
                translated_storage[i] = std::move(*en);
                final_lines[i] = translated_storage[i].c_str();
                continue;
            }
        }
        final_lines[i] = raw_lines[i];
    }

    const float glyph_scale = ReadLiveModalGlyphScale(g_modal_runtime.exe_base);
    const bool has_buttons = (button_mode == kModalYesFocusedMode ||
                              button_mode == kModalNoFocusedMode);
    int current_y = offset_y + chrome_text_logic::ComputeModalFirstLineOffsetY(
                                   line_count, has_buttons);

    const std::uint32_t white = kModalColorWhite;
    for (int i = 0; i < kModalLineSlotCount; ++i) {
        if (final_lines[i] == nullptr) {
            continue;
        }
        std::string_view line_view(final_lines[i]);
        const std::size_t units =
            chrome_text_logic::MeasureEngineDisplayUnits(line_view);
        float step_x = chrome_text_logic::kModalDefaultStepX;
        if (units > kMaxUnscaledModalUnits) {
            step_x = chrome_text_logic::kModalDefaultStepX *
                     (static_cast<float>(kMaxUnscaledModalUnits) /
                      static_cast<float>(units));
        }

        float xy[2] = {
            chrome_text_logic::ComputeModalLineX(line_view, offset_x, step_x),
            static_cast<float>(current_y),
        };
        g_modal_runtime.draw_ui_text(final_lines[i], bg_sprite_id, xy, &white,
                                     glyph_scale, glyph_scale, step_x);
        current_y += chrome_text_logic::kModalLineStrideY;
    }

    if (has_buttons) {
        const auto btn = chrome_text_logic::ComputeModalButtonLayout(
            offset_x, offset_y, chrome_text_logic::kModalDefaultStepX);

        const std::uint32_t yes_color = (button_mode == kModalYesFocusedMode)
                                            ? kModalColorHighlight
                                            : kModalColorDim;
        const std::uint32_t no_color = (button_mode == kModalNoFocusedMode)
                                           ? kModalColorHighlight
                                           : kModalColorDim;

        float yes_xy[2] = {btn.yes_x, btn.y};
        g_modal_runtime.draw_ui_text("Yes", bg_sprite_id, yes_xy, &yes_color,
                                     glyph_scale, glyph_scale,
                                     chrome_text_logic::kModalDefaultStepX);

        float slash_xy[2] = {btn.slash_x, btn.y};
        g_modal_runtime.draw_ui_text("/", bg_sprite_id, slash_xy, &white,
                                     glyph_scale, glyph_scale,
                                     chrome_text_logic::kModalDefaultStepX);

        float no_xy[2] = {btn.no_x, btn.y};
        g_modal_runtime.draw_ui_text("No", bg_sprite_id, no_xy, &no_color,
                                     glyph_scale, glyph_scale,
                                     chrome_text_logic::kModalDefaultStepX);
    }
}

void EmitRel32(std::uint8_t*& cursor, std::uintptr_t target) noexcept {
    const auto rel = static_cast<std::int32_t>(
        target - reinterpret_cast<std::uintptr_t>(cursor + 4));
    std::memcpy(cursor, &rel, sizeof(rel));
    cursor += 4;
}

// Emits the 35-byte __cdecl -> FUN_0081e6f0 register/stack adapter thunk.
std::uint8_t* BuildDrawUiTextAdapter(std::uint8_t* dst,
                                     std::uintptr_t draw_fn_va) noexcept {
    std::uint8_t* p = dst;
    *p++ = 0x55u;                          // push ebp
    *p++ = 0x8Bu; *p++ = 0xECu;            // mov  ebp, esp
    *p++ = 0x57u;                          // push edi
    *p++ = 0x56u;                          // push esi
    *p++ = 0x53u;                          // push ebx
    *p++ = 0xFFu; *p++ = 0x75u; *p++ = 0x20u;  // push [ebp + 0x20] (step_x)
    *p++ = 0xFFu; *p++ = 0x75u; *p++ = 0x1Cu;  // push [ebp + 0x1C] (scale_y)
    *p++ = 0xFFu; *p++ = 0x75u; *p++ = 0x18u;  // push [ebp + 0x18] (scale_x)
    *p++ = 0xFFu; *p++ = 0x75u; *p++ = 0x14u;  // push [ebp + 0x14] (rgba)
    *p++ = 0x8Bu; *p++ = 0x4Du; *p++ = 0x08u;  // mov  ecx, [ebp + 0x08] (text)
    *p++ = 0x8Bu; *p++ = 0x7Du; *p++ = 0x0Cu;  // mov  edi, [ebp + 0x0C] (layer)
    *p++ = 0x8Bu; *p++ = 0x45u; *p++ = 0x10u;  // mov  eax, [ebp + 0x10] (xy)
    *p++ = 0xE8u;                          // call FUN_0081e6f0
    EmitRel32(p, draw_fn_va);
    *p++ = 0x83u; *p++ = 0xC4u; *p++ = 0x10u;  // add  esp, 0x10
    *p++ = 0x5Bu;                          // pop  ebx
    *p++ = 0x5Eu;                          // pop  esi
    *p++ = 0x5Fu;                          // pop  edi
    *p++ = 0x5Du;                          // pop  ebp
    *p++ = 0xC3u;                          // ret
    return p;
}

// Emits the original-prologue trampoline for FUN_0082f5b0 (6 stolen bytes + JMP).
std::uint8_t* BuildModalOriginalTrampoline(std::uint8_t* dst,
                                           const std::uint8_t* site) noexcept {
    std::uint8_t* p = dst;
    std::memcpy(p, site, kModalDialogStolenLen);
    p += kModalDialogStolenLen;
    *p++ = 0xE9u;
    EmitRel32(p, reinterpret_cast<std::uintptr_t>(site + kModalDialogStolenLen));
    return p;
}

// Emits the entry dispatcher jumped to from FUN_0082f5b0:
//   1. Calls C_RenderLocalizedModal(ESI, ECX, bg_sprite_id, line2, line3, mode)
//   2. Calls orig_trampoline(ECX=0, ESI=modal_xy, bg_sprite_id, 0, 0, 0) to
//      queue the native modal background sprite in exact engine z-order.
std::uint8_t* BuildModalEntryDispatcher(
    std::uint8_t* dst,
    std::uintptr_t c_handler_va,
    std::uintptr_t orig_tramp_va) noexcept {
    std::uint8_t* p = dst;
    *p++ = 0x55u;                          // push ebp
    *p++ = 0x8Bu; *p++ = 0xECu;            // mov  ebp, esp
    *p++ = 0x53u;                          // push ebx
    *p++ = 0x56u;                          // push esi
    *p++ = 0x57u;                          // push edi

    *p++ = 0xFFu; *p++ = 0x75u; *p++ = 0x14u;  // push [ebp + 0x14] (button_mode)
    *p++ = 0xFFu; *p++ = 0x75u; *p++ = 0x10u;  // push [ebp + 0x10] (line3)
    *p++ = 0xFFu; *p++ = 0x75u; *p++ = 0x0Cu;  // push [ebp + 0x0C] (line2)
    *p++ = 0xFFu; *p++ = 0x75u; *p++ = 0x08u;  // push [ebp + 0x08] (bg_sprite_id)
    *p++ = 0x51u;                          // push ecx (line1)
    *p++ = 0x56u;                          // push esi (modal_xy)
    *p++ = 0xE8u;                          // call C_RenderLocalizedModal
    EmitRel32(p, c_handler_va);
    *p++ = 0x83u; *p++ = 0xC4u; *p++ = 0x18u;  // add  esp, 0x18

    *p++ = 0x6Au; *p++ = 0x00u;            // push 0 (button_mode = 0)
    *p++ = 0x6Au; *p++ = 0x00u;            // push 0 (line3 = nullptr)
    *p++ = 0x6Au; *p++ = 0x00u;            // push 0 (line2 = nullptr)
    *p++ = 0xFFu; *p++ = 0x75u; *p++ = 0x08u;  // push [ebp + 0x08] (bg_sprite_id)
    *p++ = 0x33u; *p++ = 0xC9u;            // xor  ecx, ecx (line1 = nullptr)
    *p++ = 0xE8u;                          // call orig_trampoline
    EmitRel32(p, orig_tramp_va);
    *p++ = 0x83u; *p++ = 0xC4u; *p++ = 0x10u;  // add  esp, 0x10

    *p++ = 0x5Fu;                          // pop  edi
    *p++ = 0x5Eu;                          // pop  esi
    *p++ = 0x5Bu;                          // pop  ebx
    *p++ = 0x5Du;                          // pop  ebp
    *p++ = 0xC3u;                          // ret
    return p;
}

struct SavedPointerSlot {
    const char** slot = nullptr;
    const char* original = nullptr;
    const char* replaced = nullptr;
};

struct SavedRdataBytes {
    std::uint8_t* addr = nullptr;
    std::vector<std::uint8_t> original;
};

}  // namespace

bool IsEngineImageUnpackedAndRelocated(std::uintptr_t exe_base) noexcept {
    if (exe_base == 0u) {
        return false;
    }
    if (!IsPrologueOrDetourReady(exe_base + kLzssDecompressorRva,
                                 kLzssDecompressorExpectedPrologue)) {
        return false;
    }
    if (!IsPrologueOrDetourReady(exe_base + kModalDialogHookRva,
                                 kModalDialogExpectedPrologue)) {
        return false;
    }
    if (!IsTextRendererRelocated(exe_base)) {
        return false;
    }

    float glyph_scale = 0.0f;
    if (!SafeReadFloat(exe_base + kUiGlyphScaleFloatRva, glyph_scale)) {
        return false;
    }
    if (!(glyph_scale >= kMinReadyGlyphScale &&
          glyph_scale <= kMaxReadyGlyphScale)) {
        return false;
    }

    std::size_t yes_len = 0u;
    if (!SafeReadBoundedString(
            reinterpret_cast<const char*>(exe_base + kModalYesShortSlotRva),
            yes_len)) {
        return false;
    }
    if (yes_len < kMinShortYesBytes || yes_len >= kMaxShortYesBytes) {
        return false;
    }

    std::uintptr_t final_reloc_val = 0u;
    if (!SafeReadUintptr(exe_base + kLastHighlowRelocRva, final_reloc_val)) {
        return false;
    }
    if (final_reloc_val != exe_base + kLastHighlowRelocTargetRva) {
        return false;
    }

    if (!IsDataStringSlotReady(exe_base, kFirstConfigTooltipSlotRva,
                               kFirstConfigTooltipTargetRva)) {
        return false;
    }
    return IsDataStringSlotReady(exe_base, kLastModalSlotRva,
                                 kLastModalSlotTargetRva);
}

struct EngineUiController::Impl {
    bool installed = false;
    bool data_ptrs_patched = false;
    bool rdata_slots_patched = false;
    std::uintptr_t exe_base = 0u;
    std::deque<std::string> owned_strings;
    std::vector<SavedPointerSlot> patched_pointers;
    std::vector<SavedRdataBytes> patched_rdata;

    std::uint8_t* modal_site = nullptr;
    std::uint8_t* thunk_page = nullptr;
    std::array<std::uint8_t, kModalDialogStolenLen> modal_orig_bytes{};
    std::array<std::uint8_t, kModalDialogStolenLen> modal_written_bytes{};
    bool modal_hooked = false;

    ~Impl() { Shutdown(); }

    bool IsRdataAddress(const char* ptr) const noexcept {
        const auto addr = reinterpret_cast<std::uintptr_t>(ptr);
        return addr >= (exe_base + kRdataStartRva) &&
               addr < (exe_base + kRdataEndRva);
    }

    std::size_t PatchDataPointerTables() {
        std::size_t count = 0u;
        for (const auto& range : kPointerTableRanges) {
            for (std::uint32_t rva = range.first_rva; rva <= range.last_rva;
                 rva += sizeof(const char*)) {
                auto* slot = reinterpret_cast<const char**>(exe_base + rva);
                const char* current = nullptr;
                if (!SafeReadPointerSlot(slot, current)) {
                    continue;
                }
                if (current == nullptr || !IsRdataAddress(current)) {
                    continue;
                }

                std::size_t jp_len = 0u;
                if (!SafeReadBoundedString(current, jp_len)) {
                    continue;
                }
                const std::string_view jp_view(current, jp_len);

                auto translated =
                    range.is_config_tooltip
                        ? chrome_text_logic::TranslateConfigTooltipCp932(jp_view)
                        : chrome_text_logic::TranslateEngineUiCp932(jp_view);
                if (!translated.has_value()) {
                    continue;
                }

                DWORD old_prot = 0u;
                if (!::VirtualProtect(slot, sizeof(const char*), PAGE_READWRITE,
                                      &old_prot)) {
                    continue;
                }

                owned_strings.push_back(std::move(*translated));
                const char* replacement = owned_strings.back().c_str();
                *slot = replacement;

                DWORD dummy = 0u;
                ::VirtualProtect(slot, sizeof(const char*), old_prot, &dummy);

                patched_pointers.push_back({slot, current, replacement});
                ++count;
            }
        }
        return count;
    }

    std::size_t PatchShortRdataSlots() {
        std::size_t count = 0u;
        for (const auto& item : kInPlaceRdataSlots) {
            auto* addr = reinterpret_cast<std::uint8_t*>(exe_base + item.rva);
            std::size_t cur_len = 0u;
            if (!SafeReadBoundedString(reinterpret_cast<const char*>(addr),
                                       cur_len)) {
                continue;
            }
            const std::string_view current_view(
                reinterpret_cast<const char*>(addr), cur_len);

            const std::string expected_cp932 =
                translator_logic::Utf8ToCp932(item.expected_utf8);
            if (current_view != expected_cp932) {
                continue;
            }

            const auto en =
                chrome_text_logic::TranslateEngineUiCp932(current_view);
            if (!en.has_value() || en->size() + 1u > item.capacity) {
                continue;
            }

            SavedRdataBytes backup;
            backup.addr = addr;
            backup.original.resize(item.capacity);
            std::memcpy(backup.original.data(), addr, item.capacity);

            DWORD old_prot = 0u;
            if (!::VirtualProtect(addr, item.capacity, PAGE_READWRITE,
                                  &old_prot)) {
                continue;
            }

            std::memset(addr, 0, item.capacity);
            std::memcpy(addr, en->data(), en->size());

            DWORD dummy = 0u;
            ::VirtualProtect(addr, item.capacity, old_prot, &dummy);

            patched_rdata.push_back(std::move(backup));
            ++count;
        }
        return count;
    }

    bool InstallModalDialogHook() noexcept {
        auto* site =
            reinterpret_cast<std::uint8_t*>(exe_base + kModalDialogHookRva);
        std::array<std::uint8_t, kModalDialogStolenLen> probe{};
        if (!SafeReadBytes(site, probe.data(), kModalDialogStolenLen)) {
            Log("ENGINE-UI: AV probing FUN_0082f5b0 at %p", site);
            return false;
        }

        if (probe != kModalDialogExpectedPrologue) {
            Log("ENGINE-UI: unexpected prologue %02X %02X %02X %02X %02X %02X "
                "at RVA 0x%X — skipping modal hook",
                probe[0], probe[1], probe[2], probe[3], probe[4], probe[5],
                kModalDialogHookRva);
            return false;
        }

        auto* page = static_cast<std::uint8_t*>(::VirtualAlloc(
            nullptr, kThunkPageBytes, MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));
        if (page == nullptr) {
            Log("ENGINE-UI: VirtualAlloc failed for modal thunks");
            return false;
        }

        std::uint8_t* draw_adapter = page;
        BuildDrawUiTextAdapter(draw_adapter, exe_base + kDrawUiTextHelperRva);

        std::uint8_t* orig_tramp = page + kOrigTrampolineOffset;
        BuildModalOriginalTrampoline(orig_tramp, site);

        std::uint8_t* entry_disp = page + kEntryDispatcherOffset;
        BuildModalEntryDispatcher(
            entry_disp,
            reinterpret_cast<std::uintptr_t>(&C_RenderLocalizedModal),
            reinterpret_cast<std::uintptr_t>(orig_tramp));

        ::FlushInstructionCache(::GetCurrentProcess(), page, kThunkPageBytes);

        g_modal_runtime.exe_base = exe_base;
        g_modal_runtime.draw_ui_text =
            reinterpret_cast<CallDrawUiTextThunkFn>(draw_adapter);

        DWORD old_prot = 0u;
        if (!::VirtualProtect(site, kModalDialogStolenLen,
                              PAGE_EXECUTE_READWRITE, &old_prot)) {
            g_modal_runtime = {};
            ::VirtualFree(page, 0u, MEM_RELEASE);
            Log("ENGINE-UI: VirtualProtect failed at RVA 0x%X",
                kModalDialogHookRva);
            return false;
        }

        modal_orig_bytes = probe;
        modal_written_bytes[0] = kJmpRel32Opcode;
        const auto rel = static_cast<std::int32_t>(
            reinterpret_cast<std::uintptr_t>(entry_disp) -
            reinterpret_cast<std::uintptr_t>(site + kRel32InstructionLen));
        std::memcpy(modal_written_bytes.data() + 1u, &rel, sizeof(rel));
        modal_written_bytes[kRel32InstructionLen] = kNopOpcode;

        std::memcpy(site, modal_written_bytes.data(), kModalDialogStolenLen);

        DWORD dummy = 0u;
        ::VirtualProtect(site, kModalDialogStolenLen, old_prot, &dummy);
        ::FlushInstructionCache(::GetCurrentProcess(), site,
                                kModalDialogStolenLen);

        modal_site = site;
        thunk_page = page;
        modal_hooked = true;
        Log("ENGINE-UI: modal hook installed at RVA 0x%X (site=%p thunks=%p)",
            kModalDialogHookRva, site, page);
        return true;
    }

    bool Install(std::uintptr_t base) noexcept {
        if (base == 0u) {
            return false;
        }
        if (installed && exe_base == base) {
            return true;
        }
        if (exe_base != 0u && exe_base != base) {
            Shutdown();
        }
        if (!IsEngineImageUnpackedAndRelocated(base)) {
            Log("ENGINE-UI: image at %p not yet unpacked/relocated — deferring "
                "install",
                reinterpret_cast<const void*>(base));
            return false;
        }
        exe_base = base;

        if (!data_ptrs_patched) {
            PatchDataPointerTables();
            data_ptrs_patched = !patched_pointers.empty();
        }
        if (!rdata_slots_patched) {
            PatchShortRdataSlots();
            rdata_slots_patched = !patched_rdata.empty();
        }
        if (!modal_hooked) {
            InstallModalDialogHook();
        }

        installed = (data_ptrs_patched && rdata_slots_patched && modal_hooked);
        Log("ENGINE-UI: patched %zu .data string pointers, %zu .rdata short "
            "slots, modal_hook=%d",
            patched_pointers.size(), patched_rdata.size(),
            modal_hooked ? 1 : 0);
        return installed;
    }

    void Shutdown() noexcept {
        if (modal_hooked && modal_site != nullptr) {
            std::array<std::uint8_t, kModalDialogStolenLen> current_modal{};
            if (SafeReadBytes(modal_site, current_modal.data(),
                              current_modal.size()) &&
                current_modal == modal_written_bytes) {
                DWORD old_prot = 0u;
                if (::VirtualProtect(modal_site, kModalDialogStolenLen,
                                     PAGE_EXECUTE_READWRITE, &old_prot)) {
                    std::memcpy(modal_site, modal_orig_bytes.data(),
                                kModalDialogStolenLen);
                    DWORD dummy = 0u;
                    ::VirtualProtect(modal_site, kModalDialogStolenLen,
                                     old_prot, &dummy);
                    ::FlushInstructionCache(::GetCurrentProcess(), modal_site,
                                            kModalDialogStolenLen);
                }
            }
            g_modal_runtime = {};
            if (thunk_page != nullptr) {
                ::VirtualFree(thunk_page, 0u, MEM_RELEASE);
                thunk_page = nullptr;
            }
            modal_site = nullptr;
            modal_hooked = false;
        }

        for (auto it = patched_rdata.rbegin(); it != patched_rdata.rend();
             ++it) {
            DWORD old_prot = 0u;
            if (::VirtualProtect(it->addr, it->original.size(), PAGE_READWRITE,
                                 &old_prot)) {
                std::memcpy(it->addr, it->original.data(), it->original.size());
                DWORD dummy = 0u;
                ::VirtualProtect(it->addr, it->original.size(), old_prot,
                                 &dummy);
            }
        }
        patched_rdata.clear();
        rdata_slots_patched = false;

        for (auto it = patched_pointers.rbegin(); it != patched_pointers.rend();
             ++it) {
            const char* current_ptr = nullptr;
            if (SafeReadPointerSlot(it->slot, current_ptr) &&
                current_ptr == it->replaced) {
                DWORD old_prot = 0u;
                if (::VirtualProtect(it->slot, sizeof(const char*),
                                     PAGE_READWRITE, &old_prot)) {
                    *it->slot = it->original;
                    DWORD dummy = 0u;
                    ::VirtualProtect(it->slot, sizeof(const char*), old_prot,
                                     &dummy);
                }
            }
        }
        patched_pointers.clear();
        data_ptrs_patched = false;
        owned_strings.clear();
        exe_base = 0u;
        installed = false;
    }
};

EngineUiController::EngineUiController() : impl_(std::make_unique<Impl>()) {}

EngineUiController::EngineUiController(EngineUiController&&) noexcept = default;

EngineUiController& EngineUiController::operator=(
    EngineUiController&&) noexcept = default;

EngineUiController::~EngineUiController() = default;

bool EngineUiController::Install(std::uintptr_t exe_base) noexcept {
    return impl_ != nullptr && impl_->Install(exe_base);
}

void EngineUiController::Shutdown() noexcept {
    if (impl_ != nullptr) {
        impl_->Shutdown();
    }
}

bool EngineUiController::IsInstalled() const noexcept {
    return impl_ != nullptr && impl_->installed;
}

std::size_t EngineUiController::PatchedPointerCount() const noexcept {
    return impl_ != nullptr ? impl_->patched_pointers.size() : 0u;
}

std::size_t EngineUiController::PatchedRdataSlotCount() const noexcept {
    return impl_ != nullptr ? impl_->patched_rdata.size() : 0u;
}

namespace {
EngineUiController& GlobalController() {
    static EngineUiController controller;
    return controller;
}
}  // namespace

bool InstallEngineUiHooks(std::uintptr_t exe_base) noexcept {
    return GlobalController().Install(exe_base);
}

void ShutdownEngineUiHooks() noexcept {
    GlobalController().Shutdown();
}

}  // namespace engine_ui_hooks
