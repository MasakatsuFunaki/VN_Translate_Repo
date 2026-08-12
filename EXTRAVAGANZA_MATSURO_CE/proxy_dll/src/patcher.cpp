// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "patcher.h"
#include "log.h"
#include <cstring>

// ==========================================================================
// Runtime patcher for EXTRAVAGANZA ~Matsuro~ CE (BLACKCyc engine).
//
// Patches the PrintMessage constructor to use English-friendly text settings:
//   fontSize=14 (was 24), nextY=18 (was 30), space=-4 (was 1),
//   windowMessageNumber=7 (was 4).
// NOPs out FXF config reads that would override these values at runtime.
//
// All RVAs are from the runtime memory dump (matsuro_CE_dumped.exe).
// The unpacker restores code to the same virtual addresses.
// ==========================================================================

static HANDLE g_patchThread = nullptr;
static volatile bool g_shutdownRequested = false;

// Patch bytes at a given RVA. Verifies expected bytes before patching.
static bool PatchBytes(BYTE* base, DWORD rva,
                       const BYTE* expected, const BYTE* replacement, size_t len)
{
    BYTE* addr = base + rva;
    if (memcmp(addr, expected, len) != 0) {
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
    memset(nops, 0x90, len);
    return PatchBytes(base, rva, expected, nops, len);
}

// NOP out a range, only verifying the first `checkLen` opcode bytes.
// Used for code blocks containing relocated absolute addresses, where we
// can't predict the exact address bytes but can verify the opcode prefix.
static bool NopRangeRelaxed(BYTE* base, DWORD rva, BYTE firstByte, size_t len)
{
    BYTE* addr = base + rva;
    if (addr[0] != firstByte) {
        Log("NOP FAIL at RVA 0x%X: expected opcode %02X, found %02X",
            rva, firstByte, addr[0]);
        return false;
    }
    DWORD oldProtect;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("NOP FAIL at RVA 0x%X: VirtualProtect error %u", rva, GetLastError());
        return false;
    }
    memset(addr, 0x90, len);
    VirtualProtect(addr, len, oldProtect, &oldProtect);
    Log("NOP OK at RVA 0x%X (%zu bytes)", rva, len);
    return true;
}

// Wait for bytes at a given RVA to match expected values.
static bool WaitForBytes(BYTE* base, DWORD rva, const BYTE* expected,
                         size_t checkLen, int maxRetries = 200, int sleepMs = 10)
{
    BYTE* addr = base + rva;
    for (int i = 0; i < maxRetries; i++) {
        __try {
            if (memcmp(addr, expected, checkLen) == 0) {
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
    return false;
}

static void ApplyPatches(BYTE* base)
{
    int ok = 0, fail = 0;

    // ── Patch 1: fontSize default 24 → 14 ──────────────────────────────
    // RVA 0x100D11: C7 82 48 46 00 00 18 00 00 00
    // mov dword ptr [edx+0x4648], 0x18 → change 0x18 to 0x0E
    {
        const BYTE orig[] = { 0x18 };
        const BYTE repl[] = { 0x0E };
        if (PatchBytes(base, 0x100D17, orig, repl, 1)) ok++; else fail++;
    }

    // ── Patch 2: nextY addend 6 → 4 ────────────────────────────────────
    // RVA 0x100D43: 83 C2 06 (add edx, 6) → add edx, 4
    // nextY = fontSize + 4 = 14 + 4 = 18
    {
        const BYTE orig[] = { 0x06 };
        const BYTE repl[] = { 0x04 };
        if (PatchBytes(base, 0x100D45, orig, repl, 1)) ok++; else fail++;
    }

    // ── Patch 3: NOP FXF fontSize read ──────────────────────────────────
    // RVA 0x100D1B (28 bytes): push "fontSize"; mov; add; push; mov; call
    // Contains relocated absolute address — use relaxed check
    if (NopRangeRelaxed(base, 0x100D1B, 0x68, 28)) ok++; else fail++;

    // ── Patch 4: NOP FXF nextY read ─────────────────────────────────────
    // RVA 0x100DA8 (29 bytes) — relaxed (relocated addr)
    if (NopRangeRelaxed(base, 0x100DA8, 0x68, 29)) ok++; else fail++;

    // ── Patch 5: NOP FXF LNextY read ────────────────────────────────────
    // RVA 0x100E97 (28 bytes) — relaxed (relocated addr)
    if (NopRangeRelaxed(base, 0x100E97, 0x68, 28)) ok++; else fail++;

    // ── Patch 6: space default 1 → -4 ──────────────────────────────────
    // RVA 0x100DCB: C7 82 6C 46 00 00 01 00 00 00
    // English letter advance = 14 + (-4) = 10px per half-width char
    {
        const BYTE orig[] = { 0x01, 0x00, 0x00, 0x00 };
        const BYTE repl[] = { 0xFC, 0xFF, 0xFF, 0xFF };  // -4 as int32
        WaitForBytes(base, 0x100DD1, orig, 4);
        if (PatchBytes(base, 0x100DD1, orig, repl, 4)) ok++; else fail++;
    }

    // ── Patch 7: NOP FXF space read ─────────────────────────────────────
    // RVA 0x100DD5 (28 bytes) — relaxed (relocated addr)
    if (NopRangeRelaxed(base, 0x100DD5, 0x68, 28)) ok++; else fail++;

    // ── Patch 8: NOP FXF nextX read ─────────────────────────────────────
    // RVA 0x100D8C (28 bytes) — relaxed (relocated addr)
    if (NopRangeRelaxed(base, 0x100D8C, 0x68, 28)) ok++; else fail++;

    // ── Patch 9: windowMessageNumber 4 → 7 ─────────────────────────────
    // RVA 0x1010CF: C7 81 CC 46 00 00 04 00 00 00
    // Allows 7 lines in window mode (fits at fontSize=14, nextY=18: 7*18=126px)
    {
        const BYTE orig[] = { 0x04 };
        const BYTE repl[] = { 0x07 };
        WaitForBytes(base, 0x1010D5, orig, 1);
        if (PatchBytes(base, 0x1010D5, orig, repl, 1)) ok++; else fail++;
    }

    // ── Patch 10: NOP FXF windowMessageNumber read ──────────────────────
    // RVA 0x101104 (28 bytes) — relaxed (relocated addr)
    if (NopRangeRelaxed(base, 0x101104, 0x68, 28)) ok++; else fail++;

    Log("Patches applied: %d OK, %d FAILED", ok, fail);
}

// Polling thread: waits for the unpacker to decompress the game code,
// then applies patches.
static DWORD WINAPI PatcherThread(LPVOID)
{
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    Log("Patcher: ImageBase=0x%p, waiting for code to unpack...", base);

    // Sentinel: the fontSize default instruction at RVA 0x100D11
    // C7 82 48 46 00 00 18 00 00 00 = mov dword ptr [edx+0x4648], 0x18
    const BYTE sentinel[] = {
        0xC7, 0x82, 0x48, 0x46, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00
    };
    const DWORD sentinelRVA = 0x100D11;

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
