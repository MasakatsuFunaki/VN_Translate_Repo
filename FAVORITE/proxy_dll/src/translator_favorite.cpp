// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// =============================================================================
// Runtime translator for FAVORITE / FVP engine (Sakura.exe).
//
// Game: さくら、もゆ。 -as the Night's, Reincarnation- (FAVORITE 2019).
//
// STATUS: SKELETON. The CP932 -> wide / text-render hook target inside
// Sakura.exe has NOT yet been identified. Once Ghidra (run via the scripts
// under FAVORITE/analysys/) surfaces the right function, fill in:
//
//     HOOK_RVA          - the hook site offset (image base relative)
//     HOOK_PROLOGUE     - the first N bytes (>= 5) the hook will steal
//     HOOK_LEN          - sizeof(HOOK_PROLOGUE)
//     hook trampoline   - the inline asm in InstallTextHook (cdecl/thiscall
//                         differ, see amakano2pe's implementation for the
//                         shape of an x86 cdecl hook).
//
// Until that's done the DLL still loads, forwards every winmm.dll export to
// the real System32 winmm.dll, and reads the translation_table.tsv -- it
// just doesn't translate anything yet. This means dropping winmm.dll into
// the game folder is safe for testing the proxy/forwarding path.
//
// =============================================================================

#include "translator.h"
#include "translator_logic.h"
#include "engine_scout.h"
#include "log.h"

#include <windows.h>
#include <cstring>
#include <string>
#include <vector>
#include <cstdio>


static translator_logic::TranslationMap g_translations;
static bool g_translationsLoaded = false;

static int g_hookHits = 0;
static int g_translationHits = 0;
static int g_exactHits = 0;
static int g_normHits = 0;
static int g_missHits = 0;


// -- TSV loader (wraps the pure parser with Win32 file I/O) -------------------
//
// translation_table.tsv is UTF-8 (Python writes it). The FVP engine hands
// the runtime hook CP932 (Shift-JIS) bytes. To make `unordered_map::find`
// hit at all, the keys must be in the same encoding as the lookup string
// — so we transcode every JP key UTF-8 -> CP932 right after parsing the
// TSV. EN values stay ASCII (the translator prompt enforces ASCII-only)
// so they need no conversion.

static std::string Utf8ToCp932(const std::string& utf8)
{
    if (utf8.empty()) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(),
                                   nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(),
                        w.data(), wlen);
    int alen = WideCharToMultiByte(932, 0, w.data(), wlen,
                                   nullptr, 0, nullptr, nullptr);
    if (alen <= 0) return {};
    std::string out(alen, '\0');
    WideCharToMultiByte(932, 0, w.data(), wlen,
                        out.data(), alen, nullptr, nullptr);
    return out;
}

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

    translator_logic::TranslationMap utf8Map;
    int count = translator_logic::ParseTsvBuffer(buf.data(), n, utf8Map);

    // Re-key UTF-8 -> CP932 so runtime CP932 lookups hit.
    g_translations.clear();
    g_translations.reserve(utf8Map.size() * 2);
    int dropped = 0;
    for (const auto& kv : utf8Map) {
        std::string cp932 = Utf8ToCp932(kv.first);
        if (cp932.empty()) { dropped++; continue; }
        g_translations.emplace(std::move(cp932), kv.second);
    }
    Log("Translator: loaded %d translations from translation_table.tsv "
        "(re-keyed UTF-8->CP932, %d dropped, %zu unique CP932 keys)",
        count, dropped, g_translations.size());

    // Sample a handful of CP932 keys so the log makes it obvious we re-keyed
    // correctly. Log() transcodes CP932 -> UTF-8 so the JP shows up readable.
    int sampled = 0;
    for (const auto& kv : g_translations) {
        Log("Translator: sample [%s] -> [%s]",
            kv.first.c_str(), kv.second.c_str());
        if (++sampled >= 5) break;
    }

    g_translationsLoaded = (!g_translations.empty());
    return g_translationsLoaded;
}


// -- String pool for translated output ----------------------------------------

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


// Once the hook target is identified, this is the function the trampoline
// calls. It receives a CP932 char* on input, returns a CP932 char* (the
// English replacement, or the original on miss). English ASCII is a valid
// CP932 byte sequence so the engine's downstream conversion just works.
extern "C" const char* __cdecl FavoriteTranslateText(const char* src);

static const char* TranslateTextInner(const char* src)
{
    if (!g_translationsLoaded || !src) return src;
    if (!IsReadablePtr(src)) return src;
    if (!src[0]) return src;

    g_hookHits++;

    const size_t srcLen = strlen(src);
    const std::string key(src, srcLen);
    const std::string* en = translator_logic::FindTranslation(g_translations, key);
    bool wasNorm = false;
    std::string norm;
    if (!en) {
        norm = translator_logic::NormalizeCp932LookupKey(src, srcLen);
        if (!norm.empty() && norm.size() != srcLen) {
            en = translator_logic::FindTranslation(g_translations, norm);
            wasNorm = (en != nullptr);
        }
    }

    if (!en) {
        g_missHits++;
        // Log every miss for the first 100, then every 50th. This is the
        // diagnostic we need: any string the engine queries that isn't in
        // the TSV shows up here so we know exactly what to translate next.
        if (g_missHits <= 100 || (g_missHits % 50) == 0) {
            if (norm.empty() || norm.size() == srcLen) {
                Log("MISS #%d hook=#%d: [%.80s]",
                    g_missHits, g_hookHits, src);
            } else {
                Log("MISS #%d hook=#%d: [%.80s] (norm tried: [%.80s])",
                    g_missHits, g_hookHits, src, norm.c_str());
            }
        }
        return src;
    }

    if (wasNorm) g_normHits++;
    else         g_exactHits++;

    g_translationHits++;
    const std::string wrapped = translator_logic::WrapAsciiToColumnGrid(*en, 58);
    char* slot = AllocTransSlot();
    size_t copyLen = wrapped.size();
    if (copyLen >= TRANS_SLOT_SIZE) copyLen = TRANS_SLOT_SIZE - 1;
    memcpy(slot, wrapped.c_str(), copyLen);
    slot[copyLen] = '\0';

    if (g_translationHits <= 50 || (g_translationHits % 100) == 0) {
        Log("HIT  #%d hook=#%d %s: [%.60s] -> [%.60s]",
            g_translationHits, g_hookHits,
            wasNorm ? "(norm)" : "(exact)", src, slot);
    }
    return slot;
}

extern "C" const char* __cdecl FavoriteTranslateText(const char* src)
{
    __try {
        return TranslateTextInner(src);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Translator: EXCEPTION in FavoriteTranslateText, ptr=0x%p hit=%d",
            src, g_hookHits);
        return src;
    }
}


// -- Hook installation --------------------------------------------------------
//
// Hook target: FUN_00433610 inside Sakura.exe — the per-string CP932
// byte-iterator inside the FVP TextPrint render path. Identified via:
//
//   1. ScoutEngine() found Sakura.log-backed mappings with RTTI for
//      TextFont / FontRasterizer / TextureRenderer at runtime.
//   2. Full-image dump (sakura_runtime.bin, 604 KB) loaded into Ghidra.
//   3. find_textrender_path.java located the FVP script-command
//      registration table at FUN_0043F8D0, which registers TextPrint
//      with handler 0x0043CA90 (extracted by parsing `mov ecx, imm32 ;
//      push offset s_TextPrint` patterns in the dump).
//   4. Decompiling 0x0043CA90 showed it dispatches into FUN_00433610
//      with the string pointer pulled from the global string table.
//   5. FUN_00433610 is the canonical CP932 byte-walker — uses
//      __ismbblead per byte to iterate Shift-JIS lead/trail pairs and
//      queues each char for the renderer. ASCII Latin (our EN
//      translation) is valid CP932 (single-byte), so swapping the
//      buffer pointer at entry is all we need.
//
// Calling convention: __thiscall.
//   ECX     = `this` (TextFont/render context)
//   [esp+4] = char* (the JP CP932 string we want to substitute)
//
// Prologue (5 bytes — exactly enough for a JMP rel32, no NOPs needed):
//   83 EC 0C    sub esp, 0Ch
//   55          push ebp
//   56          push esi

static const DWORD HOOK_RVA = 0x00033610;  // FUN_00433610 — CP932 render iterator
static const size_t HOOK_LEN = 5;          // stolen prologue size
static const BYTE HOOK_PROLOGUE[HOOK_LEN] = {
    0x83, 0xEC, 0x0C,  // sub esp, 0Ch
    0x55,              // push ebp
    0x56,              // push esi
};

static BYTE* g_hookSite = nullptr;
static BYTE* g_trampoline = nullptr;

static bool InstallTextHook(BYTE* base)
{
    g_hookSite = base + HOOK_RVA;
    BYTE* returnAddr = g_hookSite + HOOK_LEN;

    if (memcmp(g_hookSite, HOOK_PROLOGUE, HOOK_LEN) != 0) {
        Log("TEXT HOOK: bytes mismatch at RVA 0x%X", HOOK_RVA);
        Log("  Expected: %02X %02X %02X %02X %02X",
            HOOK_PROLOGUE[0], HOOK_PROLOGUE[1], HOOK_PROLOGUE[2],
            HOOK_PROLOGUE[3], HOOK_PROLOGUE[4]);
        Log("  Found:    %02X %02X %02X %02X %02X",
            g_hookSite[0], g_hookSite[1], g_hookSite[2],
            g_hookSite[3], g_hookSite[4]);
        return false;
    }

    g_trampoline = (BYTE*)VirtualAlloc(nullptr, 256,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampoline) {
        Log("TEXT HOOK: VirtualAlloc failed");
        return false;
    }

    BYTE* p = g_trampoline;

    // __thiscall: ECX = this. Stack at entry to FUN_00433610:
    //   [esp+0x00] = ret addr
    //   [esp+0x04] = char* (the JP string)  <- we patch this
    //
    // After pushad (32 bytes), the stack arg is at [esp+0x20+0x04] = [esp+0x24].
    // pushad/popad preserves ECX (this) automatically.

    *p++ = 0x60;                             // pushad

    *p++ = 0x8B; *p++ = 0x44; *p++ = 0x24;   // mov eax, [esp+0x24]
    *p++ = 0x24;
    *p++ = 0x50;                             // push eax
    *p++ = 0xE8;                             // call FavoriteTranslateText
    *(DWORD*)p = (DWORD)(BYTE*)&FavoriteTranslateText - (DWORD)(p + 4);
    p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;   // add esp, 4
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24;   // mov [esp+0x24], eax
    *p++ = 0x24;

    *p++ = 0x61;                             // popad

    // Execute stolen prologue (5 bytes)
    memcpy(p, HOOK_PROLOGUE, HOOK_LEN);
    p += HOOK_LEN;

    // jmp back to hookSite + HOOK_LEN
    *p++ = 0xE9;
    *(DWORD*)p = (DWORD)returnAddr - (DWORD)(p + 4);
    p += 4;

    DWORD oldProtect;
    if (!VirtualProtect(g_hookSite, HOOK_LEN, PAGE_EXECUTE_READWRITE,
                        &oldProtect)) {
        Log("TEXT HOOK: VirtualProtect failed");
        return false;
    }

    // 5-byte JMP rel32, exactly fills the stolen prologue (no NOP padding).
    g_hookSite[0] = 0xE9;
    *(DWORD*)(g_hookSite + 1) = (DWORD)g_trampoline - (DWORD)(g_hookSite + 5);

    DWORD dummy;
    VirtualProtect(g_hookSite, HOOK_LEN, oldProtect, &dummy);
    FlushInstructionCache(GetCurrentProcess(), g_hookSite, HOOK_LEN);

    Log("TEXT HOOK: installed at 0x%08X (RVA 0x%X), trampoline=0x%08X",
        (DWORD)g_hookSite, HOOK_RVA, (DWORD)g_trampoline);
    return true;
}


// Worker thread: the engine is decrypted from Sakura.log into Sakura.exe's
// image AT RUNTIME — when our winmm.dll DllMain runs, the bytes at our
// hook RVA may not yet be in place. So we poll the prologue from a
// background thread and install the inline hook only once the expected
// bytes appear. Same pattern works for the diagnostic scout.
static DWORD WINAPI ScoutThread(LPVOID)
{
    Log("Scout: worker thread started, waiting 1s for engine unpack...");
    Sleep(1000);

    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    BYTE* hookSite = base + HOOK_RVA;

    // Poll for prologue match (engine may decrypt slightly later than DllMain).
    bool installed = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (memcmp(hookSite, HOOK_PROLOGUE, HOOK_LEN) == 0) {
            installed = InstallTextHook(base);
            break;
        }
        Sleep(200);
    }
    if (!installed) {
        Log("Scout: hook prologue never appeared at RVA 0x%X after 6s — "
            "logging current bytes for diagnosis:", HOOK_RVA);
        Log("  bytes: %02X %02X %02X %02X %02X",
            hookSite[0], hookSite[1], hookSite[2], hookSite[3], hookSite[4]);
    }

    // Run scout AFTER hook attempt so the diagnostic dump reflects the
    // post-hook state too.
    ScoutEngine();
    return 0;
}

void TranslatorInit()
{
    LoadTranslations();
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    Log("Translator: Sakura.exe base=0x%08X", (DWORD)base);

    // Hook installation deferred to ScoutThread because the engine code
    // is decrypted asynchronously and won't be present at DllMain time.
    HANDLE hScout = CreateThread(nullptr, 0, ScoutThread, nullptr, 0, nullptr);
    if (hScout) CloseHandle(hScout);
}

void TranslatorShutdown()
{
    Log("Translator: %d hook hits, %d translations applied "
        "(exact=%d, norm=%d, miss=%d)",
        g_hookHits, g_translationHits,
        g_exactHits, g_normHits, g_missHits);
}
