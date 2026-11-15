// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// bytecode_hooks.h
//
// Post-unpack inline hooks for sn.bin bytecode handlers not covered by the
// main dialogue/narration/choice renderers — specifically opcode 0x86
// (FUN_0083efb0, RVA 0x2EFB0), which appends Miki's 31 voiced off-screen lines
// directly into the 1024-entry backlog ring buffer without invoking the
// dialogue renderer.

#include "translator_logic.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace bytecode_hooks {

// --- RVA & struct layout constants for op 0x86 (FUN_0083efb0) ---------------
//
// Disassembly at RVA 0x2EFB0 (preferred ImageBase = 0x00810000):
//   +0x00: 53                         push ebx
//   +0x01: 8B 1D <g_backlogWriteIdx>  mov  ebx, dword ptr [0x06C2A870]
//   +0x07: 56                         push esi
//   +0x08: 8B F3                      mov  esi, ebx
//   +0x0A: 57                         push edi
//   +0x0B: 69 F6 84 05 00 00          imul esi, esi, 0x584
//   ...
//   +0x22: 8B 3D <g_textPtrGlobal>    mov  edi, dword ptr [0x06D92B78]
//   +0x28: 0F BF C8                   movsx ecx, ax
//   +0x2B: 8D 96 <g_backlogSlots+8>   lea  edx, [0x06C2B888 + esi]
//   +0x31: 89 8E <g_backlogSlots+4>   mov  [0x06C2B884 + esi], ecx
//   +0x37..+0x56: strcpy slot.text from edi & advance edi past NUL
//   +0x57: 89 3D <g_textPtrGlobal>    mov  dword ptr [0x06D92B78], edi
//   +0x5D: 8D BE <g_backlogSlots+512> lea  edi, [0x06C2BA80 + esi]
//   +0x63..+0x89: copy 0x384-byte snapshot, advance ring index, pop & ret
constexpr std::uint32_t kOp86HandlerRva      = 0x2EFB0u;
constexpr std::size_t   kOp86HandlerMinBytes = 0x8Au;
constexpr std::size_t   kOp86PreHookOffset   = 0x22u;
constexpr std::size_t   kOp86PostHookOffset  = 0x57u;
constexpr std::size_t   kOp86HookPatchLen    = 6u;

// Preferred-base (ImageBase = 0x00810000) globals in unpacked cc_dumped.exe.
constexpr std::uint32_t kPreferredBacklogWriteIdxVa = 0x06C2A870u;
constexpr std::uint32_t kPreferredBacklogSlotBaseVa = 0x06C2B880u;
constexpr std::uint32_t kPreferredDialogueTextPtrVa = 0x06D92B78u;

// Per-slot layout in the 1024-entry backlog ring buffer:
//   +0x000: int32_t kind      (4 bytes)
//   +0x004: int32_t voice_id  (4 bytes)
//   +0x008: char    text[504] (0x1F8 bytes: 0x06C2B888 .. 0x06C2BA7F)
//   +0x200: uint8_t snap[900] (0x384 bytes: 0x06C2BA80 .. 0x06C2BE03)
constexpr std::size_t kOp86RingEntryCount     = 1024u;
constexpr std::size_t kOp86SlotStride         = 0x584u;
constexpr std::size_t kOp86SlotKindOffset     = 0x000u;
constexpr std::size_t kOp86SlotVoiceOffset    = 0x004u;
constexpr std::size_t kOp86SlotTextOffset     = 0x008u;
constexpr std::size_t kOp86SlotSnapshotOffset = 0x200u;
constexpr std::size_t kOp86SlotTextCapacity   =
    kOp86SlotSnapshotOffset - kOp86SlotTextOffset;  // 504 bytes (0x1F8)

static_assert(kOp86SlotTextCapacity == translator_logic::kBacklogSlotTextCapacity,
              "op 0x86 slot text capacity must match translator_logic");

// Pure ASLR-tolerant byte validator for FUN_0083efb0 (op 0x86).
// Verifies the relocation-invariant prologue/anchor bytes and confirms that
// +0x22 (`MOV EDI,[imm32]`) and +0x57 (`MOV [imm32],EDI`) address the exact
// same non-zero script-cursor global. Writes that resolved global VA to
// `*out_text_ptr_global_va` when non-null.
bool ValidateOp86HandlerBytes(const std::uint8_t* fn_bytes,
                              std::size_t avail_bytes,
                              std::uint32_t* out_text_ptr_global_va = nullptr) noexcept;

// RAII / PIMPL controller owning the executable trampoline page and original
// stolen instruction bytes for bytecode menu/backlog hooks.
class BytecodeHookController {
public:
    BytecodeHookController();
    ~BytecodeHookController();

    BytecodeHookController(const BytecodeHookController&) = delete;
    BytecodeHookController& operator=(const BytecodeHookController&) = delete;
    BytecodeHookController(BytecodeHookController&&) noexcept;
    BytecodeHookController& operator=(BytecodeHookController&&) noexcept;

    bool Install(std::uintptr_t exe_base,
                 const translator_logic::TranslationMap* map);
    void Shutdown() noexcept;
    bool IsInstalled() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Process-wide lifecycle entry points invoked by translator.cpp after the
// packed executable finishes unpacking its .text section.
bool InstallBytecodeMenuHooks(std::uintptr_t exe_base,
                              const translator_logic::TranslationMap* map);
void ShutdownBytecodeMenuHooks();

}  // namespace bytecode_hooks
