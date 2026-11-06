// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Runtime patcher for Welcome to AnzU! CE (System-NNN / BCmkri engine).
//
// After the unpacker finishes decompressing the code section, patches the
// PrintMessage constructor across all 10 font/layout/FXF sites so English text
// fits inside the message window.
#include "patcher.h"

#include "log.h"
#include "menu_autopatch.h"
#include "patcher_logic.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr int kDefaultWaitRetries = 200;
constexpr int kDefaultWaitSleepMs = 10;
constexpr int kUnpackPollMaxRetries = 3000;
constexpr int kUnpackPollSleepMs = 10;
constexpr DWORD kPostUnpackSettleMs = 50;
constexpr DWORD kThreadShutdownWaitMs = 5000;
constexpr std::size_t kPreviewIndex0 = 0;
constexpr std::size_t kPreviewIndex1 = 1;
constexpr std::size_t kPreviewIndex2 = 2;

HANDLE g_patchThread = nullptr;
volatile bool g_shutdownRequested = false;

bool PatchBytes(BYTE* base,
                DWORD rva,
                const BYTE* expected,
                const BYTE* replacement,
                std::size_t len) {
    BYTE* addr = base + rva;
    if (!patcher_logic::BytesMatch(addr, expected, len)) {
        Log("PATCH FAIL at RVA 0x%X: bytes mismatch", rva);
        Log("  Expected: %02X %02X %02X ...", expected[kPreviewIndex0],
            len > kPreviewIndex1 ? expected[kPreviewIndex1] : 0,
            len > kPreviewIndex2 ? expected[kPreviewIndex2] : 0);
        Log("  Found:    %02X %02X %02X ...", addr[kPreviewIndex0],
            len > kPreviewIndex1 ? addr[kPreviewIndex1] : 0,
            len > kPreviewIndex2 ? addr[kPreviewIndex2] : 0);
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("PATCH FAIL at RVA 0x%X: VirtualProtect error %u", rva, GetLastError());
        return false;
    }
    std::memcpy(addr, replacement, len);
    VirtualProtect(addr, len, oldProtect, &oldProtect);
    Log("PATCH OK at RVA 0x%X (%zu bytes)", rva, len);
    return true;
}

bool WaitForBytes(BYTE* base,
                  DWORD rva,
                  const BYTE* expected,
                  std::size_t checkLen,
                  int maxRetries = kDefaultWaitRetries,
                  int sleepMs = kDefaultWaitSleepMs) {
    BYTE* addr = base + rva;
    for (int i = 0; i < maxRetries; ++i) {
        __try {
            if (patcher_logic::BytesMatch(addr, expected, checkLen)) {
                if (i > 0) {
                    Log("  RVA 0x%X: bytes ready after %d ms", rva, i * sleepMs);
                }
                return true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        Sleep(static_cast<DWORD>(sleepMs));
    }
    Log("  RVA 0x%X: TIMEOUT waiting for bytes (%d ms)", rva, maxRetries * sleepMs);
    return false;
}

bool NopRangeRelaxed(BYTE* base, DWORD rva, std::size_t len, const char* desc) {
    const BYTE pushOpcode = patcher_logic::kPushImm32Opcode;
    WaitForBytes(base, rva, &pushOpcode, sizeof(pushOpcode));
    BYTE* addr = base + rva;
    if (addr[0] != pushOpcode) {
        Log("NOP FAIL at RVA 0x%X (%s): first byte is 0x%02X, expected 0x68 (push)",
            rva, desc, addr[0]);
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("NOP FAIL at RVA 0x%X (%s): VirtualProtect error %u", rva, desc, GetLastError());
        return false;
    }
    patcher_logic::FillNops(addr, len);
    VirtualProtect(addr, len, oldProtect, &oldProtect);
    Log("NOP OK at RVA 0x%X (%zu bytes, %s)", rva, len, desc);
    return true;
}

void ApplyPatches(BYTE* base, DWORD sentinelRva) {
    int ok = 0;
    int fail = 0;

    {
        const BYTE orig[] = {patcher_logic::kOrigFontSize};
        const BYTE repl[] = {patcher_logic::kPatchedFontSize};
        if (PatchBytes(base, sentinelRva + patcher_logic::kRelPatch1FontSize,
                       orig, repl, sizeof(orig))) {
            ++ok;
        } else {
            ++fail;
        }
    }

    {
        const BYTE orig[] = {patcher_logic::kOrigNextYAddend};
        const BYTE repl[] = {patcher_logic::kPatchedNextYAddend};
        if (PatchBytes(base, sentinelRva + patcher_logic::kRelPatch2NextYAddend,
                       orig, repl, sizeof(orig))) {
            ++ok;
        } else {
            ++fail;
        }
    }

    if (NopRangeRelaxed(base, sentinelRva + patcher_logic::kRelPatch3FxfFontSize,
                        patcher_logic::kPatch3FxfFontSizeNopLen, "FXF fontSize")) {
        ++ok;
    } else {
        ++fail;
    }

    if (NopRangeRelaxed(base, sentinelRva + patcher_logic::kRelPatch4FxfNextY,
                        patcher_logic::kPatch4FxfNextYNopLen, "FXF nextY")) {
        ++ok;
    } else {
        ++fail;
    }

    if (NopRangeRelaxed(base, sentinelRva + patcher_logic::kRelPatch5FxfLNextY,
                        patcher_logic::kPatch5FxfLNextYNopLen, "FXF LNextY")) {
        ++ok;
    } else {
        ++fail;
    }

    {
        const DWORD spaceRva = sentinelRva + patcher_logic::kRelPatch6SpaceDefault;
        WaitForBytes(base, spaceRva,
                     patcher_logic::kOrigSpaceImmediate, patcher_logic::kSpaceImmediateLen);
        if (PatchBytes(base, spaceRva,
                       patcher_logic::kOrigSpaceImmediate,
                       patcher_logic::kPatchedSpaceImmediate,
                       patcher_logic::kSpaceImmediateLen)) {
            ++ok;
        } else {
            ++fail;
        }
    }

    if (NopRangeRelaxed(base, sentinelRva + patcher_logic::kRelPatch7FxfSpace,
                        patcher_logic::kPatch7FxfSpaceNopLen, "FXF space")) {
        ++ok;
    } else {
        ++fail;
    }

    if (NopRangeRelaxed(base, sentinelRva + patcher_logic::kRelPatch8FxfNextX,
                        patcher_logic::kPatch8FxfNextXNopLen, "FXF nextX")) {
        ++ok;
    } else {
        ++fail;
    }

    {
        const DWORD msgNumRva = sentinelRva + patcher_logic::kRelPatch9WindowMsgNum;
        const BYTE orig[] = {patcher_logic::kOrigWindowMsgNum};
        const BYTE repl[] = {patcher_logic::kPatchedWindowMsgNum};
        WaitForBytes(base, msgNumRva, orig, sizeof(orig));
        if (PatchBytes(base, msgNumRva, orig, repl, sizeof(orig))) {
            ++ok;
        } else {
            ++fail;
        }
    }

    if (NopRangeRelaxed(base, sentinelRva + patcher_logic::kRelPatch10FxfWindowMsgNum,
                        patcher_logic::kPatch10FxfWindowMsgNumNopLen,
                        "FXF windowMessageNumber")) {
        ++ok;
    } else {
        ++fail;
    }

    Log("Patches applied: %d OK, %d FAILED", ok, fail);
}

DWORD FindUnpackedSentinelRva(BYTE* base) {
    if (patcher_logic::BytesMatch(base + patcher_logic::kPrintMessageSentinelRva,
                                  patcher_logic::kPrintMessageSentinel,
                                  patcher_logic::kPrintMessageSentinelLen)) {
        return patcher_logic::kPrintMessageSentinelRva;
    }
    if (patcher_logic::BytesMatch(base + patcher_logic::kAltPrintMessageSentinelRva,
                                  patcher_logic::kPrintMessageSentinel,
                                  patcher_logic::kPrintMessageSentinelLen)) {
        return patcher_logic::kAltPrintMessageSentinelRva;
    }
    for (DWORD rva = patcher_logic::kTextSectionScanStartRva;
         rva + patcher_logic::kPrintMessageSentinelLen <= patcher_logic::kTextSectionScanEndRva;
         ++rva) {
        if (patcher_logic::BytesMatch(base + rva,
                                      patcher_logic::kPrintMessageSentinel,
                                      patcher_logic::kPrintMessageSentinelLen)) {
            return rva;
        }
    }
    return 0;
}

DWORD WINAPI PatcherThread(LPVOID) {
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    Log("Patcher: ImageBase=0x%p, waiting for code to unpack...", base);

    for (int i = 0; i < kUnpackPollMaxRetries && !g_shutdownRequested; ++i) {
        __try {
            const DWORD sentinelRva = FindUnpackedSentinelRva(base);
            if (sentinelRva != 0) {
                Log("Patcher: Code unpacked at RVA 0x%X (detected at iteration %d)",
                    sentinelRva, i);
                Sleep(kPostUnpackSettleMs);
                ApplyPatches(base, sentinelRva);
                return 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        Sleep(kUnpackPollSleepMs);
    }

    Log("Patcher: TIMEOUT - sentinel pattern not found after 30 seconds");
    return 1;
}

}  // namespace

void PatcherInit() {
    (void)menu_autopatch::EnsureGameFilesPatched();
    g_shutdownRequested = false;
    g_patchThread = CreateThread(nullptr, 0, PatcherThread, nullptr, 0, nullptr);
    if (!g_patchThread) {
        Log("Patcher: Failed to create thread, error %u", GetLastError());
    }
}

void PatcherShutdown() {
    g_shutdownRequested = true;
    if (g_patchThread) {
        WaitForSingleObject(g_patchThread, kThreadShutdownWaitMs);
        CloseHandle(g_patchThread);
        g_patchThread = nullptr;
    }
}
