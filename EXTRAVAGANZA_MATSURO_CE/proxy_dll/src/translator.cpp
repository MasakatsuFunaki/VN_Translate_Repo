// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator.h"
#include "log.h"
#include <cstring>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdio>

// ==========================================================================
// Runtime translator for EXTRAVAGANZA ~Matsuro~ CE (BLACKCyc engine).
//
// Hooks FUN_00c20f40 (text splitter, RVA 0x0FF140) to intercept the CP932
// text string (param_4 / [esp+0x10] at call site) and replace it with
// the English translation from a lookup table.
//
// The text splitter signature (thiscall):
//   void __thiscall TextSplitter(this, int mode, uint flags, char* text, int param5)
// In x86 thiscall: ecx=this, stack: mode, flags, text, param5
//   text = [esp+0x0C] at function entry (after return addr is pushed)
//
// We hook the first 6 bytes of the function:
//   55              push ebp
//   8B EC           mov ebp, esp
//   81 EC 5C 01 00 00   sub esp, 0x15C
// Total: 9 bytes. We use the first 6 (push ebp; mov ebp,esp; first 3 of sub)
// Actually let's use 9 bytes for the full prologue to avoid mid-instruction splits.
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
        // Only log access violations (skip illegal instructions from unpacker)
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
            // Dump a few stack dwords for context
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
    return EXCEPTION_CONTINUE_SEARCH;  // let default handler terminate
}

// ── JSON loader (minimal, no external deps) ────────────────────────────
// Parses the flat translation_table.json: {"jp_cp932_hex": "en_text", ...}
// Actually, we'll use a simpler format: one entry per line, tab-separated:
//   JP_TEXT\tEN_TEXT
// This avoids needing a JSON parser in C++.

static std::string WideToCP932(const wchar_t* wide)
{
    int len = WideCharToMultiByte(932, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len - 1, '\0');
    WideCharToMultiByte(932, 0, wide, -1, &result[0], len, nullptr, nullptr);
    return result;
}

// Unescape \\r\\n and \\t in a string (from the TSV file)
static void UnescapeInPlace(std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[i + 1];
            if (next == 'r') { out += '\r'; i++; continue; }
            if (next == 'n') { out += '\n'; i++; continue; }
            if (next == 't') { out += '\t'; i++; continue; }
            if (next == '\\') { out += '\\'; i++; continue; }
        }
        out += s[i];
    }
    s = out;
}

static bool LoadTranslations()
{
    // Look for translation_table.tsv next to the game exe
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Replace exe filename with translation_table.tsv
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) {
        wcscpy(lastSlash + 1, L"translation_table.tsv");
    } else {
        wcscpy(exePath, L"translation_table.tsv");
    }

    FILE* f = _wfopen(exePath, L"rb");
    if (!f) {
        Log("Translator: translation_table.tsv not found");
        // Try UTF-8 path too
        char pathA[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, exePath, -1, pathA, MAX_PATH, nullptr, nullptr);
        Log("  Tried: %s", pathA);
        return false;
    }

    // Read entire file
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<char> buf(size + 1);
    fread(buf.data(), 1, size, f);
    buf[size] = '\0';
    fclose(f);

    // Skip UTF-8 BOM if present
    char* ptr = buf.data();
    if (size >= 3 && (unsigned char)ptr[0] == 0xEF &&
        (unsigned char)ptr[1] == 0xBB && (unsigned char)ptr[2] == 0xBF) {
        ptr += 3;
    }

    // Parse line by line: JP_CP932\tEN_CP932
    int count = 0;
    char* line = ptr;
    while (*line) {
        // Find end of line
        char* eol = line;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;

        // Null-terminate this line
        char savedEol = *eol;
        *eol = '\0';

        // Find tab separator
        char* tab = strchr(line, '\t');
        if (tab && tab > line) {
            *tab = '\0';
            std::string jp(line);
            std::string en(tab + 1);

            // Unescape \r\n sequences
            UnescapeInPlace(jp);
            UnescapeInPlace(en);

            if (!jp.empty() && !en.empty()) {
                g_translations[jp] = en;
                count++;
            }
        }

        // Advance past EOL
        *eol = savedEol;
        line = eol;
        while (*line == '\r' || *line == '\n') line++;
    }

    Log("Translator: loaded %d translations from translation_table.tsv", count);
    g_translationsLoaded = true;
    return count > 0;
}

// ── Hook function ──────────────────────────────────────────────────────
// Called from the trampoline with: ecx=this, and the original stack frame.
// We need to read param_4 (text pointer) from the stack and potentially
// replace it before the original function processes it.
//
// Stack layout at hook entry (after our trampoline saves):
//   [esp+0x04] = return addr (caller of TextSplitter)
//   [esp+0x08] = param_2 (mode)
//   [esp+0x0C] = param_3 (flags)
//   [esp+0x10] = param_4 (text pointer) ← WE MODIFY THIS
//   [esp+0x14] = param_5
//
// We use a __cdecl helper called from the trampoline that receives
// a pointer to the stack location of param_4.

// Buffer pool for translated strings (they must persist until the function returns)
// We use a ring buffer of slots since only a few are active at once.
#define TRANS_POOL_SLOTS 64
#define TRANS_SLOT_SIZE 2048
static char g_transPool[TRANS_POOL_SLOTS][TRANS_SLOT_SIZE];
static int g_transPoolIdx = 0;

static char* AllocTransSlot()
{
    int idx = g_transPoolIdx;
    g_transPoolIdx = (g_transPoolIdx + 1) % TRANS_POOL_SLOTS;
    return g_transPool[idx];
}

// Called from trampoline: param is pointer to the text pointer on the stack.
// Returns the (possibly modified) text pointer.
static bool IsReadablePtr(const void* ptr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return true;
}

// Inner function does the actual lookup (no SEH — uses C++ objects)
static const char* TranslateTextInner(const char* originalText)
{
    if (!g_translationsLoaded || !originalText) {
        return originalText;
    }

    // Validate pointer before dereferencing
    if (!IsReadablePtr(originalText)) {
        Log("Translator: INVALID ptr 0x%p at hit %d", originalText, g_hookHits);
        return originalText;
    }

    if (!originalText[0]) {
        return originalText;
    }

    g_hookHits++;

    // Log first 20 hits for debugging, then every 200th
    if (g_hookHits <= 20 || (g_hookHits % 200) == 0) {
        Log("Hook #%d: ptr=0x%p text=[%.60s]", g_hookHits, originalText, originalText);
    }

    // Look up the Japanese text in the translation table
    auto it = g_translations.find(originalText);
    if (it != g_translations.end()) {
        g_translationHits++;

        // Copy English text to a persistent buffer
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

// SEH wrapper (no C++ objects — safe for __try/__except)
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
// Hook the text splitter at RVA 0x0FF140.
// Original bytes (9 bytes):
//   55                push ebp
//   8B EC             mov ebp, esp
//   81 EC 5C 01 00 00 sub esp, 0x15C
//
// Trampoline:
//   1. Save all registers
//   2. Read text pointer from [esp + saved_regs_size + 0x10]
//   3. Call TranslateText(text)
//   4. Write result back to [esp + saved_regs_size + 0x10]
//   5. Restore registers
//   6. Execute original 9 bytes
//   7. Jump back to hookSite + 9

static bool InstallTextHook(BYTE* base)
{
    const DWORD hookRVA = 0x0FF140;
    const size_t hookLen = 9;  // 55 8B EC 81 EC 5C 01 00 00
    g_hookSite = base + hookRVA;
    BYTE* returnAddr = g_hookSite + hookLen;

    const BYTE expected[] = {
        0x55,                               // push ebp
        0x8B, 0xEC,                         // mov ebp, esp
        0x81, 0xEC, 0x5C, 0x01, 0x00, 0x00  // sub esp, 0x15C
    };

    if (memcmp(g_hookSite, expected, hookLen) != 0) {
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

    // At this point the stack looks like:
    //   [esp+0x00] = return address (caller)
    //   [esp+0x04] = param_2 (mode)      - pushed by caller
    //   [esp+0x08] = param_3 (flags)     - pushed by caller
    //   [esp+0x0C] = param_4 (text ptr)  - pushed by caller
    //   [esp+0x10] = param_5             - pushed by caller
    // ecx = this pointer (thiscall)

    // Save ecx (this pointer, thiscall convention)
    *p++ = 0x51;  // push ecx

    // pushad (saves EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI = 8*4=32 bytes)
    *p++ = 0x60;  // pushad

    // Now stack: [pushad 32 bytes][saved ecx 4 bytes][ret addr][mode][flags][text][param5]
    // text pointer is at [esp + 0x20 + 0x04 + 0x0C] = [esp + 0x30]
    //   pushad=32=0x20, push ecx=4=0x04, text offset from original esp=0x0C

    // push the text pointer value as argument to TranslateText
    // mov eax, [esp+0x30]
    *p++ = 0x8B; *p++ = 0x44; *p++ = 0x24; *p++ = 0x30;
    // push eax
    *p++ = 0x50;

    // call TranslateText
    *p++ = 0xE8;
    DWORD callTarget = (DWORD)(BYTE*)&TranslateText - (DWORD)(p + 4);
    *(DWORD*)p = callTarget;
    p += 4;

    // add esp, 4 (clean up cdecl arg)
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;

    // Write returned pointer back to the text slot on stack
    // mov [esp+0x30], eax
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x30;

    // popad
    *p++ = 0x61;

    // pop ecx (restore this pointer)
    *p++ = 0x59;

    // Execute original 9 bytes:
    //   push ebp; mov ebp,esp; sub esp,0x15C
    memcpy(p, expected, hookLen);
    p += hookLen;

    // jmp back to returnAddr (hookSite + 9)
    *p++ = 0xE9;
    *(DWORD*)p = (DWORD)returnAddr - (DWORD)(p + 4);
    p += 4;

    // Patch hook site: overwrite with JMP to trampoline + NOPs
    DWORD oldProtect;
    if (!VirtualProtect(g_hookSite, hookLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("TEXT HOOK: VirtualProtect failed");
        return false;
    }

    // JMP rel32 to trampoline (5 bytes)
    g_hookSite[0] = 0xE9;
    *(DWORD*)(g_hookSite + 1) = (DWORD)g_trampoline - (DWORD)(g_hookSite + 5);
    // NOP remaining 4 bytes
    g_hookSite[5] = 0x90;
    g_hookSite[6] = 0x90;
    g_hookSite[7] = 0x90;
    g_hookSite[8] = 0x90;

    VirtualProtect(g_hookSite, hookLen, oldProtect, &oldProtect);

    Log("TEXT HOOK: installed at RVA 0x%X (trampoline at 0x%p)", hookRVA, g_trampoline);
    return true;
}

// ── Thread: wait for unpack, then install hook ─────────────────────────
static DWORD WINAPI TranslatorThread(LPVOID)
{
    // First, load translation table (can do before code unpacks)
    LoadTranslations();
    if (!g_translationsLoaded) {
        Log("Translator: no translations loaded, hook not installed");
        return 0;
    }

    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    Log("Translator: ImageBase=0x%p, waiting for code to unpack...", base);

    // Wait for the text splitter function to be unpacked
    // Sentinel: first 6 bytes of FUN at RVA 0x0FF140
    const BYTE sentinel[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x5C };
    const DWORD sentinelRVA = 0x0FF140;

    for (int i = 0; i < 3000 && !g_shutdownRequested; i++) {
        __try {
            if (memcmp(base + sentinelRVA, sentinel, sizeof(sentinel)) == 0) {
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
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Page not yet committed
        }
        Sleep(10);
    }

    Log("Translator: TIMEOUT waiting for code to unpack");
    return 0;
}

static PVOID g_vehHandle = nullptr;

void TranslatorInit()
{
    // Register crash handler first so we catch any crash during init or runtime
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
