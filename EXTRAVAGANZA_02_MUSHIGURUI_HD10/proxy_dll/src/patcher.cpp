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
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

// ==========================================================================
// Runtime patcher for mushigurui HD10 (BLACKCyc engine).
//
// Same engine as EXTRAVAGANZA_CE but with different struct offsets.
// The packed EXE decompresses code at runtime. Font size and line
// spacing are set by FXF config files (fontSize=24, nextY=30). English
// translations need smaller text to fit in the textbox without cutoff.
//
// After the unpacker finishes, patch the PrintMessage constructor to:
//   1. Use fontSize=16 instead of 24
//   2. Calculate nextY = fontSize + 4 = 20 (instead of + 6 = 30)
//   3. Prevent FXF config reads from overriding patched values
//   4. Set space=-4 for tighter English letter spacing
//   5. Raise windowMessageNumber from 4 to 6 for more visible lines
//
// mushigurui_HD10 struct offsets (vs EXTRAVAGANZA_CE):
//   fontSize:             edi+0x4680  (was 0x4648)
//   nextY:                edi+0x46A4  (was 0x4658)
//   nextX:                edi+0x469C  (was 0x4654)
//   space:                edi+0x46BC  (was 0x466C)
//   windowMessageNumber:  edi+0x474C  (was 0x46CC)
//   fullMessageNumber:    edi+0x4750  (was 0x46D0)
//   mode:                 esi+0x45F8  (was 0x45DC)
//   lineCount:            esi+0x01E4  (was 0x01CC)
//
// FXF override prevention: In this build the FXF reader returns -1 if the
// field is not found. After each call there is "cmp eax,-1; je skip".
// Changing je (0x74) to jmp (0xEB) forces the skip, preventing override.
//
// All RVAs are from the runtime memory dump (mushigurui_HD10_dumped.exe).
// ImageBase = 0x004C0000. RVA = file_offset in dump.
// ==========================================================================

static HANDLE g_patchThread = nullptr;
static volatile bool g_shutdownRequested = false;
static PVOID g_vehHandle = nullptr;

// ── Vectored Exception Handler: catches crashes and logs details ────────
static const char* ExceptionCodeName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:     return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:       return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:   return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:  return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:     return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:        return "IN_PAGE_ERROR";
    case EXCEPTION_DATATYPE_MISALIGNMENT:return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:   return "FLT_DIVIDE_BY_ZERO";
    default: return "UNKNOWN";
    }
}

// Helper: safely read a DWORD from memory, returns false if inaccessible
static bool SafeReadDword(DWORD addr, DWORD* out)
{
    __try {
        *out = *(DWORD*)addr;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Helper: dump up to 'n' bytes at address as hex, safely
static void LogMemAt(const char* label, DWORD addr, int n = 32)
{
    __try {
        BYTE* p = (BYTE*)addr;
        char buf[256];
        int pos = 0;
        for (int i = 0; i < n && pos < 240; i++)
            pos += sprintf(buf + pos, "%02X ", p[i]);
        Log("  %s (0x%08X): %s", label, addr, buf);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("  %s (0x%08X): <inaccessible>", label, addr);
    }
}

// Helper: check if a DWORD looks like ASCII text (printable bytes).
// Thin wrapper around the pure patcher_logic::LooksLikeAscii so this
// file keeps its local name and the pure version can be tested.
static bool LooksLikeAscii(DWORD val)
{
    return patcher_logic::LooksLikeAscii(static_cast<std::uint32_t>(val));
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    // Ignore C++ exceptions, OutputDebugString, breakpoints, etc.
    if (code == 0xE06D7363 ||   // C++ exception (MSVC)
        code == 0x40010006 ||   // OutputDebugString
        code == EXCEPTION_BREAKPOINT ||
        code == EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;

    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    CONTEXT* ctx = ep->ContextRecord;
    DWORD rva = (DWORD)((BYTE*)ep->ExceptionRecord->ExceptionAddress - base);

    // Filter out the unpacker's INT_DIVIDE_BY_ZERO and ILLEGAL_INSTRUCTION
    // at known addresses — these are handled by game SEH, not real crashes.
    if ((code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_ILLEGAL_INSTRUCTION)
        && (rva == 0x32F2D5 || rva == 0x32F300))
        return EXCEPTION_CONTINUE_SEARCH;

    // Also filter the OLE/COM 0x800401F0 noise
    if (code == 0x800401F0)
        return EXCEPTION_CONTINUE_SEARCH;

    Log("!!! CRASH: %s (0x%08X) at VA 0x%p (RVA 0x%06X)",
        ExceptionCodeName(code), code,
        ep->ExceptionRecord->ExceptionAddress, rva);

    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        const char* op = ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ";
        DWORD targetAddr = (DWORD)ep->ExceptionRecord->ExceptionInformation[1];
        Log("  %s of address 0x%08X", op, targetAddr);
    }

    Log("  EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X",
        ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
    Log("  ESI=0x%08X EDI=0x%08X EBP=0x%08X ESP=0x%08X",
        ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
    Log("  EIP=0x%08X EFLAGS=0x%08X", ctx->Eip, ctx->EFlags);

    // Dump bytes at crash address
    __try {
        BYTE* ip = (BYTE*)ep->ExceptionRecord->ExceptionAddress;
        Log("  Bytes at EIP-4: %02X %02X %02X %02X [%02X] %02X %02X %02X %02X %02X %02X %02X",
            ip[-4], ip[-3], ip[-2], ip[-1], ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6], ip[7]);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("  (could not read bytes at EIP)");
    }

    // ── Dereference registers: show what each register points to ────────
    Log("  Register dereferences:");
    struct { const char* name; DWORD val; } regs[] = {
        {"EAX", ctx->Eax}, {"EBX", ctx->Ebx}, {"ECX", ctx->Ecx}, {"EDX", ctx->Edx},
        {"ESI", ctx->Esi}, {"EDI", ctx->Edi}
    };
    for (auto& r : regs) {
        if (r.val < 0x10000) continue;  // skip small values (not pointers)
        DWORD deref;
        if (SafeReadDword(r.val, &deref)) {
            DWORD derefRva = deref - (DWORD)base;
            if (LooksLikeAscii(deref)) {
                BYTE* b = (BYTE*)&deref;
                Log("    [%s] = 0x%08X (ASCII: '%c%c%c%c')",
                    r.name, deref, b[0], b[1], b[2], b[3]);
            } else if (derefRva < 0x00B00000) {
                Log("    [%s] = 0x%08X (RVA 0x%06X)", r.name, deref, derefRva);
            } else {
                Log("    [%s] = 0x%08X", r.name, deref);
            }
        }
    }

    // ── For the SPT label crash (RVA 0x043123): dump SPT object details ─
    // The crash function at RVA 0x0430E0 reads:
    //   ECX = SPT object (this ptr), offset +0x908 = data table, +0x2664 = base_index
    //   It does: table[(base_index + val) * 4], then table[result * 4] + result
    if (rva == 0x043123 || rva == 0x04311A) {
        Log("  --- SPT label lookup details (crash func RVA 0x0430E0) ---");
        DWORD ecx = ctx->Ecx;
        DWORD tablePtr, baseIdx, count;
        if (SafeReadDword(ecx + 0x908, &tablePtr))
            Log("    SPT data table ptr [ECX+0x908] = 0x%08X", tablePtr);
        if (SafeReadDword(ecx + 0x2664, &baseIdx))
            Log("    Base index [ECX+0x2664] = %u (0x%X)", baseIdx, baseIdx);
        if (SafeReadDword(ecx, &count))
            Log("    Entry count [ECX] = %u", count);
        // Show entries being iterated
        DWORD objBase = ecx + 4;
        Log("    First entries in object array:");
        for (int i = 0; i < 8; i++) {
            DWORD entry;
            if (SafeReadDword(objBase + i * 4, &entry)) {
                // Check if table lookup would work
                DWORD lookupIdx = 0, lookupVal = 0;
                if (SafeReadDword(ecx + 0x2664, &lookupIdx)) {
                    lookupIdx += entry;
                    if (SafeReadDword(tablePtr + lookupIdx * 4, &lookupVal)) {
                        Log("      [%d] val=%u, table[%u+%u=%u] = %u%s",
                            i, entry, baseIdx, entry, lookupIdx, lookupVal,
                            LooksLikeAscii(lookupVal) ? " *** ASCII GARBAGE ***" : "");
                    }
                }
            }
        }
        // Show bytes around the table entry that returned garbage
        if (SafeReadDword(ecx + 0x908, &tablePtr) && SafeReadDword(ecx + 0x2664, &baseIdx)) {
            DWORD badIdx = baseIdx + ctx->Esi;  // ESI often holds the current *piVar4
            Log("    Table around bad index (base=%u, ESI=%u, sum=%u):",
                baseIdx, ctx->Esi, badIdx);
            LogMemAt("table[idx-4..idx+4]", tablePtr + (badIdx > 4 ? badIdx - 4 : 0) * 4, 36);
        }
    }

    // ── Full stack dump (first 64 DWORDs) ───────────────────────────────
    Log("  Stack trace (full):");
    __try {
        DWORD* stack = (DWORD*)ctx->Esp;
        for (int i = 0; i < 64; i++) {
            DWORD val = stack[i];
            DWORD valRva = val - (DWORD)base;
            if (valRva < 0x00B00000) {
                Log("    [ESP+%02X] = 0x%08X (RVA 0x%06X)", i * 4, val, valRva);
            } else if (val > 0x10000 && val < 0xF0000000) {
                // Could be heap/data pointer - show first 4 bytes
                DWORD deref;
                if (SafeReadDword(val, &deref) && LooksLikeAscii(deref)) {
                    BYTE* b = (BYTE*)&deref;
                    Log("    [ESP+%02X] = 0x%08X -> ASCII '%c%c%c%c'",
                        i * 4, val, b[0], b[1], b[2], b[3]);
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("    (could not read stack)");
    }

    // ── EBP chain walk (frame pointers) ─────────────────────────────────
    Log("  Call chain (EBP walk):");
    __try {
        DWORD ebp = ctx->Ebp;
        for (int depth = 0; depth < 16 && ebp > 0x10000 && ebp < 0xFFF00000; depth++) {
            DWORD retAddr;
            if (!SafeReadDword(ebp + 4, &retAddr)) break;
            DWORD retRva = retAddr - (DWORD)base;
            if (retRva < 0x00B00000)
                Log("    frame[%d] EBP=0x%08X ret=0x%08X (RVA 0x%06X)", depth, ebp, retAddr, retRva);
            else
                Log("    frame[%d] EBP=0x%08X ret=0x%08X", depth, ebp, retAddr);
            DWORD nextEbp;
            if (!SafeReadDword(ebp, &nextEbp) || nextEbp <= ebp) break;
            ebp = nextEbp;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("    (EBP walk failed)");
    }

    // Flush before the process dies
    if (g_logFile) fflush(g_logFile);

    return EXCEPTION_CONTINUE_SEARCH;
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

// Wait for bytes at a given RVA to match expected values.
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
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
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

    // ── Patch 1: fontSize default 24 → 16 ──────────────────────────────
    // RVA 0x05E681: mov dword ptr [edi+0x4680], 0x18
    // Bytes: C7 87 80 46 00 00 [18] 00 00 00
    // Change immediate at +6 from 0x18 to 0x10
    {
        const BYTE orig[] = { 0x18 };
        const BYTE repl[] = { 0x10 };
        if (PatchBytes(base, 0x05E687, orig, repl, 1)) ok++; else fail++;
    }

    // ── Patch 2: nextY addend 6 → 4 ────────────────────────────────────
    // RVA 0x05E6E5: add eax, 6  (83 C0 [06])
    // So nextY = fontSize + 4 = 16 + 4 = 20
    {
        const BYTE orig[] = { 0x06 };
        const BYTE repl[] = { 0x04 };
        if (PatchBytes(base, 0x05E6E7, orig, repl, 1)) ok++; else fail++;
    }

    // ── Patch 3: Skip FXF fontSize override ─────────────────────────────
    // After FXF read call at RVA 0x05E68B:
    //   E8 xx xx xx xx  call FXF_read
    //   83 F8 FF        cmp eax, -1
    //   74 2A           je skip_override  ← change to EB (jmp)
    {
        const BYTE orig[] = { 0x74 };
        const BYTE repl[] = { 0xEB };
        if (PatchBytes(base, 0x05E693, orig, repl, 1)) ok++; else fail++;
    }

    // ── Patch 4: Skip FXF nextX override ────────────────────────────────
    // push "nextX" at 0x05E76C, call at 0x05E771, je at 0x05E779
    {
        const BYTE orig[] = { 0x74 };
        const BYTE repl[] = { 0xEB };
        WaitForBytes(base, 0x05E771, (const BYTE*)"\xE8", 1);
        if (PatchBytes(base, 0x05E779, orig, repl, 1)) ok++; else fail++;
    }

    // ── Patch 5: Skip FXF nextY override ────────────────────────────────
    // push "nextY" at 0x05E7AB, call at 0x05E7B0, je at 0x05E7B8
    {
        const BYTE orig[] = { 0x74 };
        const BYTE repl[] = { 0xEB };
        WaitForBytes(base, 0x05E7B0, (const BYTE*)"\xE8", 1);
        if (PatchBytes(base, 0x05E7B8, orig, repl, 1)) ok++; else fail++;
    }

    // ── Patch 6: space default 1 → -4 ──────────────────────────────────
    // RVA 0x05E81F: mov dword ptr [edi+0x46BC], 1
    // Bytes: C7 87 BC 46 00 00 [01 00 00 00]
    // Change immediate to -4 (0xFFFFFFFC)
    {
        const BYTE orig[] = { 0x01, 0x00, 0x00, 0x00 };
        const BYTE repl[] = { 0xFC, 0xFF, 0xFF, 0xFF };
        WaitForBytes(base, 0x05E825, orig, 4);
        if (PatchBytes(base, 0x05E825, orig, repl, 4)) ok++; else fail++;
    }

    // ── Patch 7: Skip FXF space override ────────────────────────────────
    // push "space" at 0x05E814, call at 0x05E829, je at 0x05E831
    {
        const BYTE orig[] = { 0x74 };
        const BYTE repl[] = { 0xEB };
        WaitForBytes(base, 0x05E829, (const BYTE*)"\xE8", 1);
        if (PatchBytes(base, 0x05E831, orig, repl, 1)) ok++; else fail++;
    }

    // ── Patch 8: Skip FXF LNextY override ───────────────────────────────
    // push "LNextY" at 0x05EA58, call at 0x05EA5D, je at 0x05EA65
    // Note: jump offset is 0x26 (not 0x2A like others)
    {
        const BYTE orig[] = { 0x74 };
        const BYTE repl[] = { 0xEB };
        WaitForBytes(base, 0x05EA5D, (const BYTE*)"\xE8", 1);
        if (PatchBytes(base, 0x05EA65, orig, repl, 1)) ok++; else fail++;
    }

    // ── Patch 9: windowMessageNumber 4 → 6 ─────────────────────────────
    // RVA 0x05F273: mov dword ptr [edi+0x474C], 4
    // Bytes: C7 87 4C 47 00 00 [04] 00 00 00
    // Change immediate from 0x04 to 0x06
    // This allows 6 visible lines (fits at fontSize=16, nextY=20: 6*20=120px)
    {
        const BYTE orig[] = { 0x04 };
        const BYTE repl[] = { 0x06 };
        WaitForBytes(base, 0x05F279, orig, 1);
        if (PatchBytes(base, 0x05F279, orig, repl, 1)) ok++; else fail++;
    }

    // ── Patch 10: Skip FXF windowMessageNumber override ─────────────────
    // push "windowMessageNumber" at 0x05F268, call at 0x05F283, je at 0x05F28B
    {
        const BYTE orig[] = { 0x74 };
        const BYTE repl[] = { 0xEB };
        WaitForBytes(base, 0x05F283, (const BYTE*)"\xE8", 1);
        if (PatchBytes(base, 0x05F28B, orig, repl, 1)) ok++; else fail++;
    }

    // NOTE: An attempt at Patch 11/12 (fontSize=16 + FXF skip on the
    // [SelectMessage] constructor at RVA 0x047D00, struct offset +0x4348)
    // applied cleanly but had ZERO visible effect on the in-game choice
    // menu. [SelectMessage] turned out to drive the backlog message
    // selector, not in-game choices — its FXF keys include backLogOk,
    // optionOk, cursorIsOnMessage. The in-game choice menu uses a render
    // path that does not match the standard `mov [reg+disp], 0x18` font
    // template used by all other text widgets. Patch deferred.

    Log("Patches applied: %d OK, %d FAILED", ok, fail);
}

// Polling thread: waits for the unpacker to decompress the game code,
// then applies patches.
static DWORD WINAPI PatcherThread(LPVOID)
{
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    Log("Patcher: ImageBase=0x%p, waiting for code to unpack...", base);

    // Sentinel: the fontSize default instruction at RVA 0x05E681
    // mov dword ptr [edi+0x4680], 0x18 = C7 87 80 46 00 00 18 00 00 00
    const BYTE sentinel[] = {
        0xC7, 0x87, 0x80, 0x46, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00
    };
    const DWORD sentinelRVA = 0x05E681;

    // Poll every 10ms for up to 30 seconds
    for (int i = 0; i < 3000 && !g_shutdownRequested; i++) {
        __try {
            if (patcher_logic::BytesMatch(base + sentinelRVA, sentinel, sizeof(sentinel))) {
                Log("Patcher: Code unpacked (detected at iteration %d)", i);
                Sleep(50);
                ApplyPatches(base);
                return 0;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
        Sleep(10);
    }

    Log("Patcher: TIMEOUT - sentinel pattern not found after 30 seconds");
    return 1;
}

void PatcherInit()
{
    // Install crash handler first so we catch any crash during patching or gameplay
    g_vehHandle = AddVectoredExceptionHandler(1, CrashHandler);
    Log("Crash handler installed: %s", g_vehHandle ? "OK" : "FAILED");

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
    if (g_vehHandle) {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }
}
