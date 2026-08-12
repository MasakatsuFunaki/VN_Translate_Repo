// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "proportionalizer.h"
#include "patcher_logic.h"
#include "log.h"

// ==========================================================================
// Proportionalizer v2: EXE memory patches for proportional text rendering.
//
// The DDSystem/PIL engine uses THREE width systems that must all agree:
//   1. Rendering (FUN_00401700, RVA 0x1700): glyph bitmap cell width
//      Original: [edi+18h]=21 → cell_advance = 21 * [edi+04h]=24 = 504
//   2. Measurement (FUN_00402860, RVA 0x2860): used by bytecode interpreter
//      for line-break calculation and name-box sizing.
//      Original: half-width = [edi+20h]/2 = 21/2 = 10
//   3. Text-loop advance (FUN_00401840, RVA 0x1840): returned per-character
//      to the rendering loop (FUN_00404a40) which accumulates X cursor.
//      Original: [esi+20h] = 21 (fullwidth), sar → 10 (halfwidth)
//
// If these three don't match, the name box is sized by measurement (#2)
// but filled by text-loop advance (#3). A mismatch causes the last
// character(s) of speaker names to be clipped (e.g. "Michae" not "Michael").
//
// Patch #1 (RVA 0x178C): Rendering cell width 21 → 8
// Patch #2 (RVA 0x2901): Measurement half-width 10 → 8
// Patch #3 (RVA 0x27D9): Text-loop advance base 21 → 16 (→ sar → 8 half)
//
// NOTE on line spacing: do NOT patch it here. The render loop's
// 0x0D-newline advance (RVA 0x4A6C, mov eax,[esi+24h]) never runs for
// dialogue -- the message window draws one glyph per bytecode call with
// per-char x/y computed in the Bootup.dat system scripts. The spacing
// lives in script data (global var 0x2D1) and is patched at decrypt time
// by translator_logic::PatchMessageLineSpacing.
// ==========================================================================

// Patch a sequence of bytes in the EXE's code section at runtime.
// Returns true if the original bytes matched and the patch was applied.
static bool PatchCode(BYTE* base, DWORD rva,
                      const BYTE* expected, const BYTE* replacement, size_t len)
{
    BYTE* addr = base + rva;
    if (!patcher_logic::BytesMatch(addr, expected, len)) {
        Log("PATCH FAIL at RVA 0x%X: bytes mismatch", rva);
        Log("  Expected: %02X %02X %02X", expected[0], expected[1],
            len > 2 ? expected[2] : 0);
        Log("  Found:    %02X %02X %02X", addr[0], addr[1],
            len > 2 ? addr[2] : 0);
        return false;
    }
    DWORD oldProtect;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("PATCH FAIL at RVA 0x%X: VirtualProtect error %u", rva, GetLastError());
        return false;
    }
    memcpy(addr, replacement, len);
    VirtualProtect(addr, len, oldProtect, &oldProtect);
    Log("PATCH OK at RVA 0x%X (%zu bytes)", rva, len);
    return true;
}

void ProportionalizerInit() {
    HMODULE exe = GetModuleHandleW(nullptr);
    BYTE* base = (BYTE*)exe;
    Log("Proportionalizer: ImageBase=0x%X", (DWORD)base);

    // Patch #1: Rendering advance — use fixed 8 instead of fixed cell width 21
    // RVA 0x178C: 8B 5F 18 (mov ebx,[edi+18h]) → 6A 08 5B (push 8; pop ebx)
    {
        const BYTE orig[] = { 0x8B, 0x5F, 0x18 };
        const BYTE repl[] = { 0x6A, 0x08, 0x5B };
        PatchCode(base, 0x178C, orig, repl, 3);
    }

    // Patch #2: Measurement — EN half-width = 8 instead of 10
    // RVA 0x2901: D1 F8 (sar eax,1) → B0 08 (mov al,8)
    {
        const BYTE orig[] = { 0xD1, 0xF8 };
        const BYTE repl[] = { 0xB0, 0x08 };
        PatchCode(base, 0x2901, orig, repl, 2);
    }

    // Patch #3: Text-loop advance — load 16 instead of [esi+20h]=21
    // In FUN_00401840, the per-glyph return value is:
    //   fullwidth: [esi+20h] = 21     halfwidth: [esi+20h] >> 1 = 10
    // After patch:
    //   fullwidth: 16                 halfwidth: 16 >> 1 = 8
    // This makes the text-loop advance (8) match rendering (#1) and
    // measurement (#2), fixing speaker name clipping in the name box.
    // RVA 0x27D9: 8B 76 20 (mov esi,[esi+20h]) → 6A 10 5E (push 16; pop esi)
    {
        const BYTE orig[] = { 0x8B, 0x76, 0x20 };
        const BYTE repl[] = { 0x6A, 0x10, 0x5E };
        PatchCode(base, 0x27D9, orig, repl, 3);
    }
}

void ProportionalizerShutdown() {
    // Patches are in-memory only; no cleanup needed.
}
