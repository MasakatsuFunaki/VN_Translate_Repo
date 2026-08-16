// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator.h"
#include "translator_logic.h"
#include "log.h"
#include <windows.h>
#include <cstring>
#include <string>
#include <vector>
#include <cstdio>

// ==========================================================================
// Runtime translator for amakano2pe (CatSystem2 / cs2.exe).
//
// Hooks FUN_0063a9b0 (RVA 0x23A9B0), the universal cp932/UTF-8 -> wide
// converter that all in-game text passes through before being drawn.
// Signature (cdecl):
//   void FUN_0063a9b0(char* src, wchar_t* dst);
// Internally calls MultiByteToWideChar(CP_UTF8 = 0xfde9, ...).
//
// Stack layout at entry (cdecl, no thiscall):
//   [esp+0x00] = return address
//   [esp+0x04] = src  (char*) <- intercepted
//   [esp+0x08] = dst  (wchar_t*)
//
// Hook prologue (14 bytes stolen):
//   83 EC 20            sub  esp, 20h
//   A1 A4 C5 A6 00      mov  eax, [00A6C5A4h]
//   33 C4               xor  eax, esp
//   89 44 24 1C         mov  [esp+1Ch], eax
//
// Translation table file format (UTF-8, tab-separated):
//   <jp_utf8>\t<en_utf8>\n
//   Escapes: \\ \r \n \t
// Lookup misses pass through unchanged.
//
// The pure logic for TSV parsing + UTF-8-safe truncation + direct
// lookup lives in translator_logic.{h,cpp} so it can be exercised
// by the gtest suite under tests/ without pulling in any Win32.
// ==========================================================================

static translator_logic::TranslationMap g_translations;
static bool g_translationsLoaded = false;

static BYTE* g_trampoline = nullptr;
static BYTE* g_hookSite = nullptr;

static int g_hookHits = 0;
static int g_translationHits = 0;
static int g_truncations = 0;
static int g_fontScaleHits = 0;

// Font shrink ratio: every lfHeight is multiplied by FONT_NUM/FONT_DEN before
// being written into the LOGFONTW struct. 9/10 = 90% of original size,
// enough to fit ~25% more English chars per textbox row.
static const int FONT_NUM = 9;
static const int FONT_DEN = 10;

// Maximum English characters per textbox line before a word-wrap break
// is inserted. The engine wraps at ~80-82 chars for typical English at
// 9/10 font scale; 76 gives ~5-char margin for wide letters (W, M) and
// punctuation without cutting lines excessively short.
static const size_t WRAP_CHARS = 76;

// Running column position on the engine's current textbox line. The
// engine concatenates consecutive MESSAGEs into a single rendered
// line, so per-MESSAGE wrap that always assumes col=0 fails to wrap
// correctly when a short MESSAGE is appended to a longer prior one
// (mid-word break). We carry the col across calls and reset it when
// a new utterance starts (no leading `\n` continuation marker).
static size_t g_currentCol = 0;

// Last-call dedup. The engine calls FUN_0063a9b0 (our hook) TWICE
// per MESSAGE -- once during layout/measurement, once during draw --
// so a non-cached wrap path produces a different output the second
// time (col was already advanced by the first call), and the engine
// draws the WRONG (un-broken) output. Solution: when the second call
// arrives with the same src bytes as the last one, return the same
// translated slot and DO NOT advance col. The per-MESSAGE col carry
// across distinct MESSAGEs still works because we update the cache
// only on a fresh src.
static std::string g_lastSrcKey;
static const char* g_lastOutput = nullptr;

// -- TSV loader (wraps the pure parser with Win32 file I/O) --------------

static bool LoadTranslations()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) wcscpy(lastSlash + 1, L"translation_table.tsv");
    else           wcscpy(exePath, L"translation_table.tsv");

    FILE* f = _wfopen(exePath, L"rb");
    if (!f) {
        Log("Translator: translation_table.tsv not found");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return false; }
    std::vector<char> buf(static_cast<size_t>(size));
    size_t n = fread(buf.data(), 1, buf.size(), f);
    fclose(f);

    int count = translator_logic::ParseTsvBuffer(buf.data(), n, g_translations);
    Log("Translator: loaded %d translations from translation_table.tsv", count);
    g_translationsLoaded = (count > 0);
    return count > 0;
}

// -- String pool for translated output -----------------------------------

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

static const char* TranslateTextInner(const char* src)
{
    if (!g_translationsLoaded || !src) return src;
    if (!IsReadablePtr(src)) return src;
    if (!src[0]) return src;

    g_hookHits++;

    // Log first 200 distinct strings, then 1-in-100 thereafter
    if (g_hookHits <= 200 || (g_hookHits % 100) == 0) {
        Log("Hook #%d: ptr=0x%p [%.80s]", g_hookHits, src, src);
    }

    const std::string key(src);
    const std::string* en = translator_logic::FindTranslation(g_translations, key);
    if (!en) return src;

    // Engine calls us twice per MESSAGE (layout pass + draw pass).
    // Without dedup the second call re-wraps from an already-advanced
    // col and may drop the break our first call inserted, leaving the
    // engine to mid-word break the un-wrapped string. Returning the
    // same cached slot makes the second call a no-op for state.
    if (!g_lastSrcKey.empty() && g_lastSrcKey == key && g_lastOutput) {
        if (g_translationHits <= 50 || (g_translationHits % 100) == 0) {
            Log("Translate dedup: [%.50s] -> [%.50s] col=%zu",
                src, g_lastOutput, g_currentCol);
        }
        return g_lastOutput;
    }

    g_translationHits++;
    char* slot = AllocTransSlot();

    // Word-wrap the English text before copying into the slot so the
    // engine's pixel-based line-break never cuts mid-word. The wrapper
    // inserts `\n` control codes at word boundaries slightly inside the
    // engine's ~80-char pixel limit (WRAP_CHARS = 76).
    //
    // The engine concatenates consecutive MESSAGEs into a single
    // textbox line, so we carry g_currentCol across calls. When the EN
    // begins with the JP-authored `\n` continuation marker, this
    // MESSAGE is appended to the prior one -- soften the marker to a
    // space and wrap starting from the running column. Otherwise
    // (fresh utterance / new textbox), reset col to 0.
    std::string wrapped = *en;
    bool isContinuation = translator_logic::StripLeadingControlNewline(wrapped);
    size_t startCol = isContinuation ? g_currentCol : 0;
    if (!isContinuation) g_currentCol = 0;
    g_currentCol = translator_logic::WordWrapForEngine(
        wrapped, WRAP_CHARS, g_currentCol);

    // Per-call wrap trace (throttled the same way as Translate logs).
    // Logs cont/start_col/end_col so you can replay the column carry
    // and verify each \n landed where the wrapper claimed it would.
    if (g_translationHits <= 50 || (g_translationHits % 100) == 0) {
        Log("Wrap #%d: cont=%d sc=%zu ec=%zu en=[%.80s] out=[%.80s]",
            g_translationHits, isContinuation ? 1 : 0,
            startCol, g_currentCol, en->c_str(), wrapped.c_str());
    }

    // SAFETY: cap translation length to avoid downstream buffer
    // overflows. Empirically the engine tolerates several x the source
    // length (heap-allocated dst), so we allow a generous margin --
    // enough for word-wrapped multi-line English replacing a short JP
    // string, but not unbounded.
    size_t srcLen  = strlen(src);
    size_t wrapLen = wrapped.size();
    size_t maxLen  = srcLen * 8 + 64;
    if (maxLen > 512) maxLen = 512;
    size_t copyLen = wrapLen;
    if (copyLen > maxLen) {
        copyLen = translator_logic::Utf8SafeTruncate(wrapped.c_str(), maxLen);
        g_truncations++;
        if (g_truncations <= 30 || (g_truncations % 100) == 0) {
            Log("Truncate #%d: src=%u en=%u kept=%u [%.40s] -> [%.*s]",
                g_truncations, (unsigned)srcLen, (unsigned)wrapLen,
                (unsigned)copyLen, src, (int)copyLen, wrapped.c_str());
        }
    }
    if (copyLen >= TRANS_SLOT_SIZE) copyLen = TRANS_SLOT_SIZE - 1;
    memcpy(slot, wrapped.c_str(), copyLen);
    slot[copyLen] = '\0';
    if (g_translationHits <= 50 || (g_translationHits % 100) == 0) {
        Log("Translate #%d: [%.50s] -> [%.50s]",
            g_translationHits, src, slot);
    }

    // Cache for the dedup check above. Engine's next layout/draw pass
    // for this same MESSAGE will return `slot` directly without a
    // second wrap, keeping g_currentCol stable.
    g_lastSrcKey = key;
    g_lastOutput = slot;
    return slot;
}

static const char* __cdecl TranslateText(const char* src)
{
    __try {
        return TranslateTextInner(src);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("Translator: EXCEPTION in TranslateText, ptr=0x%p hit=%d",
            src, g_hookHits);
        return src;
    }
}

// -- Hook installation ---------------------------------------------------

static bool InstallTextHook(BYTE* base)
{
    const DWORD hookRVA = 0x23A9B0;
    const size_t hookLen = 14;
    g_hookSite = base + hookRVA;
    BYTE* returnAddr = g_hookSite + hookLen;

    const BYTE expected[hookLen] = {
        0x83, 0xEC, 0x20,                    // sub esp, 20h
        0xA1, 0xA4, 0xC5, 0xA6, 0x00,        // mov eax, [00A6C5A4h]
        0x33, 0xC4,                          // xor eax, esp
        0x89, 0x44, 0x24, 0x1C               // mov [esp+1Ch], eax
    };

    if (memcmp(g_hookSite, expected, hookLen) != 0) {
        Log("TEXT HOOK: bytes mismatch at RVA 0x%X", hookRVA);
        Log("  Found: %02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X",
            g_hookSite[0], g_hookSite[1], g_hookSite[2], g_hookSite[3],
            g_hookSite[4], g_hookSite[5], g_hookSite[6], g_hookSite[7],
            g_hookSite[8], g_hookSite[9], g_hookSite[10], g_hookSite[11],
            g_hookSite[12], g_hookSite[13]);
        return false;
    }

    g_trampoline = (BYTE*)VirtualAlloc(nullptr, 256,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampoline) {
        Log("TEXT HOOK: VirtualAlloc failed");
        return false;
    }

    BYTE* p = g_trampoline;

    // cdecl, no this/ecx to preserve. Stack at entry:
    //   [esp+0x00] = ret addr
    //   [esp+0x04] = src   <- we patch this
    //   [esp+0x08] = dst
    //
    // After pushad (32 bytes), src is at [esp+0x20+0x04] = [esp+0x24].

    *p++ = 0x60;                             // pushad

    *p++ = 0x8B; *p++ = 0x44; *p++ = 0x24;   // mov eax, [esp+0x24]
    *p++ = 0x24;
    *p++ = 0x50;                             // push eax
    *p++ = 0xE8;                             // call TranslateText
    *(DWORD*)p = (DWORD)(BYTE*)&TranslateText - (DWORD)(p + 4);
    p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;   // add esp, 4
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24;   // mov [esp+0x24], eax
    *p++ = 0x24;

    *p++ = 0x61;                             // popad

    // Execute stolen prologue (14 bytes)
    memcpy(p, expected, hookLen);
    p += hookLen;

    // jmp back to hookSite + 14
    *p++ = 0xE9;
    *(DWORD*)p = (DWORD)returnAddr - (DWORD)(p + 4);
    p += 4;

    DWORD oldProtect;
    if (!VirtualProtect(g_hookSite, hookLen, PAGE_EXECUTE_READWRITE,
                        &oldProtect)) {
        Log("TEXT HOOK: VirtualProtect failed");
        return false;
    }

    // Write 5-byte JMP + NOP padding
    g_hookSite[0] = 0xE9;
    *(DWORD*)(g_hookSite + 1) = (DWORD)g_trampoline - (DWORD)(g_hookSite + 5);
    for (size_t i = 5; i < hookLen; i++) g_hookSite[i] = 0x90;

    DWORD dummy;
    VirtualProtect(g_hookSite, hookLen, oldProtect, &dummy);
    FlushInstructionCache(GetCurrentProcess(), g_hookSite, hookLen);

    Log("TEXT HOOK: installed at 0x%08X (RVA 0x%X), trampoline=0x%08X",
        (DWORD)g_hookSite, hookRVA, (DWORD)g_trampoline);
    return true;
}

// -- Font-size scaling hook ----------------------------------------------
// Hooks FUN_0063d000 (RVA 0x23D000) -- global LOGFONTW.lfHeight setter.
// Stack at entry: [esp+4]=height. ECX = `this` (font ctx).
// Stolen prologue (7 bytes, no RIP-relative bytes): 6B 81 84 0B 00 00 5C
//   = imul eax,[ecx+0xB84],0x5C

static int __cdecl ScaleHeight(int h)
{
    g_fontScaleHits++;
    if (h > 0) return (h * FONT_NUM) / FONT_DEN;
    if (h < 0) return -(((-h) * FONT_NUM) / FONT_DEN);
    return 0;
}

static bool InstallFontScaleHook(BYTE* base)
{
    const DWORD hookRVA = 0x23D000;
    const size_t hookLen = 7;
    BYTE* site = base + hookRVA;
    BYTE* returnAddr = site + hookLen;

    const BYTE expected[hookLen] = {
        0x6B, 0x81, 0x84, 0x0B, 0x00, 0x00, 0x5C
    };
    if (memcmp(site, expected, hookLen) != 0) {
        Log("FONT HOOK: bytes mismatch at RVA 0x%X", hookRVA);
        return false;
    }

    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 128,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;

    BYTE* p = tramp;
    *p++ = 0x51;                                  // push ecx
    *p++ = 0x8B; *p++ = 0x44; *p++ = 0x24; *p++ = 0x08; // mov eax,[esp+8]
    *p++ = 0x50;                                  // push eax
    *p++ = 0xE8;                                  // call ScaleHeight
    *(DWORD*)p = (DWORD)(BYTE*)&ScaleHeight - (DWORD)(p + 4);
    p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;        // add esp,4
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x08; // mov [esp+8],eax
    *p++ = 0x59;                                  // pop ecx

    memcpy(p, expected, hookLen);                 // stolen prologue
    p += hookLen;

    *p++ = 0xE9;                                  // jmp back
    *(DWORD*)p = (DWORD)returnAddr - (DWORD)(p + 4);
    p += 4;

    DWORD oldProtect;
    if (!VirtualProtect(site, hookLen, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    site[0] = 0xE9;
    *(DWORD*)(site + 1) = (DWORD)tramp - (DWORD)(site + 5);
    for (size_t i = 5; i < hookLen; i++) site[i] = 0x90;
    DWORD dummy;
    VirtualProtect(site, hookLen, oldProtect, &dummy);
    FlushInstructionCache(GetCurrentProcess(), site, hookLen);

    Log("FONT HOOK: installed at 0x%08X (RVA 0x%X), scale=%d/%d",
        (DWORD)site, hookRVA, FONT_NUM, FONT_DEN);
    return true;
}

void TranslatorInit()
{
    LoadTranslations();
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    Log("Translator: cs2.exe base=0x%08X", (DWORD)base);
    if (!InstallTextHook(base)) {
        Log("Translator: hook install FAILED - translation disabled");
    }
    if (!InstallFontScaleHook(base)) {
        Log("Translator: font-scale hook install FAILED - text size unchanged");
    }
}

void TranslatorShutdown()
{
    Log("Translator: %d hook hits, %d translations applied, %d truncations, %d font scales",
        g_hookHits, g_translationHits, g_truncations, g_fontScaleHits);
}
