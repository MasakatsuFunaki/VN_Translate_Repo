// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "patcher.h"
#include "patcher_logic.h"
#include "log.h"
#include <cstring>
#include <intrin.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

// ==========================================================================
// Runtime patcher for EXTRAVAGANZA ~Mushi Mederu Shoujo~ (BLACKCyc engine).
//
// Problem: The packed EXE decompresses code at runtime. Font size and line
// spacing are set by FXF config files (fontSize=24, nextY=30). English
// translations need smaller text to fit in the textbox without cutoff.
//
// Solution: After the unpacker finishes, patch the PrintMessage constructor
// (FUN_004f8880) to:
//   1. Use fontSize=14 instead of 24
//   2. Calculate nextY = fontSize + 4 = 18 (instead of + 6 = 30)
//   3. NOP out FXF config reads that would override these values
//
// This replaces the old analysys/modify_fxf.py approach (patching FXF files)
// with in-memory code patching, which is cleaner and doesn't modify game files.
//
// All RVAs are from the runtime memory dump (mushiEx_CE_dumped.exe).
// The unpacker restores code to the same virtual addresses.
// ==========================================================================

static HANDLE g_patchThread = nullptr;
static volatile bool g_shutdownRequested = false;

// Forward declarations
static void InstallFontHook();

// ── Logging hook for FUN_004f7650 (text splitter) ───────────────────────
// Hook captures `this` pointer (ecx in thiscall) and logs key object fields
// and the text being processed on each call.
//
// PrintMessage object layout (offsets from this):
//   +0x01CC = lineCount (after splitting)
//   +0x03D4 = line buffer 0 (256 bytes each, line[i] at +0x3D4 + i*0x100)
//   +0x4648 = fontSize
//   +0x4654 = nextX
//   +0x4658 = nextY
//   +0x466C = space
//   +0x46CC = windowMessageNumber
//   +0x46D0 = fullMessageNumber
//   +0x45DC = mode flag (0x22220001 = window mode)

static BYTE* g_textSplitterReturn = nullptr;  // address to jump back after hook
static int g_hookCallCount = 0;

// Called from our inline hook trampoline with the this pointer
static void __cdecl LogTextSplitter(BYTE* thisPtr)
{
    g_hookCallCount++;

    __try {
        DWORD fontSize = *(DWORD*)(thisPtr + 0x4648);
        DWORD nextY    = *(DWORD*)(thisPtr + 0x4658);
        DWORD space    = *(DWORD*)(thisPtr + 0x466C);
        DWORD wmn      = *(DWORD*)(thisPtr + 0x46CC);
        DWORD fmn      = *(DWORD*)(thisPtr + 0x46D0);
        DWORD mode     = *(DWORD*)(thisPtr + 0x45DC);
        DWORD lineCnt  = *(DWORD*)(thisPtr + 0x01CC);

        Log("=== TextSplitter call #%d ===", g_hookCallCount);
        Log("  this=0x%p mode=0x%08X (window=%s)", thisPtr, mode,
            (mode == 0x22220001) ? "YES" : "NO");
        Log("  fontSize=%d nextY=%d space=%d", fontSize, nextY, space);
        Log("  windowMsgNum=%d fullMsgNum=%d lineCount(pre)=%d",
            wmn, fmn, lineCnt);

        // Log the second argument (text pointer) - it's on the stack
        // In thiscall, arg1 is at [esp+4] after the return address is pushed.
        // But we're called from the trampoline so we don't have direct access.
        // Instead, log the line buffers AFTER the function runs.
        // For now, log what we can see.

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("  (exception reading this=0x%p)", thisPtr);
    }
}

// Called AFTER the text splitter returns, to log results.
// We hook the capping code instead — log right before the cap decision.
static void __cdecl LogBeforeCap(BYTE* thisPtr)
{
    __try {
        DWORD lineCnt = *(DWORD*)(thisPtr + 0x01CC);
        DWORD wmn     = *(DWORD*)(thisPtr + 0x46CC);
        DWORD fmn     = *(DWORD*)(thisPtr + 0x46D0);
        DWORD mode    = *(DWORD*)(thisPtr + 0x45DC);

        Log("  >> CAP CHECK: lineCount=%d windowMsgNum=%d fullMsgNum=%d mode=0x%08X",
            lineCnt, wmn, fmn, mode);

        if (mode == 0x22220001) {
            Log("  >> WINDOW MODE: lineCount=%d vs cap=%d => %s",
                lineCnt, wmn, (lineCnt > (int)wmn) ? "WILL CAP!" : "ok");
        } else {
            Log("  >> FULL MODE: lineCount=%d vs cap=%d => %s",
                lineCnt, fmn, (lineCnt > (int)fmn) ? "WILL CAP!" : "ok");
        }

        // Log first 10 line buffers
        for (int i = 0; i < 10 && i < (int)lineCnt; i++) {
            char* linePtr = (char*)(thisPtr + 0x03D4 + i * 0x100);
            char buf[128] = {0};
            strncpy(buf, linePtr, 120);
            buf[120] = '\0';
            Log("  >> line[%d]: \"%s\"", i, buf);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("  >> (exception in LogBeforeCap)");
    }
}

// ── Chart popup text rendering hook ─────────────────────────────────
// Hook FUN_00407340 (chart popup renderer) to log text rendering params.
// FUN_00407340 is __fastcall: ecx = chart object pointer.
// Chart object layout for popup:
//   +0x408 = page index
//   +0x40C = popup width
//   +0x410 = popup height
//   +0x414+page*8 = popup x
//   +0x418+page*8 = popup y
//   +0x424+page*8 = text x
//   +0x428+page*8 = text base y
//   +0x270 = selected node ID
static int g_chartLogCount = 0;

static void __cdecl LogChartPopup(BYTE* chartObj)
{
    __try {
        int nodeId = *(int*)(chartObj + 0x270);
        if (nodeId == -1) return;

        int page   = *(int*)(chartObj + 0x408);
        int popW   = *(int*)(chartObj + 0x40C);
        int popH   = *(int*)(chartObj + 0x410);
        int popX   = *(int*)(chartObj + 0x414 + page * 8);
        int popY   = *(int*)(chartObj + 0x418 + page * 8);
        int txtX   = *(int*)(chartObj + 0x424 + page * 8);
        int txtY   = *(int*)(chartObj + 0x428 + page * 8);

        // Only log occasionally to avoid flooding
        g_chartLogCount++;
        if (g_chartLogCount <= 5 || (g_chartLogCount % 100) == 0) {
            Log("CHART POPUP #%d: node=%d page=%d", g_chartLogCount, nodeId, page);
            Log("  popup: x=%d y=%d w=%d h=%d", popX, popY, popW, popH);
            Log("  text:  x=%d y=%d", txtX, txtY);
            Log("  usable text width = %d px, at fontSize=14+1 spacing = %d fullwidth chars",
                popW - (txtX - popX) * 2, (popW - (txtX - popX) * 2) / 15);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("CHART POPUP: exception reading chartObj=0x%p", chartObj);
    }
}

static BYTE* g_chartTrampoline = nullptr;

// Install hook on FUN_00407340 (chart popup renderer).
// Original bytes at RVA 0x007340:
//   55          push ebp
//   8B EC       mov ebp, esp
//   83 EC 2C    sub esp, 0x2C
// Total: 6 bytes. We replace with JMP to trampoline + 1 NOP.
static bool InstallChartPopupHook(BYTE* base)
{
    const DWORD hookRVA = 0x007340;
    const size_t hookLen = 6;
    BYTE* hookSite = base + hookRVA;
    BYTE* returnAddr = hookSite + hookLen;

    const BYTE expected[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C };
    if (memcmp(hookSite, expected, hookLen) != 0) {
        Log("CHART HOOK: bytes mismatch at RVA 0x%X", hookRVA);
        return false;
    }

    g_chartTrampoline = (BYTE*)VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE,
                                             PAGE_EXECUTE_READWRITE);
    if (!g_chartTrampoline) return false;

    BYTE* p = g_chartTrampoline;

    // Save ecx (chart object pointer — fastcall first arg)
    *p++ = 0x51;  // push ecx

    // pushad
    *p++ = 0x60;

    // push ecx (chart object as arg to LogChartPopup)
    *p++ = 0x51;

    // call LogChartPopup
    p = patcher_logic::EmitRelCall32(
            p, reinterpret_cast<std::uintptr_t>(&LogChartPopup));

    // add esp, 4
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;

    // popad
    *p++ = 0x61;

    // Restore ecx
    *p++ = 0x59;  // pop ecx

    // Execute original 6 bytes:
    //   push ebp; mov ebp,esp; sub esp,0x2C
    p = patcher_logic::EmitBytes(p, expected, hookLen);

    // jmp back to returnAddr
    p = patcher_logic::EmitRelJmp32(
            p, reinterpret_cast<std::uintptr_t>(returnAddr));

    // Patch hook site
    DWORD oldProtect;
    VirtualProtect(hookSite, hookLen, PAGE_EXECUTE_READWRITE, &oldProtect);
    patcher_logic::EmitRelJmp32(
        hookSite, reinterpret_cast<std::uintptr_t>(g_chartTrampoline));
    hookSite[5] = 0x90;  // NOP the 6th byte (JMP rel32 is only 5 wide)
    VirtualProtect(hookSite, hookLen, oldProtect, &oldProtect);

    Log("CHART HOOK: installed at RVA 0x%X", hookRVA);
    return true;
}

// Executable trampoline page for our hook code
static BYTE* g_trampoline = nullptr;

// Install inline hook at the capping check site (RVA 0x0F7E96).
// Original code at 0x0F7E96:
//   81 B9 DC 45 00 00 01 00 22 22   cmp [ecx+0x45DC], 0x22220001
//   75 37                            jne +0x37 (fullscreen branch)
// Total: 12 bytes. We replace with a JMP to our trampoline which:
//   1. Saves registers
//   2. Calls LogBeforeCap(this)
//   3. Restores registers
//   4. Executes the original 12 bytes
//   5. Jumps back to 0x0F7EA2
static bool InstallCapHook(BYTE* base)
{
    const DWORD hookRVA = 0x0F7E96;
    const size_t hookLen = 12;  // bytes we're overwriting
    BYTE* hookSite = base + hookRVA;
    BYTE* returnAddr = hookSite + hookLen;  // 0x0F7EA2

    // Verify expected bytes
    const BYTE expected[] = {
        0x81, 0xB9, 0xDC, 0x45, 0x00, 0x00, 0x01, 0x00, 0x22, 0x22,
        0x75, 0x37
    };
    if (memcmp(hookSite, expected, hookLen) != 0) {
        Log("CAP HOOK: bytes mismatch at RVA 0x%X", hookRVA);
        return false;
    }

    // Allocate executable memory for the trampoline
    g_trampoline = (BYTE*)VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
    if (!g_trampoline) {
        Log("CAP HOOK: VirtualAlloc failed");
        return false;
    }

    // Build trampoline:
    //   pushad
    //   push ecx              ; this pointer (ecx from thiscall convention - 
    //                         ; stored in [ebp-15Ch] but ecx still holds it here)
    //   Actually, at this point ecx = this (loaded at 0F7E90: mov ecx,[ebp-15Ch])
    //   call LogBeforeCap
    //   add esp, 4
    //   popad
    //   ; execute original 12 bytes
    //   cmp dword ptr [ecx+0x45DC], 0x22220001
    //   jne +0x37  (but relative to trampoline, need to fix)
    //   jmp returnAddr

    BYTE* p = g_trampoline;

    // pushad
    *p++ = 0x60;

    // push ecx (this ptr as arg to LogBeforeCap)
    *p++ = 0x51;

    // call LogBeforeCap (relative call)
    p = patcher_logic::EmitRelCall32(
            p, reinterpret_cast<std::uintptr_t>(&LogBeforeCap));

    // add esp, 4 (clean up push ecx)
    *p++ = 0x83;
    *p++ = 0xC4;
    *p++ = 0x04;

    // popad
    *p++ = 0x61;

    // Execute original 12 bytes:
    // cmp dword ptr [ecx+0x45DC], 0x22220001
    p = patcher_logic::EmitBytes(p, expected, 10);  // the cmp instruction

    // The jne +0x37 was relative to hookSite+10. We need it to jump to
    // the original fullscreen branch target: hookSite + 10 + 2 + 0x37 = base + 0x0F7ED9
    // From our trampoline, we use jne to an absolute jump.
    // jne to fullscreen_trampoline
    *p++ = 0x75;
    *p++ = 0x05;  // +5 to skip the next jmp

    // jmp returnAddr (window mode path continues at 0x0F7EA2)
    p = patcher_logic::EmitRelJmp32(
            p, reinterpret_cast<std::uintptr_t>(returnAddr));

    // fullscreen_trampoline: jmp to original fullscreen branch (base + 0x0F7ED9)
    BYTE* fullscreenAddr = base + 0x0F7ED9;
    p = patcher_logic::EmitRelJmp32(
            p, reinterpret_cast<std::uintptr_t>(fullscreenAddr));

    Log("CAP HOOK: trampoline at 0x%p, size=%d bytes", g_trampoline, (int)(p - g_trampoline));

    // Now patch the hook site: JMP to trampoline + NOP remaining bytes
    DWORD oldProtect;
    VirtualProtect(hookSite, hookLen, PAGE_EXECUTE_READWRITE, &oldProtect);

    patcher_logic::EmitRelJmp32(
        hookSite, reinterpret_cast<std::uintptr_t>(g_trampoline));
    // NOP remaining 7 bytes (12 - 5 = 7)
    patcher_logic::FillNops(hookSite + 5, hookLen - 5);

    VirtualProtect(hookSite, hookLen, oldProtect, &oldProtect);

    Log("CAP HOOK: installed at RVA 0x%X, return to 0x%p", hookRVA, returnAddr);
    return true;
}

// Patch bytes at a given RVA. Verifies expected bytes before patching.
static bool PatchBytes(BYTE* base, DWORD rva,
                       const BYTE* expected, const BYTE* replacement, size_t len)
{
    BYTE* addr = base + rva;
    if (!patcher_logic::BytesMatch(addr, expected, len)) {
        Log("PATCH FAIL at RVA 0x%X: bytes mismatch", rva);
        Log("  Expected: %02X %02X %02X ...", expected[0], expected[1],
            len > 2 ? expected[2] : 0);
        Log("  Found:    %02X %02X %02X ...", addr[0], addr[1],
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

// NOP out a range of bytes (fill with 0x90).
static bool NopRange(BYTE* base, DWORD rva, const BYTE* expected, size_t len)
{
    BYTE nops[64];
    if (len > sizeof(nops)) return false;
    patcher_logic::FillNops(nops, len);
    return PatchBytes(base, rva, expected, nops, len);
}

// Wait for bytes at a given RVA to match expected values (unpacker may still be running).
// Checks only the first `checkLen` bytes of `expected`.
// Returns true if matched within timeout, false otherwise.
static bool WaitForBytes(BYTE* base, DWORD rva, const BYTE* expected,
                         size_t checkLen, int maxRetries = 200, int sleepMs = 10)
{
    BYTE* addr = base + rva;
    for (int i = 0; i < maxRetries; i++) {
        __try {
            if (patcher_logic::BytesMatch(addr, expected, checkLen)) {
                if (i > 0)
                    Log("  RVA 0x%X: bytes ready after %d ms", rva, i * sleepMs);
                return true;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Page not committed yet
        }
        Sleep(sleepMs);
    }
    Log("  RVA 0x%X: TIMEOUT waiting for bytes (%d ms)", rva, maxRetries * sleepMs);
    __try {
        Log("  Expected: %02X %02X %02X %02X %02X",
            expected[0], expected[1], expected[2], expected[3], expected[4]);
        Log("  Found:    %02X %02X %02X %02X %02X",
            addr[0], addr[1], addr[2], addr[3], addr[4]);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("  (could not read memory at RVA 0x%X)", rva);
    }
    return false;
}

static void ApplyPatches(BYTE* base)
{
    int ok = 0, fail = 0;

    // ── Patch 1: fontSize default 24 → 14 ──────────────────────────────
    // RVA 0x0F89B3: mov dword ptr [edx+0x4648], 0x18
    // Change the immediate value from 0x18 to 0x0E
    {
        // Full instruction: C7 82 48 46 00 00 18 00 00 00
        // The 0x18 is at offset +6 within the instruction, RVA 0x0F89B7 + 2 = 0x0F89B7
        // Actually: the instruction starts at 0x0F89B1 (after the FF byte of previous instr)
        // Byte at RVA 0x0F89B7 is the immediate 0x18
        const BYTE orig[] = { 0x18 };
        const BYTE repl[] = { 0x0E };
        if (PatchBytes(base, 0x0F89B7, orig, repl, 1)) ok++; else fail++;
    }

    // ── Patch 2: nextY addend 6 → 4 ────────────────────────────────────
    // RVA 0x0F89E3: add edx, 6  →  add edx, 4
    // So nextY = fontSize + 4 = 14 + 4 = 18
    {
        const BYTE orig[] = { 0x06 };
        const BYTE repl[] = { 0x04 };
        if (PatchBytes(base, 0x0F89E5, orig, repl, 1)) ok++; else fail++;
    }

    // ── Patch 3: NOP FXF fontSize read ──────────────────────────────────
    // RVA 0x0F89BB-0x0F89D6 (28 bytes)
    // push "fontSize"; mov eax,[ebp-16Ch]; add eax,0x4648; push eax;
    //   mov ecx,[ebp-16Ch]; call FUN_005260c0
    {
        const BYTE orig[] = {
            0x68, 0xC4, 0xFB, 0x56, 0x00,               // push "fontSize"
            0x8B, 0x85, 0x94, 0xFE, 0xFF, 0xFF,          // mov eax,[ebp-16Ch]
            0x05, 0x48, 0x46, 0x00, 0x00,                 // add eax,0x4648
            0x50,                                          // push eax
            0x8B, 0x8D, 0x94, 0xFE, 0xFF, 0xFF,          // mov ecx,[ebp-16Ch]
            0xE8, 0xE9, 0xD6, 0x02, 0x00                  // call FUN_005260c0
        };
        if (NopRange(base, 0x0F89BB, orig, 28)) ok++; else fail++;
    }

    // ── Patch 4: NOP FXF nextY read ─────────────────────────────────────
    // RVA 0x0F8A48-0x0F8A64 (29 bytes)
    {
        const BYTE orig[] = {
            0x68, 0xE8, 0xFB, 0x56, 0x00,               // push "nextY"
            0x8B, 0x8D, 0x94, 0xFE, 0xFF, 0xFF,          // mov ecx,[ebp-16Ch]
            0x81, 0xC1, 0x58, 0x46, 0x00, 0x00,          // add ecx,0x4658
            0x51,                                          // push ecx
            0x8B, 0x8D, 0x94, 0xFE, 0xFF, 0xFF,          // mov ecx,[ebp-16Ch]
            0xE8, 0x5B, 0xD6, 0x02, 0x00                  // call FUN_005260c0
        };
        WaitForBytes(base, 0x0F8A48, orig, 7);
        if (NopRange(base, 0x0F8A48, orig, 29)) ok++; else fail++;
    }

    // ── Patch 5: NOP FXF LNextY read ────────────────────────────────────
    // RVA 0x0F8B37-0x0F8B52 (28 bytes)
    // LNextY = nextY is set before FXF read; NOP prevents FXF override.
    {
        const BYTE orig[] = {
            0x68, 0x10, 0xFC, 0x56, 0x00,               // push "LNextY"
            0x8B, 0x85, 0x94, 0xFE, 0xFF, 0xFF,          // mov eax,[ebp-16Ch]
            0x05, 0x68, 0x46, 0x00, 0x00,                 // add eax,0x4668
            0x50,                                          // push eax
            0x8B, 0x8D, 0x94, 0xFE, 0xFF, 0xFF,          // mov ecx,[ebp-16Ch]
            0xE8, 0x6D, 0xD5, 0x02, 0x00                  // call FUN_005260c0
        };
        WaitForBytes(base, 0x0F8B37, orig, 7);
        if (NopRange(base, 0x0F8B37, orig, 28)) ok++; else fail++;
    }

    // ── Patch 6: space default 1 -> -4 ──────────────────────────────────
    // RVA 0x0F8A6B: mov dword ptr [edx+0x466C], 1
    // Change the immediate 1 to -4 (English letter advance = 14 + (-4) = 10px)
    {
        const BYTE orig[] = { 0x01, 0x00, 0x00, 0x00 };
        const BYTE repl[] = { 0xFC, 0xFF, 0xFF, 0xFF }; // -4 as uint32
        WaitForBytes(base, 0x0F8A71, orig, 4);
        if (PatchBytes(base, 0x0F8A71, orig, repl, 4)) ok++; else fail++;
    }

    // ── Patch 7: NOP FXF space read ───────────────────────────────────
    // RVA 0x0F8A75 - 0x0F8A90 (28 bytes)
    // push "space"; mov eax,[ebp-16Ch]; ...
    {
        const BYTE orig[] = {
            0x68, 0xF0, 0xFB, 0x56, 0x00,               // push "space"
            0x8B, 0x85, 0x94, 0xFE, 0xFF, 0xFF,         // mov eax,[ebp-16Ch]
            0x05, 0x6C, 0x46, 0x00, 0x00,               // add eax,0x466C
            0x50,                                       // push eax
            0x8B, 0x8D, 0x94, 0xFE, 0xFF, 0xFF,         // mov ecx,[ebp-16Ch]
            0xE8, 0x2F, 0xD6, 0x02, 0x00                // call ...
        };
        WaitForBytes(base, 0x0F8A75, orig, 5);
        if (NopRange(base, 0x0F8A75, orig, 28)) ok++; else fail++;
    }

    // ── Patch 8: nextX default 0 -> 0 ──────────────────────────────────
    // Left side padding. Keep at 0 to maximize horizontal real estate.
    {
        const BYTE orig[] = { 0x00, 0x00, 0x00, 0x00 };
        const BYTE repl[] = { 0x00, 0x00, 0x00, 0x00 };
        WaitForBytes(base, 0x0F8997, orig, 4);
        if (PatchBytes(base, 0x0F8997, orig, repl, 4)) ok++; else fail++;
    }

    // ── Patch 9: NOP FXF nextX read ───────────────────────────────────
    // RVA 0x0F8A2C - 0x0F8A47 (28 bytes)
    // push "nextX"; mov eax,[ebp-16Ch]; ...
    {
        const BYTE orig[] = {
            0x68, 0xE0, 0xFB, 0x56, 0x00,               // push "nextX"
            0x8B, 0x85, 0x94, 0xFE, 0xFF, 0xFF,         // mov eax,[ebp-16Ch]
            0x05, 0x54, 0x46, 0x00, 0x00,               // add eax,0x4654
            0x50,                                       // push eax
            0x8B, 0x8D, 0x94, 0xFE, 0xFF, 0xFF,         // mov ecx,[ebp-16Ch]
            0xE8, 0x78, 0xD6, 0x02, 0x00                // call ...
        };
        WaitForBytes(base, 0x0F8A2C, orig, 5);
        if (NopRange(base, 0x0F8A2C, orig, 28)) ok++; else fail++;
    }

    // ── Patch 10: windowMessageNumber 4 → 7 ────────────────────────────
    // RVA 0x0F8D6F: mov dword ptr [ecx+0x46CC], 4
    // Full instruction: C7 81 CC 46 00 00 04 00 00 00
    // Byte at RVA 0x0F8D75 is the immediate value 0x04 → change to 0x07
    // This allows 7 lines in window mode (fits at fontSize=14, nextY=18: 7*18=126px)
    {
        const BYTE orig[] = { 0x04 };
        const BYTE repl[] = { 0x07 };
        WaitForBytes(base, 0x0F8D75, orig, 1);
        if (PatchBytes(base, 0x0F8D75, orig, repl, 1)) ok++; else fail++;
    }

    // ── Patch 11: NOP FXF windowMessageNumber read ──────────────────────
    // RVA 0x0F8DA4 - 0x0F8DBF (28 bytes)
    // push "windowMessageNumber"; mov eax,[ebp-16Ch]; add eax,0x46CC;
    //   push eax; mov ecx,[ebp-16Ch]; call FUN_005260c0
    // NOP prevents FXF config from overriding our patched value of 7.
    {
        const BYTE orig[] = {
            0x68, 0xC0, 0xFC, 0x56, 0x00,               // push "windowMessageNumber"
            0x8B, 0x85, 0x94, 0xFE, 0xFF, 0xFF,          // mov eax,[ebp-16Ch]
            0x05, 0xCC, 0x46, 0x00, 0x00,                 // add eax,0x46CC
            0x50,                                          // push eax
            0x8B, 0x8D, 0x94, 0xFE, 0xFF, 0xFF,          // mov ecx,[ebp-16Ch]
            0xE8, 0x00, 0xD3, 0x02, 0x00                   // call FUN_005260c0
        };
        WaitForBytes(base, 0x0F8DA4, orig, 5);
        if (NopRange(base, 0x0F8DA4, orig, 28)) ok++; else fail++;
    }

    // ── Patch 12: Chart popup text fontSize 20 → 14 ────────────────────
    // In FUN_00407340 (chart popup renderer) at RVA 0x007504:
    //   6A 14   push 0x14  (fontSize=20 arg to FUN_0042e610)
    // Change immediate from 0x14 to 0x0E (14) to match dialog text size.
    {
        const BYTE orig[] = { 0x6A, 0x14, 0x8B, 0x55, 0xD8 };
        const BYTE repl[] = { 0x6A, 0x0E, 0x8B, 0x55, 0xD8 };
        WaitForBytes(base, 0x007504, orig, 5);
        if (PatchBytes(base, 0x007504, orig, repl, 5)) ok++; else fail++;
    }

    // ── Patch 13: Chart popup lineSpacing 28 → 18 ──────────────────────
    // Same function, RVA 0x00750D:
    //   6B C0 1C   imul eax, eax, 0x1C  (line * 28 for y-offset)
    // Change 0x1C to 0x12 (18) so lines are 18px apart (fontSize 14 + 4 gap).
    {
        const BYTE orig[] = { 0x6B, 0xC0, 0x1C, 0x03, 0x45 };
        const BYTE repl[] = { 0x6B, 0xC0, 0x12, 0x03, 0x45 };
        if (PatchBytes(base, 0x00750D, orig, repl, 5)) ok++; else fail++;
    }

    Log("Patches applied: %d OK, %d FAILED", ok, fail);

    // Install GDI font creation hook (must be after unpacker restores IAT)
    InstallFontHook();

    // Install chart popup logging hook
    if (InstallChartPopupHook(base)) {
        Log("CHART HOOK: popup logging installed");
    } else {
        Log("CHART HOOK: FAILED to install popup logging");
    }

    // Install logging hook on the line-count capping code in FUN_004f7650
    // This logs lineCount, windowMessageNumber, text lines on every text display
    if (InstallCapHook(base)) {
        Log("CAP HOOK: logging hook installed successfully");
    } else {
        Log("CAP HOOK: FAILED to install logging hook");
    }
}

// Polling thread: waits for the unpacker to decompress the game code,
// then applies patches.
static DWORD WINAPI PatcherThread(LPVOID)
{
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    Log("Patcher: ImageBase=0x%p, waiting for code to unpack...", base);

    // Sentinel: the fontSize default instruction at RVA 0x0F89B1
    // mov dword ptr [edx+0x4648], 0x18 = C7 82 48 46 00 00 18 00 00 00
    // Preceded by FF from previous instruction
    const BYTE sentinel[] = {
        0xFF, 0xC7, 0x82, 0x48, 0x46, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00
    };
    const DWORD sentinelRVA = 0x0F89B0;

    // Poll every 10ms for up to 30 seconds
    for (int i = 0; i < 3000 && !g_shutdownRequested; i++) {
        __try {
            if (memcmp(base + sentinelRVA, sentinel, sizeof(sentinel)) == 0) {
                Log("Patcher: Code unpacked (detected at iteration %d)", i);
                Sleep(50);
                ApplyPatches(base);
                return 0;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            // Page not yet committed; keep waiting
        }
        Sleep(10);
    }

    Log("Patcher: TIMEOUT - sentinel pattern not found after 30 seconds");
    return 1;
}

// ── GDI Font Hooks ──────────────────────────────────────────────────────
// Hook CreateFontIndirectW, CreateFontW, and ExtTextOutW to identify
// which code path renders flowchart text vs dialog text.

static decltype(&CreateFontIndirectW) g_origCreateFontIndirectW = nullptr;
static decltype(&CreateFontW) g_origCreateFontW = nullptr;
static decltype(&ExtTextOutW) g_origExtTextOutW = nullptr;
static BYTE* g_exeBase = nullptr;

static HFONT WINAPI HookedCreateFontIndirectW(const LOGFONTW* lf)
{
    if (lf) {
        void* retAddr = _ReturnAddress();
        DWORD rva = (DWORD)((BYTE*)retAddr - g_exeBase);

        char face[64] = {0};
        WideCharToMultiByte(CP_ACP, 0, lf->lfFaceName, -1, face, sizeof(face)-1, NULL, NULL);

        Log("FONT: CreateFontIndirectW height=%d width=%d weight=%d face=\"%s\" caller_RVA=0x%06X",
            lf->lfHeight, lf->lfWidth, lf->lfWeight, face, rva);
    }
    return g_origCreateFontIndirectW(lf);
}

static HFONT WINAPI HookedCreateFontW(
    int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight,
    DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet,
    DWORD iOutPrecision, DWORD iClipPrecision, DWORD iQuality,
    DWORD iPitchAndFamily, LPCWSTR pszFaceName)
{
    void* retAddr = _ReturnAddress();
    DWORD rva = (DWORD)((BYTE*)retAddr - g_exeBase);

    char face[64] = {0};
    if (pszFaceName)
        WideCharToMultiByte(CP_ACP, 0, pszFaceName, -1, face, sizeof(face)-1, NULL, NULL);

    Log("FONT: CreateFontW height=%d width=%d weight=%d charset=%u face=\"%s\" caller_RVA=0x%06X",
        cHeight, cWidth, cWeight, iCharSet, face, rva);

    return g_origCreateFontW(cHeight, cWidth, cEscapement, cOrientation, cWeight,
        bItalic, bUnderline, bStrikeOut, iCharSet,
        iOutPrecision, iClipPrecision, iQuality,
        iPitchAndFamily, pszFaceName);
}

// Log ExtTextOutW calls — but only when caller is in the game EXE (RVA < 0xB00000)
// to avoid flooding from system calls.
static BOOL WINAPI HookedExtTextOutW(
    HDC hdc, int x, int y, UINT options, const RECT* lprect,
    LPCWSTR lpString, UINT c, const INT* lpDx)
{
    void* retAddr = _ReturnAddress();
    DWORD rva = (DWORD)((BYTE*)retAddr - g_exeBase);

    // Only log calls from within the game EXE
    if (rva < 0x00B00000 && lpString && c > 0) {
        // Convert first few chars for logging
        char text[128] = {0};
        WideCharToMultiByte(CP_ACP, 0, lpString, (c > 30 ? 30 : c), text, sizeof(text)-1, NULL, NULL);
        Log("TEXT: ExtTextOutW x=%d y=%d len=%u caller_RVA=0x%06X text=\"%s\"",
            x, y, c, rva, text);
    }

    return g_origExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);
}

// Patch the IAT entry for a given function in the main EXE's import table
static bool HookIAT(HMODULE hModule, const char* dllName, const char* funcName, void* hookFunc, void** origFunc)
{
    BYTE* base = (BYTE*)hModule;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY* importDir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (!importDir->VirtualAddress)
        return false;

    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + importDir->VirtualAddress);
    for (; imp->Name; imp++) {
        const char* name = (const char*)(base + imp->Name);
        if (_stricmp(name, dllName) != 0)
            continue;

        IMAGE_THUNK_DATA* origThunk = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* iatThunk  = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);

        for (; origThunk->u1.AddressOfData; origThunk++, iatThunk++) {
            if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + origThunk->u1.AddressOfData);
            if (strcmp(ibn->Name, funcName) == 0) {
                DWORD oldProt;
                VirtualProtect(&iatThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt);
                *origFunc = (void*)iatThunk->u1.Function;
                iatThunk->u1.Function = (ULONG_PTR)hookFunc;
                VirtualProtect(&iatThunk->u1.Function, sizeof(void*), oldProt, &oldProt);
                return true;
            }
        }
    }
    return false;
}

// Inline hook: overwrites first 5 bytes of target function with JMP to hook.
// Saves the original bytes + a JMP-back in an executable trampoline.
static BYTE* g_fontTrampoline = nullptr;

static bool InlineHook(BYTE* target, void* hookFunc, void** origTrampoline)
{
    // Allocate executable page for the trampoline
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;

    // Copy original 5 bytes to trampoline, then JMP back to target+5
    BYTE* tp = patcher_logic::EmitBytes(tramp, target, 5);
    patcher_logic::EmitRelJmp32(
        tp, reinterpret_cast<std::uintptr_t>(target + 5));

    *origTrampoline = tramp;

    // Overwrite target with JMP to our hook
    DWORD oldProt;
    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProt);
    patcher_logic::EmitRelJmp32(
        target, reinterpret_cast<std::uintptr_t>(hookFunc));
    VirtualProtect(target, 5, oldProt, &oldProt);

    return true;
}

static void InstallFontHook()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    g_exeBase = (BYTE*)exe;

    HMODULE gdi32 = GetModuleHandleW(L"GDI32.dll");
    if (!gdi32) gdi32 = LoadLibraryW(L"GDI32.dll");
    if (!gdi32) {
        Log("FONT HOOK: Could not load GDI32.dll");
        return;
    }

    // Hook CreateFontIndirectW (inline on GDI32)
    {
        BYTE* target = (BYTE*)GetProcAddress(gdi32, "CreateFontIndirectW");
        if (target) {
            Log("FONT HOOK: CreateFontIndirectW at 0x%p, bytes: %02X %02X %02X %02X %02X",
                target, target[0], target[1], target[2], target[3], target[4]);
            void* tramp = nullptr;
            if (InlineHook(target, (void*)HookedCreateFontIndirectW, &tramp)) {
                g_origCreateFontIndirectW = (decltype(g_origCreateFontIndirectW))tramp;
                Log("FONT HOOK: CreateFontIndirectW inline hook OK");
            }
        }
    }

    // Hook CreateFontW (inline on GDI32)
    {
        BYTE* target = (BYTE*)GetProcAddress(gdi32, "CreateFontW");
        if (target) {
            Log("FONT HOOK: CreateFontW at 0x%p, bytes: %02X %02X %02X %02X %02X",
                target, target[0], target[1], target[2], target[3], target[4]);
            void* tramp = nullptr;
            if (InlineHook(target, (void*)HookedCreateFontW, &tramp)) {
                g_origCreateFontW = (decltype(g_origCreateFontW))tramp;
                Log("FONT HOOK: CreateFontW inline hook OK");
            }
        }
    }

    // Hook ExtTextOutW (inline on GDI32)
    {
        BYTE* target = (BYTE*)GetProcAddress(gdi32, "ExtTextOutW");
        if (target) {
            Log("FONT HOOK: ExtTextOutW at 0x%p, bytes: %02X %02X %02X %02X %02X",
                target, target[0], target[1], target[2], target[3], target[4]);
            void* tramp = nullptr;
            if (InlineHook(target, (void*)HookedExtTextOutW, &tramp)) {
                g_origExtTextOutW = (decltype(g_origExtTextOutW))tramp;
                Log("FONT HOOK: ExtTextOutW inline hook OK");
            }
        }
    }
}

void PatcherInit()
{
    g_shutdownRequested = false;
    g_patchThread = CreateThread(nullptr, 0, PatcherThread, nullptr, 0, nullptr);
    if (!g_patchThread) {
        Log("Patcher: Failed to create thread, error %u", GetLastError());
    }
}

void PatcherShutdown()
{
    g_shutdownRequested = true;
    if (g_patchThread) {
        WaitForSingleObject(g_patchThread, 5000);
        CloseHandle(g_patchThread);
        g_patchThread = nullptr;
    }
}
