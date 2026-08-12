// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator.h"
#include "translator_logic.h"
#include "patcher_logic.h"
#include "log.h"
#include <cstring>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdio>

// ==========================================================================
// Runtime translator for mushigurui HD10 (BLACKCyc engine).
//
// Hooks FUN_00523850 (text splitter, RVA 0x063850) to intercept the CP932
// text string (param_4 / [esp+0x10] at call site) and replace it with
// the English translation from a lookup table.
//
// The text splitter signature (thiscall):
//   void __thiscall TextSplitter(this, int mode, uint flags, char* text, int param5)
// In x86 thiscall: ecx=this, stack: mode, flags, text, param5
//   text = [esp+0x0C] at function entry (after return addr is pushed)
//
// We hook the first 9 bytes of the function:
//   55                    push ebp
//   8B EC                 mov ebp, esp
//   81 EC 3C 05 00 00     sub esp, 0x53C
// ==========================================================================

// Translation table: CP932 Japanese string -> CP932 English string
static std::unordered_map<std::string, std::string> g_translations;
static bool g_translationsLoaded = false;

// Hook infrastructure
static BYTE* g_trampoline = nullptr;
static BYTE* g_hookSite = nullptr;
static HANDLE g_translatorThread = nullptr;
static volatile bool g_shutdownRequested = false;

// Stats
static int g_hookHits = 0;
static int g_translationHits = 0;

// Vectored Exception Handler — catches any crash and logs the address
static LONG WINAPI TranslatorVEH(EXCEPTION_POINTERS* ep)
{
    if (ep && ep->ExceptionRecord && ep->ContextRecord) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_ACCESS_VIOLATION ||
            code == EXCEPTION_STACK_OVERFLOW) {
            DWORD eip = ep->ContextRecord->Eip;
            DWORD esp = ep->ContextRecord->Esp;
            DWORD ecx = ep->ContextRecord->Ecx;
            DWORD trampolineAddr = (DWORD)g_trampoline;
            DWORD hookAddr = (DWORD)g_hookSite;
            Log("=== CRASH DETECTED ===");
            Log("  ExceptionCode: 0x%08X", code);
            Log("  EIP: 0x%08X  ESP: 0x%08X  ECX: 0x%08X", eip, esp, ecx);
            Log("  EAX: 0x%08X  EBX: 0x%08X  EDX: 0x%08X",
                ep->ContextRecord->Eax, ep->ContextRecord->Ebx,
                ep->ContextRecord->Edx);
            Log("  Trampoline: 0x%08X  HookSite: 0x%08X", trampolineAddr, hookAddr);
            Log("  Hook hits: %d  Translation hits: %d", g_hookHits, g_translationHits);
            if (code == EXCEPTION_ACCESS_VIOLATION &&
                ep->ExceptionRecord->NumberParameters >= 2) {
                Log("  Access type: %s  Address: 0x%08X",
                    ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
                    (DWORD)ep->ExceptionRecord->ExceptionInformation[1]);
            }
            __try {
                DWORD* stackPtr = (DWORD*)esp;
                Log("  Stack: %08X %08X %08X %08X %08X %08X %08X %08X",
                    stackPtr[0], stackPtr[1], stackPtr[2], stackPtr[3],
                    stackPtr[4], stackPtr[5], stackPtr[6], stackPtr[7]);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                Log("  Stack: <unreadable>");
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ── TSV loader ─────────────────────────────────────────────────────────
// File IO stays in this file; all the byte-level work (BOM detection,
// line splitting, escape expansion) is delegated to translator_logic
// so the same code path is exercised by the gtest suite.

static bool LoadTranslations()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) {
        wcscpy(lastSlash + 1, L"translation_table.tsv");
    } else {
        wcscpy(exePath, L"translation_table.tsv");
    }

    FILE* f = _wfopen(exePath, L"rb");
    if (!f) {
        Log("Translator: translation_table.tsv not found");
        char pathA[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, exePath, -1, pathA, MAX_PATH, nullptr, nullptr);
        Log("  Tried: %s", pathA);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return false;
    }

    std::vector<char> buf(static_cast<size_t>(size));
    size_t nread = fread(buf.data(), 1, buf.size(), f);
    fclose(f);

    const std::size_t bomLen = translator_logic::Utf8BomLen(buf.data(), nread);
    const int count = translator_logic::ParseTsvBuffer(
        buf.data() + bomLen,
        nread - bomLen,
        g_translations);

    Log("Translator: loaded %d translations from translation_table.tsv", count);
    // Preserve original semantics: flag is set once the file was read
    // successfully, even if no entries parsed. The hook still installs
    // so a missing TSV is the only "don't hook" case.
    g_translationsLoaded = true;
    return count > 0;
}

// ── Hook function ──────────────────────────────────────────────────────

#define TRANS_POOL_SLOTS 64
#define TRANS_SLOT_SIZE 4096
static char g_transPool[TRANS_POOL_SLOTS][TRANS_SLOT_SIZE];
static int g_transPoolIdx = 0;

static char* AllocTransSlot()
{
    int idx = g_transPoolIdx;
    g_transPoolIdx = (g_transPoolIdx + 1) % TRANS_POOL_SLOTS;
    return g_transPool[idx];
}

static bool IsReadablePtr(const void* ptr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return true;
}

static const char* TranslateTextInner(const char* originalText)
{
    if (!g_translationsLoaded || !originalText) {
        return originalText;
    }

    if (!IsReadablePtr(originalText)) {
        Log("Translator: INVALID ptr 0x%p at hit %d", originalText, g_hookHits);
        return originalText;
    }

    if (!originalText[0]) {
        return originalText;
    }

    g_hookHits++;

    if (g_hookHits <= 20 || (g_hookHits % 200) == 0) {
        Log("Hook #%d: ptr=0x%p text=[%.60s]", g_hookHits, originalText, originalText);
    }

    auto it = g_translations.find(originalText);
    if (it != g_translations.end()) {
        g_translationHits++;

        char* slot = AllocTransSlot();
        size_t len = it->second.size();
        if (len >= TRANS_SLOT_SIZE) len = TRANS_SLOT_SIZE - 1;
        memcpy(slot, it->second.c_str(), len);
        slot[len] = '\0';

        if (g_translationHits <= 20 || (g_translationHits % 200) == 0) {
            Log("Translate #%d: [%.50s] -> [%.50s]",
                g_translationHits, originalText, slot);
        }
        return slot;
    }

    return originalText;
}

static const char* __cdecl TranslateText(const char* originalText)
{
    __try {
        return TranslateTextInner(originalText);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("Translator: EXCEPTION in TranslateText, ptr=0x%p hit=%d",
            originalText, g_hookHits);
        return originalText;
    }
}

// ── Hook installation ──────────────────────────────────────────────────
// Hook the text splitter at RVA 0x063850.
// Original bytes (9 bytes):
//   55                    push ebp
//   8B EC                 mov ebp, esp
//   81 EC 3C 05 00 00     sub esp, 0x53C

static bool InstallTextHook(BYTE* base)
{
    const DWORD hookRVA = 0x063850;
    const size_t hookLen = 9;  // 55 8B EC 81 EC 3C 05 00 00
    g_hookSite = base + hookRVA;
    BYTE* returnAddr = g_hookSite + hookLen;

    const BYTE expected[] = {
        0x55,                                   // push ebp
        0x8B, 0xEC,                             // mov ebp, esp
        0x81, 0xEC, 0x3C, 0x05, 0x00, 0x00     // sub esp, 0x53C
    };

    if (!patcher_logic::BytesMatch(g_hookSite, expected, hookLen)) {
        Log("TEXT HOOK: bytes mismatch at RVA 0x%X", hookRVA);
        Log("  Expected: %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            expected[0], expected[1], expected[2], expected[3],
            expected[4], expected[5], expected[6], expected[7], expected[8]);
        Log("  Found:    %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            g_hookSite[0], g_hookSite[1], g_hookSite[2], g_hookSite[3],
            g_hookSite[4], g_hookSite[5], g_hookSite[6], g_hookSite[7],
            g_hookSite[8]);
        return false;
    }

    // Allocate executable trampoline
    g_trampoline = (BYTE*)VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
    if (!g_trampoline) {
        Log("TEXT HOOK: VirtualAlloc failed");
        return false;
    }

    BYTE* p = g_trampoline;

    // Stack at entry:
    //   [esp+0x00] = return address (caller)
    //   [esp+0x04] = param_2 (mode)
    //   [esp+0x08] = param_3 (flags)
    //   [esp+0x0C] = param_4 (text ptr) ← WE MODIFY THIS
    //   [esp+0x10] = param_5
    // ecx = this pointer (thiscall)

    // Save ecx (this pointer)
    *p++ = 0x51;  // push ecx

    // pushad (8*4=32 bytes)
    *p++ = 0x60;  // pushad

    // Stack: [pushad 32][saved ecx 4][ret][mode][flags][text][param5]
    // text = [esp + 0x20 + 0x04 + 0x0C] = [esp + 0x30]

    // mov eax, [esp+0x30]
    *p++ = 0x8B; *p++ = 0x44; *p++ = 0x24; *p++ = 0x30;
    // push eax
    *p++ = 0x50;

    // call TranslateText
    p = patcher_logic::EmitRelCall32(
            p, reinterpret_cast<std::uintptr_t>(&TranslateText));

    // add esp, 4 (clean up cdecl arg)
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;

    // mov [esp+0x30], eax  (write back translated pointer)
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x30;

    // popad
    *p++ = 0x61;

    // pop ecx (restore this pointer)
    *p++ = 0x59;

    // Execute original 9 bytes
    p = patcher_logic::EmitBytes(p, expected, hookLen);

    // jmp back to hookSite + 9
    p = patcher_logic::EmitRelJmp32(
            p, reinterpret_cast<std::uintptr_t>(returnAddr));

    // Patch hook site: JMP to trampoline + NOPs
    DWORD oldProtect;
    if (!VirtualProtect(g_hookSite, hookLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("TEXT HOOK: VirtualProtect failed");
        return false;
    }

    patcher_logic::EmitRelJmp32(
        g_hookSite, reinterpret_cast<std::uintptr_t>(g_trampoline));
    patcher_logic::FillNops(g_hookSite + 5, hookLen - 5);

    VirtualProtect(g_hookSite, hookLen, oldProtect, &oldProtect);

    Log("TEXT HOOK: installed at RVA 0x%X (trampoline at 0x%p)", hookRVA, g_trampoline);
    return true;
}

// ── Thread: wait for unpack, then install hook ─────────────────────────
static DWORD WINAPI TranslatorThread(LPVOID)
{
    LoadTranslations();
    if (!g_translationsLoaded) {
        Log("Translator: no translations loaded, hook not installed");
        return 0;
    }

    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    Log("Translator: ImageBase=0x%p, waiting for code to unpack...", base);

    // Wait for the text splitter function to be unpacked
    // Sentinel: first 6 bytes of FUN at RVA 0x063850
    const BYTE sentinel[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x3C };
    const DWORD sentinelRVA = 0x063850;

    for (int i = 0; i < 3000 && !g_shutdownRequested; i++) {
        __try {
            if (patcher_logic::BytesMatch(base + sentinelRVA, sentinel, sizeof(sentinel))) {
                Log("Translator: code unpacked (iteration %d)", i);
                Sleep(50);
                if (InstallTextHook(base)) {
                    Log("Translator: runtime translation ACTIVE (%d entries)",
                        (int)g_translations.size());
                } else {
                    Log("Translator: hook installation FAILED");
                }
                return 0;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        Sleep(10);
    }

    Log("Translator: TIMEOUT waiting for code to unpack");
    return 0;
}

static PVOID g_vehHandle = nullptr;

void TranslatorInit()
{
    g_vehHandle = AddVectoredExceptionHandler(1, TranslatorVEH);
    g_translatorThread = CreateThread(nullptr, 0, TranslatorThread, nullptr, 0, nullptr);
}

void TranslatorShutdown()
{
    g_shutdownRequested = true;
    if (g_translatorThread) {
        WaitForSingleObject(g_translatorThread, 2000);
        CloseHandle(g_translatorThread);
        g_translatorThread = nullptr;
    }

    if (g_translationsLoaded) {
        Log("Translator stats: %d hook hits, %d translations applied",
            g_hookHits, g_translationHits);
    }

    if (g_trampoline) {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = nullptr;
    }

    if (g_vehHandle) {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }
}
