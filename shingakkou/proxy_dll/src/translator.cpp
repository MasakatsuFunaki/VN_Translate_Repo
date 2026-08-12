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
#include <algorithm>

// ==========================================================================
// Runtime translator for shingakkou (DDSystem/PIL engine).
//
// Strategy: Hook FUN_0040e810 (decrypt_hxb, RVA 0xe810) which decrypts
// script data loaded from the DDP3 archive.  After decryption, we scan
// the buffer for text strings (FF 01 80 markers) and replace Japanese
// text with English translations — identical to what an on-disk repack does
// offline, but performed in memory at runtime.
//
// The engine's bytecode interpreter then processes the modified data
// naturally, rendering English text character by character.
//
// decrypt_hxb signature (__fastcall + EAX):
//   LPVOID __fastcall decrypt_hxb(char* data_ECX)   // EAX = data_size
// Returns a heap-allocated structure:
//   [+0x04] BYTE* data_ptr  -> points to struct+0x406E0
//   [+0x08] DWORD data_size
//   [+0x0C] DWORD data_start_offset
//   [+0x406E0..]  actual data (16-byte HXB header + decrypted content)
//
// Hook: steal first 7 bytes of the function prologue:
//   55          push ebp
//   8B EC       mov ebp, esp
//   51          push ecx
//   53          push ebx
//   56          push esi
//   57          push edi
// ==========================================================================

// Translation table: wchar_t JP string -> wchar_t EN string
static std::unordered_map<std::wstring, std::wstring> g_translations;
static bool g_translationsLoaded = false;

// Message-window line spacing written over the system scripts' var 0x2D1
// assignment (originally 30). On-screen blit step only: keeps 4 wrapped
// EN lines inside the 90px window. The 4th line itself is recovered by
// the surface-height patch below -- spacing alone can never reveal it.
// See translator_logic::PatchMessageLineSpacing for the two-stage render.
static const BYTE kMessageLineSpacing = 20;

// Row capacity of the offscreen message text surface (layer 130),
// originally 4 rows x 26px = 104px -- which clips wrapped EN lines 4+ at
// render time before they ever reach the screen. 7 rows = 182px covers
// 6 lines; the longest translation wraps to 5.
// See translator_logic::PatchTextSurfaceHeight.
static const BYTE kTextSurfaceLineCapacity = 7;

// Message-window wrap budget in pixels (the engine's [0x45E734] global at
// message-wrap time; bytecode passes 546-10). With the proportionalizer's
// 8px halfwidth advance that is 67 ASCII chars per line. Used to pre-wrap
// English at word boundaries -- see translator_logic::WordWrapMessage.
static const int kMessageWrapBudgetPx = 536;

// Top of the per-message window-background stamp (the only opaque eraser
// of the previous message's text), relative to the window origin
// var[0x2D3]. Line 1's band lands at +spacing + var[0x2D7](=9); the stock
// stamp top (+36) matched the stock spacing 30. After the spacing patch
// above, line 1 is at +29 -- 7px above the stamp -- so old glyph tops
// accumulated there (ghost band at the top of the first line). Keep this
// derived from kMessageLineSpacing so the two can't drift apart again.
// See translator_logic::PatchMessageWindowRefreshTop.
static const BYTE kMessageRefreshTop = kMessageLineSpacing + 9;

// Hook infrastructure
static BYTE* g_trampoline = nullptr;
static BYTE* g_hookSite = nullptr;
static DWORD g_savedRetAddr = 0;
static HANDLE g_translatorThread = nullptr;
static volatile bool g_shutdownRequested = false;

// Game heap handle (global at RVA 0x91A10 in EXE)
static HANDLE g_gameHeap = nullptr;
static BYTE*  g_exeBase = nullptr;

// Stats
static int g_decryptCalls = 0;
static int g_scriptsPatched = 0;
static int g_stringsReplaced = 0;

// Patched-buffer registry (diagnostic + jump relocation). Lets the crash
// handler map a runtime pointer back to a script offset, and lets
// RelocateAfterJump find the cumulative-delta map for the buffer a jump is
// executing in. Ring of the most-recent buffers.
//
// Sized at 32 (was 8): RelocateAfterJump SILENTLY skips relocation when the
// executing buffer's base is no longer in this ring (it can't tell an
// evicted-but-resident patched buffer from a genuinely unpatched script).
// On a long / skip-heavy session more than 8 distinct scripts get patched,
// so a resident patched buffer could fall out of an 8-ring and its jumps
// would stop being relocated -> stale absolute targets -> the cursor lands
// mid-string -> a wrong dword is read as a buffer number -> the engine's own
// "範囲外バッファアクセス" bounds dialog. 32 comfortably exceeds the number of
// patched buffers resident at once; the reporter hook below logs whether a
// faulting base is still tracked, so if eviction ever does bite we will see
// it in the log rather than guessing.
static const int kMaxPatchedBufs = 32;   // must stay a power of two
static BYTE* g_patchedBase[kMaxPatchedBufs] = {nullptr};
static DWORD g_patchedSize[kMaxPatchedBufs] = {0};
static int   g_patchedRing    = 0;

// Per-buffer cumulative-delta map (same vector passed to ApplyCumulativeDelta
// for the tail/choice fixups). The runtime jump-relocation hooks use it to
// shift a stale absolute jump target into its post-replacement position.
static std::vector<translator_logic::CumulativeDelta> g_patchedCum[kMaxPatchedBufs];

static int BufIndexOf(DWORD ptr, DWORD* outOff)
{
    for (int i = 0; i < kMaxPatchedBufs; i++) {
        const DWORD base = (DWORD)g_patchedBase[i];
        if (base && ptr >= base && ptr < base + g_patchedSize[i]) {
            if (outOff) *outOff = ptr - base;
            return i;
        }
    }
    return -1;
}

// Drop any ring slot whose base == `base`. The engine frees a patched buffer
// when its script unloads; the heap then hands the SAME address back -- either
// as the source buffer of a later decrypt (a different script, possibly one we
// don't patch at all), or as the destination of a fresh HeapAlloc here. Two
// LIVE allocations can never share an address (and base = struct + 0x406E0 is
// injective in the struct pointer), so a registered slot matching `base`
// necessarily refers to a now-dead buffer. Clearing it stops the first-match
// linear scans in BufIndexOf and RelocateAfterJump from applying that slot's
// stale cumulative-delta map to the new script executing at the reused address
// -- the unified cause of the heap-reuse crashes (e.g. Bootup.dat reusing a
// freed scene script's block, or one patched buffer landing on a freed one).
static void InvalidatePatchedSlot(BYTE* base, const char* why)
{
    if (!base) return;
    for (int i = 0; i < kMaxPatchedBufs; i++) {
        if (g_patchedBase[i] == base) {
            Log("Translator: invalidating stale ring slot #%d base=0x%08X (%s)",
                i, (DWORD)base, why);
            g_patchedBase[i] = nullptr;
            g_patchedSize[i] = 0;
            g_patchedCum[i].clear();
        }
    }
}

// Safe hex dump of up to 32 bytes at an arbitrary address. Uses no CRT
// format helpers so it stays allocation- and reentrancy-light for the
// crash path; unreadable memory is reported, not faulted on.
static void LogBytesAt(const char* label, DWORD ptr, int n)
{
    if (n > 32) n = 32;
    unsigned char b[32];
    bool ok = true;
    __try { memcpy(b, (const void*)ptr, (size_t)n); }
    __except(EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (!ok) { Log("  %s @0x%08X: <unreadable>", label, ptr); return; }
    char hex[32 * 3 + 1];
    const char* H = "0123456789ABCDEF";
    int p = 0;
    for (int i = 0; i < n; i++) {
        hex[p++] = H[b[i] >> 4];
        hex[p++] = H[b[i] & 0xF];
        hex[p++] = ' ';
    }
    hex[p] = '\0';
    Log("  %s @0x%08X: %s", label, ptr, hex);
}

// Safe single-line dump of up to `maxChars` UTF-16LE chars at `ptr` as
// printable ASCII (newlines -> '|', other non-ASCII -> '.'). Used to show
// which engine error template fired and what bytecode the cursor is sitting
// on. Reentrancy-light for the reporter-hook path; faults are swallowed.
static void LogWideAt(const char* label, DWORD ptr, int maxChars)
{
    char out[160];
    int n = 0;
    __try {
        const wchar_t* w = (const wchar_t*)ptr;
        for (int i = 0; i < maxChars && n < (int)sizeof(out) - 1; i++) {
            const wchar_t c = w[i];
            if (c == 0) break;
            out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c
                     : (c == L'\n')            ? '|'
                                               : '.';
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { /* truncate at fault */ }
    out[n] = '\0';
    Log("  %s: \"%s\"", label, out);
}

// ── VEH crash handler ──────────────────────────────────────────────────

static const char* ExcName(DWORD code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_INT_OVERFLOW:          return "INT_OVERFLOW";
        default:                               return "OTHER";
    }
}

// First-chance handler. We log every *fatal-severity* exception (top two
// status bits set) except the MSVC C++ EH code 0xE06D7363, which the CRT
// raises as normal control flow. A bad bytecode jump (the suspected choice
// crash) surfaces as ILLEGAL_INSTRUCTION / PRIV_INSTRUCTION or a wild
// ACCESS_VIOLATION -- the old handler logged only AV/stack-overflow, so it
// stayed silent on exactly the case we need. Behavior is unchanged: we only
// log, then CONTINUE_SEARCH as before.
static LONG WINAPI TranslatorVEH(EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const bool fatal = (code & 0xC0000000u) == 0xC0000000u &&
                       code != 0xE06D7363u;   // MSVC C++ EH
    if (!fatal)
        return EXCEPTION_CONTINUE_SEARCH;

    // A genuine crash often cascades (the engine's own handler retries and
    // re-faults), flooding the log with thousands of identical records. The
    // FIRST fatal exception is the root cause; cap detailed logging so the
    // root stays readable.
    static volatile LONG s_fatalCount = 0;
    if (InterlockedIncrement(&s_fatalCount) > 3)
        return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT* c = ep->ContextRecord;
    const DWORD eip = c->Eip;

    DWORD imgSize = 0;
    if (g_exeBase) {
        __try {
            IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_exeBase;
            IMAGE_NT_HEADERS* nt =
                (IMAGE_NT_HEADERS*)(g_exeBase + dos->e_lfanew);
            imgSize = nt->OptionalHeader.SizeOfImage;
        } __except(EXCEPTION_EXECUTE_HANDLER) { imgSize = 0; }
    }
    const bool inExe = g_exeBase && imgSize &&
                       eip >= (DWORD)g_exeBase &&
                       eip <  (DWORD)g_exeBase + imgSize;
    const bool inTramp = g_trampoline &&
                         eip >= (DWORD)g_trampoline &&
                         eip <  (DWORD)g_trampoline + 512;

    Log("=== FATAL EXCEPTION ===");
    Log("  Code: 0x%08X (%s)", code, ExcName(code));
    Log("  EIP: 0x%08X  ESP: 0x%08X  EBP: 0x%08X", eip, c->Esp, c->Ebp);
    Log("  EAX:%08X ECX:%08X EDX:%08X EBX:%08X ESI:%08X EDI:%08X",
        c->Eax, c->Ecx, c->Edx, c->Ebx, c->Esi, c->Edi);
    Log("  ExeBase:0x%08X ImgSize:0x%X  Trampoline:0x%08X",
        (DWORD)g_exeBase, imgSize, (DWORD)g_trampoline);
    Log("  EIP region: %s%s%s  RVA:0x%X",
        inExe   ? "IN-EXE" : "",
        inTramp ? "IN-TRAMPOLINE" : "",
        (!inExe && !inTramp) ? "OUTSIDE-EXE (heap/data?)" : "",
        inExe ? (eip - (DWORD)g_exeBase) : 0);

    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        const DWORD acc  = (DWORD)ep->ExceptionRecord->ExceptionInformation[0];
        const DWORD addr = (DWORD)ep->ExceptionRecord->ExceptionInformation[1];
        Log("  AV %s at 0x%08X",
            acc == 0 ? "READ" : (acc == 1 ? "WRITE" : (acc == 8 ? "EXEC" : "?")),
            addr);
    }

    unsigned char ib[16];
    bool ok = true;
    __try { memcpy(ib, (const void*)eip, sizeof(ib)); }
    __except(EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (ok)
        Log("  bytes@EIP: %02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X",
            ib[0], ib[1], ib[2], ib[3], ib[4], ib[5], ib[6], ib[7],
            ib[8], ib[9], ib[10], ib[11], ib[12], ib[13], ib[14], ib[15]);
    else
        Log("  bytes@EIP: <unreadable>");

    // ── Map registers + stack to the patched script buffer ──
    // The crash at RVA 0xD968 is inside the engine's bytecode expression
    // VM, which reads its cursor from [ESP+0x1C]. Whichever register or
    // stack slot lands inside a patched buffer is a pointer into the
    // relocated script; its offset (and the bytes there) tell us whether
    // the VM was reading a valid expression (operand-load relocation bug)
    // or garbage (stale branch/seek target).
    {
        const struct { const char* n; DWORD v; } regs[] = {
            {"EAX", c->Eax}, {"ECX", c->Ecx}, {"EDX", c->Edx},
            {"EBX", c->Ebx}, {"ESI", c->Esi}, {"EDI", c->Edi},
            {"EBP", c->Ebp},
        };
        for (int i = 0; i < 7; i++) {
            DWORD off = 0;
            const int idx = BufIndexOf(regs[i].v, &off);
            if (idx >= 0) {
                Log("  %s=0x%08X INSIDE patched buf #%d @ script offset 0x%X",
                    regs[i].n, regs[i].v, idx, off);
                LogBytesAt(regs[i].n, regs[i].v, 32);
            }
        }
        for (int i = 0; i <= 0x40; i += 4) {
            DWORD slot = 0;
            bool ok = true;
            __try { slot = *(DWORD*)(c->Esp + i); }
            __except(EXCEPTION_EXECUTE_HANDLER) { ok = false; }
            if (!ok) continue;
            DWORD off = 0;
            const int idx = BufIndexOf(slot, &off);
            const bool cursor = (i == 0x1C);
            if (idx >= 0 || cursor) {
                Log("  [ESP+0x%02X]=0x%08X%s%s", i, slot,
                    cursor ? " (VM cursor [ESP+0x1C])" : "",
                    idx >= 0 ? " INSIDE patched buf" : "");
                if (idx >= 0)
                    Log("        -> buf #%d script offset 0x%X", idx, off);
                if (cursor || idx >= 0)
                    LogBytesAt("    bytes", slot, 32);
            }
        }
    }

    // ── Engine script-execution state at [0x491A0C] = {position, base} ──
    // The interpreter computes its cursor as base + position; a NULL cursor
    // means this state is invalid (base/position cleared). Dump it to see
    // which is zero and whether base still points at our patched buffer.
    if (g_exeBase) {
        DWORD stateP = 0, pos = 0, base = 0;
        bool sok = true;
        __try {
            stateP = *(DWORD*)(g_exeBase + 0x91A0C);
            if (stateP) { pos = *(DWORD*)(stateP + 0); base = *(DWORD*)(stateP + 4); }
        } __except(EXCEPTION_EXECUTE_HANDLER) { sok = false; }
        if (sok) {
            Log("  ScriptState[0x491A0C]=0x%08X position=0x%X base=0x%08X "
                "(cursor=base+pos=0x%08X)", stateP, pos, base, base + pos);
            DWORD bo;
            const int bidx = BufIndexOf(base, &bo);
            if (bidx >= 0) Log("        base = patched buf #%d + 0x%X", bidx, bo);
        } else {
            Log("  ScriptState[0x491A0C]: <unreadable>");
        }
    }

    // ── Caller chain (walk EBP frames) ──
    // Finds who invoked the faulting routine so we can map the trigger
    // opcode back to a bytecode-interpreter entry point.
    {
        DWORD ebp = c->Ebp;
        for (int f = 0; f < 16 && ebp && (ebp & 3) == 0; f++) {
            DWORD nextebp = 0, ret = 0;
            bool fok = true;
            __try { nextebp = *(DWORD*)ebp; ret = *(DWORD*)(ebp + 4); }
            __except(EXCEPTION_EXECUTE_HANDLER) { fok = false; }
            if (!fok) break;
            if (g_exeBase && imgSize &&
                ret >= (DWORD)g_exeBase && ret < (DWORD)g_exeBase + imgSize)
                Log("  caller[%d] ret=0x%08X RVA 0x%X", f, ret,
                    ret - (DWORD)g_exeBase);
            else
                Log("  caller[%d] ret=0x%08X (outside exe)", f, ret);
            if (nextebp <= ebp) break;   // EBP chain must climb the stack
            ebp = nextebp;
        }
    }

    Log("  Stats: decryptCalls=%d patched=%d replaced=%d",
        g_decryptCalls, g_scriptsPatched, g_stringsReplaced);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ── TSV loader (UTF-8 file -> wchar_t map) ─────────────────────────────

static std::wstring Utf8ToWide(const char* utf8, int len = -1)
{
    if (!utf8 || (len == 0)) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8, len, nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring result(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, len, &result[0], needed);
    if (len == -1 && !result.empty() && result.back() == L'\0')
        result.pop_back();
    return result;
}

static bool LoadTranslations()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash)
        wcscpy(lastSlash + 1, L"translation_table.tsv");
    else
        wcscpy(exePath, L"translation_table.tsv");

    FILE* f = _wfopen(exePath, L"rb");
    if (!f) {
        Log("Translator: translation_table.tsv not found");
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

    // Skip UTF-8 BOM, parse into a narrow UTF-8 map, then promote
    // each entry to wide so the script buffer's UTF-16LE text can be
    // looked up against it. All parsing (BOM, escape tokens, line
    // splitting) is in translator_logic so it is covered by gtest.
    const std::size_t bomLen = translator_logic::Utf8BomLen(buf.data(), nread);
    translator_logic::Utf8TranslationMap utf8Map;
    translator_logic::ParseTsvBuffer(
        buf.data() + bomLen,
        nread - bomLen,
        utf8Map);

    int count = 0;
    for (const auto& kv : utf8Map) {
        std::wstring jpWide = Utf8ToWide(kv.first.c_str());
        std::wstring enWide = Utf8ToWide(kv.second.c_str());
        if (!jpWide.empty() && !enWide.empty()) {
            g_translations[std::move(jpWide)] = std::move(enWide);
            count++;
        }
    }

    Log("Translator: loaded %d translations (%d unique)",
        count, (int)g_translations.size());
    g_translationsLoaded = true;
    return count > 0;
}

// ── Post-decrypt data patching ─────────────────────────────────────────

struct StringReplacement {
    DWORD marker_off;   // offset of FF 01 80 in data
    DWORD str_start;    // offset of first UTF-16LE byte (marker_off + 3)
    DWORD str_end;      // offset of the 00 00 null terminator
    std::vector<BYTE> new_bytes;
    int delta;          // new_bytes.size() - (str_end - str_start)
};

// Called from the trampoline after decrypt_hxb returns.
// EAX = returned structure pointer (or NULL).
static LPVOID __cdecl PatchDecryptedData(LPVOID structPtr)
{
    __try {
        return [](LPVOID structPtr) -> LPVOID {

    if (!structPtr || !g_translationsLoaded) return structPtr;

    // Lazy-init game heap on first call (may not be ready at DLL load time)
    if (!g_gameHeap && g_exeBase) {
        g_gameHeap = *(HANDLE*)(g_exeBase + 0x91A10);
        if (!g_gameHeap) g_gameHeap = GetProcessHeap();
        Log("Translator: game heap = 0x%08X (lazy init)", (DWORD)g_gameHeap);
    }
    if (!g_gameHeap) {
        g_gameHeap = GetProcessHeap();
    }

    g_decryptCalls++;

    BYTE* s = (BYTE*)structPtr;
    BYTE* data = *(BYTE**)(s + 4);
    DWORD dataSize = *(DWORD*)(s + 8);

    if (dataSize < 0x20 || !data) return structPtr;

    // ── Invalidate a stale ring slot reusing this (incoming) buffer ──
    // This decrypt may land on a block the heap reclaimed from a previously
    // patched-then-freed script. Crucially this runs BEFORE the no-match early
    // return below: a reuser with zero translatable strings (e.g. a Bootup.dat
    // system script) never reaches registration, so without this its dead
    // predecessor's slot would survive and mis-relocate the reuser's own jumps.
    InvalidatePatchedSlot(data, "decrypt reused a freed patched buffer");

    // ── System-script line-spacing patch (in place, same length) ──
    // Hits only the Bootup.dat system scripts; game scripts return 0.
    // Runs before the replacement copy below so a patched byte would be
    // carried into the new buffer if a script ever had both.
    {
        int n = translator_logic::PatchMessageLineSpacing(
            data, dataSize, kMessageLineSpacing);
        if (n > 0)
            Log("Translator: script #%d: line spacing 30->%d (%d assignment%s)",
                g_decryptCalls, kMessageLineSpacing, n, n == 1 ? "" : "s");

        n = translator_logic::PatchTextSurfaceHeight(
            data, dataSize, kTextSurfaceLineCapacity);
        if (n > 0)
            Log("Translator: script #%d: text surface 4->%d rows (%d create%s)",
                g_decryptCalls, kTextSurfaceLineCapacity, n, n == 1 ? "" : "s");

        n = translator_logic::PatchMessageWindowRefreshTop(
            data, dataSize, kMessageRefreshTop);
        if (n > 0)
            Log("Translator: script #%d: window refresh top 36->%d (%d blit%s)",
                g_decryptCalls, kMessageRefreshTop, n, n == 1 ? "" : "s");
    }

    // ── Scan for FF 01 80 text markers ──
    std::vector<StringReplacement> replacements;

    for (DWORD i = 0x11; i + 4 < dataSize; i++) {
        if (data[i] != 0xFF || data[i + 1] != 0x01 || data[i + 2] != 0x80)
            continue;

        BYTE preByte = data[i - 1];
        // 0x36 = dialogue/narration, 0x07 = misc, 0x01 = choice
        if (preByte != 0x36 && preByte != 0x07 && preByte != 0x01)
            continue;

        DWORD strStart = i + 3;
        DWORD pos = strStart;
        while (pos + 1 < dataSize) {
            WORD ch;
            memcpy(&ch, data + pos, 2);
            if (ch == 0) break;
            pos += 2;
        }
        DWORD strEnd = pos;

        int numChars = (int)(strEnd - strStart) / 2;
        if (numChars <= 0) continue;

        std::wstring jp((const wchar_t*)(data + strStart), numChars);

        auto it = g_translations.find(jp);
        if (it == g_translations.end()) continue;

        std::wstring en = it->second;
        // Word-wrap dialogue/narration so the engine's char-granularity
        // wrap never slices a word mid-letter. Same-length swap (space ->
        // '\r'), so delta math below is unaffected. Choices/misc render
        // in other widgets and are left alone.
        if (preByte == 0x36)
            translator_logic::WordWrapMessage(en, kMessageWrapBudgetPx);
        std::vector<BYTE> newBytes(en.size() * 2);
        memcpy(newBytes.data(), en.c_str(), en.size() * 2);

        int delta = (int)newBytes.size() - (int)(strEnd - strStart);
        replacements.push_back({i, strStart, strEnd, std::move(newBytes), delta});
    }

    if (replacements.empty()) {
        if (g_decryptCalls <= 5)
            Log("Translator: script #%d: no matches (size=%u)", g_decryptCalls, dataSize);
        return structPtr;
    }

    // ── Calculate total delta ──
    int totalDelta = 0;
    for (auto& r : replacements) totalDelta += r.delta;

    DWORD newDataSize = (DWORD)((int)dataSize + totalDelta);

    // ── Allocate new structure ──
    LPVOID newStruct = HeapAlloc(g_gameHeap, HEAP_ZERO_MEMORY,
                                  (SIZE_T)newDataSize + 0x406E8);
    if (!newStruct) {
        Log("Translator: HeapAlloc failed (%u bytes)", newDataSize + 0x406E8);
        return structPtr;
    }

    BYTE* ns = (BYTE*)newStruct;
    BYTE* newData = ns + 0x406E0;

    // Copy metadata header
    memcpy(ns, s, 0x406E0);

    // Update structure pointers
    *(BYTE**)(ns + 4) = newData;
    *(DWORD*)(ns + 8) = newDataSize;

    // ── Copy data with string replacements ──
    DWORD srcPos = 0, dstPos = 0;

    for (auto& r : replacements) {
        DWORD copyLen = r.str_start - srcPos;
        memcpy(newData + dstPos, data + srcPos, copyLen);
        dstPos += copyLen;

        memcpy(newData + dstPos, r.new_bytes.data(), r.new_bytes.size());
        dstPos += (DWORD)r.new_bytes.size();

        srcPos = r.str_end;
    }

    // Copy remaining data (includes offset table at tail)
    DWORD remainLen = dataSize - srcPos;
    memcpy(newData + dstPos, data + srcPos, remainLen);

    // ── Build cumulative delta map ──
    std::vector<translator_logic::CumulativeDelta> cumulative;
    int cum = 0;
    for (auto& r : replacements) {
        DWORD afterNull = r.str_end + 2;
        cum += r.delta;
        cumulative.push_back({afterNull, cum});
    }

    // ── Fix offset table at script tail ──
    // A run of 3-byte big-endian entries, ascending, each pointing to an
    // 'FF 01 80' text-record marker, terminated by a trailing 0xFF.
    // FindOffsetTable locates the start structurally; the previous
    // "walk back while the high byte is 0x00" assumed every offset fit
    // in 16 bits and so fixed up ZERO entries on every script larger
    // than 64KB (whose deeper offsets need a non-zero third byte) --
    // silently breaking every save taken in the main story.
    const translator_logic::OffsetTable tbl =
        translator_logic::FindOffsetTable(data, dataSize);
    if (tbl.count > 0) {
        const DWORD origTableStart = (DWORD)tbl.start;
        const DWORD newTableStart = (DWORD)((int)origTableStart + totalDelta);

        for (int e = 0; e < tbl.count; e++) {
            DWORD origEpos = origTableStart + e * 3;
            DWORD newEpos = newTableStart + e * 3;

            if (newEpos + 3 > newDataSize) break;

            const std::uint32_t val =
                translator_logic::ReadBE24(data + origEpos);
            const int adj =
                translator_logic::ApplyCumulativeDelta(val, cumulative);
            const std::uint32_t newVal =
                static_cast<std::uint32_t>(static_cast<int>(val) + adj);

            translator_logic::WriteBE24(newData + newEpos, newVal);
        }
    }

    // ── Relocate choice-destination jump tables ──
    // Choice menus embed '2A 10 FF 00 <count> <count x BE24>' tables of
    // absolute jump offsets that the tail fixup above does not cover (they
    // live in the bytecode body). Left stale, selecting a choice seeks into
    // a shifted English string and the engine crashes interpreting it.
    const int relocatedChoices = translator_logic::RelocateChoiceTables(
        data, dataSize, newData, newDataSize, cumulative);

    // ── Fix table pointer in script header ──
    // The header holds a 3-byte big-endian pointer at offset 0x16 (after
    // the 0xFF marker at 0x15) equal to (table_start - 5); it must shift
    // with the table. This previously read/wrote a 2-byte BE value at
    // 0x17 guarded by header[0x16]==0x00 -- but 0x16 is the pointer's
    // HIGH byte, which is 0x01 on every script >64KB, so the guard
    // failed exactly where the fix was needed. Reading the full 24-bit
    // pointer collapses to the old value on small scripts (high byte
    // 0x00), so they are unaffected.
    if (newDataSize > 0x29 && newData[0x10 + 0x05] == 0xFF) {
        const DWORD ptrOff = 0x10 + 0x06;   // 0x16
        const std::uint32_t oldPtr =
            translator_logic::ReadBE24(newData + ptrOff);
        const std::uint32_t newPtr =
            static_cast<std::uint32_t>(static_cast<int>(oldPtr) + totalDelta);
        translator_logic::WriteBE24(newData + ptrOff, newPtr);
    }

    // ── Update HXB header length (bytes 8-10 big-endian) ──
    translator_logic::WriteBE24(newData + 8, newDataSize);

    // ── Free old structure ──
    HeapFree(g_gameHeap, 0, structPtr);

    int replaced = (int)replacements.size();
    g_scriptsPatched++;
    g_stringsReplaced += replaced;

    Log("Translator: script #%d: replaced %d strings (delta=%+d, %u->%u bytes), "
        "relocated %d choice-table entries",
        g_decryptCalls, replaced, totalDelta, dataSize, newDataSize,
        relocatedChoices);

    // Register the relocated buffer so the crash handler can map a
    // faulting pointer back to a script offset. First drop any stale slot at
    // this address: our HeapAlloc above can reuse a freed earlier newStruct,
    // so newData (= newStruct + 0x406E0) may collide with a registered base
    // whose buffer is already dead (the patched-on-patched reuse case).
    InvalidatePatchedSlot(newData, "new patched buffer reused a freed base");
    g_patchedBase[g_patchedRing & (kMaxPatchedBufs - 1)] = newData;
    g_patchedSize[g_patchedRing & (kMaxPatchedBufs - 1)] = newDataSize;
    g_patchedCum[g_patchedRing & (kMaxPatchedBufs - 1)]  = cumulative;
    g_patchedRing++;
    Log("Translator: patched buf base=0x%08X size=%u (struct=0x%08X)",
        (DWORD)newData, newDataSize, (DWORD)newStruct);

    return newStruct;

        }(structPtr);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("Translator: EXCEPTION in PatchDecryptedData (call #%d)", g_decryptCalls);
        return structPtr;
    }
}

// ── Hook installation ──────────────────────────────────────────────────

static bool InstallDecryptHook(BYTE* base)
{
    g_exeBase = base;

    // Try reading game heap now; if not ready yet, PatchDecryptedData will retry
    g_gameHeap = *(HANDLE*)(base + 0x91A10);
    if (g_gameHeap) {
        Log("Translator: game heap = 0x%08X", (DWORD)g_gameHeap);
    } else {
        Log("Translator: game heap not yet initialized (will retry lazily)");
    }

    const DWORD hookRVA = 0xe810;
    const size_t hookLen = 7;
    g_hookSite = base + hookRVA;
    BYTE* returnAddr = g_hookSite + hookLen;

    const BYTE expected[] = {
        0x55,             // push ebp
        0x8B, 0xEC,       // mov ebp, esp
        0x51,             // push ecx
        0x53,             // push ebx
        0x56,             // push esi
        0x57              // push edi
    };

    if (!patcher_logic::BytesMatch(g_hookSite, expected, hookLen)) {
        Log("DECRYPT HOOK: bytes mismatch at RVA 0x%X", hookRVA);
        Log("  Expected: %02X %02X %02X %02X %02X %02X %02X",
            expected[0], expected[1], expected[2], expected[3],
            expected[4], expected[5], expected[6]);
        Log("  Found:    %02X %02X %02X %02X %02X %02X %02X",
            g_hookSite[0], g_hookSite[1], g_hookSite[2],
            g_hookSite[3], g_hookSite[4], g_hookSite[5], g_hookSite[6]);
        return false;
    }

    g_trampoline = (BYTE*)VirtualAlloc(nullptr, 512, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
    if (!g_trampoline) {
        Log("DECRYPT HOOK: VirtualAlloc failed");
        return false;
    }

    BYTE* p = g_trampoline;

    // ── ENTRY HOOK ──
    // On entry: ECX = data_ptr, EAX = data_size, [ESP] = return_addr

    // pop edx  (original return addr -> EDX)
    *p++ = 0x5A;
    // mov [g_savedRetAddr], edx
    *p++ = 0x89; *p++ = 0x15;
    *(DWORD*)p = (DWORD)&g_savedRetAddr; p += 4;

    // push <post_process_addr>  (placeholder, patched below)
    *p++ = 0x68;
    DWORD* postProcessAddr = (DWORD*)p; p += 4;

    // Execute stolen bytes
    p = patcher_logic::EmitBytes(p, expected, hookLen);

    // JMP to original+7
    p = patcher_logic::EmitRelJmp32(
            p, reinterpret_cast<std::uintptr_t>(returnAddr));

    // ── POST-PROCESSOR ──
    BYTE* postProcess = p;
    *postProcessAddr = (DWORD)postProcess;

    // pushad
    *p++ = 0x60;

    // push dword [esp+0x1C]  (saved EAX = struct ptr)
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x1C;

    // call PatchDecryptedData
    p = patcher_logic::EmitRelCall32(
            p, reinterpret_cast<std::uintptr_t>(&PatchDecryptedData));

    // add esp, 4
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;

    // mov [esp+0x1C], eax  (update EAX in pushad frame)
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x1C;

    // popad
    *p++ = 0x61;

    // push [g_savedRetAddr]; ret
    *p++ = 0xFF; *p++ = 0x35;
    *(DWORD*)p = (DWORD)&g_savedRetAddr; p += 4;
    *p++ = 0xC3;

    // ── Patch hook site ──
    DWORD oldProtect;
    if (!VirtualProtect(g_hookSite, hookLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("DECRYPT HOOK: VirtualProtect failed");
        return false;
    }

    patcher_logic::EmitRelJmp32(
        g_hookSite, reinterpret_cast<std::uintptr_t>(g_trampoline));
    patcher_logic::FillNops(g_hookSite + 5, hookLen - 5);

    VirtualProtect(g_hookSite, hookLen, oldProtect, &oldProtect);

    Log("DECRYPT HOOK: installed at RVA 0x%X (trampoline=0x%p, post=0x%p)",
        hookRVA, g_trampoline, postProcess);
    return true;
}

// ── Runtime jump-target relocation ──────────────────────────────────────
// Control flow in this engine is explicit absolute-offset jumps. The decrypt
// patcher relocates the choice table (opcode 0x2A) in the buffer, but the
// other jump opcodes (0x29 GOTO, 0x02 GOSUB, 0x26/0x27 conditional, 0x28
// two-way) carry targets that go stale once text replacement shifts the
// script. They are unanchored single bytes -- only locatable by walking the
// bytecode exactly as the interpreter does -- so instead of relocating them in
// the buffer we let the engine execute the jump, then fix the position it
// landed on.
//
// The dispatcher at RVA 0x10F66 tail-JMPs to handler = [0x4513F0 + op*4]; the
// handler sets [0x491A0C]->position to the raw target and RETs to the
// interpreter loop. We replace the dispatch-table entry for each jump opcode
// with a stub that saves the entry position P0 and the loop return address,
// swaps the return address to a shared post-handler, then JMPs the original
// handler. When the handler RETs, the post-handler relocates the new position
// via the per-buffer cumulative-delta map -- unless it equals P0+3 (a
// conditional that fell through past its 3-byte operand instead of jumping).

static DWORD g_jumpSavedP0  = 0;
static DWORD g_jumpSavedRet = 0;
static BYTE* g_jumpTramp    = nullptr;
static int   g_jumpRelocLog = 0;

static void __cdecl RelocateAfterJump(DWORD p0)
{
    if (!g_exeBase) return;
    DWORD* ctx = *(DWORD**)(g_exeBase + 0x91A0C);
    if (!ctx) return;
    const DWORD p1   = ctx[0];        // [ctx+0] = script position after the handler ran
    const DWORD base = ctx[1];        // [ctx+4] = script buffer base

    int slot = -1;
    for (int i = 0; i < kMaxPatchedBufs; i++)
        if ((DWORD)g_patchedBase[i] == base) { slot = i; break; }
    if (slot < 0) return;             // unpatched script -> raw target is correct

    // Validate the slot still describes the buffer executing here. The engine
    // can free a patched script and repopulate the SAME address through a path
    // that does NOT go through our decrypt hook (e.g. an op0x03 sub-context
    // built by 0xe9a0/0x22080, never 0xe810) -- so neither invalidation point
    // fires and this slot goes stale. Applying our dead delta-map to the new
    // (unpatched, already-correct) script's jump is exactly what derails it:
    // the cursor drops mid-string and the engine null-evals at RVA 0xDF15.
    // PatchDecryptedData stamps every patched buffer's post-patch size as a
    // BE24 at offset 8 (and the engine only reads it, so it stays put); if that
    // signature no longer equals the recorded size, a different script now
    // occupies this address. Drop the stale slot and treat the jump as
    // unpatched (its raw target is already correct).
    const DWORD size = g_patchedSize[slot];
    if (translator_logic::ReadBE24((const unsigned char*)base + 8) != size) {
        Log("Translator: stale ring slot #%d base=0x%08X size-sig %u != %u "
            "-- buffer reused without decrypt; skip reloc + drop slot",
            slot, base,
            translator_logic::ReadBE24((const unsigned char*)base + 8), size);
        g_patchedBase[slot] = nullptr;
        g_patchedSize[slot] = 0;
        g_patchedCum[slot].clear();
        return;
    }

    // Skip the script-entry bootstrap. Every script begins at offset 0x10 with a
    // conditional 0x26 whose BE24 operand IS the header table pointer at 0x16
    // (= table_start - 5), which PatchDecryptedData already relocates in the
    // buffer. On a save-load this jump fires to that already-patched offset, so
    // relocating it again overshoots; the handler-entry position there is 0x11.
    if (p0 == 0x11 || p0 == 0) return;

    // Distinguish a TAKEN conditional jump from a fall-through. The conditional
    // opcodes 0x26/0x27 carry a variable-length condition expression before
    // their BE24 operand:  <op> <expr> FF <BE24>.  When the branch is NOT taken
    // the handler just steps the position past the operand, leaving it at
    // FFindex+1+3 -- a LIVE patched-buffer cursor that must NOT be relocated.
    // (The taken path instead writes the raw BE24 target, which DOES need it.)
    // The previous guard assumed the fall-through sat at p0+3, which is only the
    // no-expression layout; for these the real fall-through is past the
    // expression, so fall-throughs were being mis-relocated -- dropping the
    // cursor mid-string (engine then read translated text as a value parameter:
    // "0x0d / value-type parameter was given a string").
    //
    // We replicate the engine's expression-token consumption (eval helper
    // 0xD6B0) from the first expr byte to find the FF terminator, then skip
    // relocation when p1 is exactly that fall-through. The opcode byte sits one
    // before the handler-entry position. The other hooked opcodes (0x02 GOSUB,
    // 0x28 two-way, 0x29 GOTO) are unconditional -- they always set the position
    // from a BE24 target and have no fall-through path -- so they always relocate.
    const BYTE op = *(BYTE*)(base + p0 - 1);
    if (op == 0x26 || op == 0x27) {
        DWORD q = p0;                                   // first expression byte
        for (int guard = 0; guard < 0x4000 && q < size; guard++) {
            const BYTE t = *(BYTE*)(base + q);
            if (t == 0xFF) {                            // bare 0xFF = expr terminator
                if (p1 == q + 4) return;                // fell through -> keep position
                break;
            }
            const BYTE low = t & 0x0F;
            if (t < 0x40) {                             // small int / inline literal
                if ((t & 0xF0) == 0)
                    q += 1 + (low == 0x0D ? 1 : low == 0x0E ? 2 : low == 0x0F ? 4 : 0);
                else
                    q += 1 + (low == 0x0E ? 1 : low == 0x0F ? 2 : 0);
            } else if ((t & 0xF0) < 0x80) {
                q += 1;                                 // 0x40-0x7F operator: no operand
            } else if ((t & 0xF0) == 0x80) {
                // inline string operand (whole 0x80-0x8F range -- the engine
                // gates this path on (byte & 0xF0) == 0x80, low nibble does not
                // affect the advance): reads UTF-16LE words or single bytes until
                // NUL, depending on the string-mode flag at [ctx+0x406DC]
                // (nonzero = wide). Mirror it exactly.
                q += 1;
                if (*(BYTE*)((BYTE*)ctx + 0x406DC)) {
                    while (q + 1 < size) { const WORD w = *(WORD*)(base + q); q += 2; if (!w) break; }
                } else {
                    while (q < size) { const BYTE b = *(BYTE*)(base + q); q += 1; if (!b) break; }
                }
            } else {                                    // 0x81-0xFE
                q += 1 + (low == 0x0E ? 1 : low == 0x0F ? 2 : 0);
            }
        }
    }

    const int adj = translator_logic::ApplyCumulativeDelta(p1, g_patchedCum[slot]);
    if (adj == 0) return;
    const DWORD np = (DWORD)((int)p1 + adj);
    // Defense: a correctly-relocated raw target always lands inside the patched
    // buffer. If it would not, p1 was already a patched offset (e.g. read from
    // an in-buffer-relocated structure) -- don't double-relocate it.
    if (np >= size) return;
    if (g_jumpRelocLog < 24) {
        Log("JumpReloc: op=0x%02X P0=0x%X target(raw)=0x%X -> 0x%X (base=0x%08X)",
            op, p0, p1, np, base);
        g_jumpRelocLog++;
    }
    ctx[0] = np;
}

static bool InstallJumpHooks(BYTE* base)
{
    const int  kOps[]   = {0x02, 0x26, 0x27, 0x28, 0x29};
    const DWORD tableRVA = 0x513F0;            // dispatcher: jmp [eax*4 + 0x4513F0]
    DWORD* table = (DWORD*)(base + tableRVA);

    g_jumpTramp = (BYTE*)VirtualAlloc(nullptr, 1024, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    if (!g_jumpTramp) { Log("JUMP HOOKS: VirtualAlloc failed"); return false; }

    BYTE* p = g_jumpTramp;

    // shared post-handler: relocate, then return to the interpreter loop
    BYTE* post = p;
    *p++ = 0x9C;                                              // pushfd
    *p++ = 0x60;                                              // pushad
    *p++ = 0xFF; *p++ = 0x35; *(DWORD*)p = (DWORD)&g_jumpSavedP0;  p += 4;  // push [P0]
    *p++ = 0xE8; *(DWORD*)p = (DWORD)&RelocateAfterJump - ((DWORD)p + 4); p += 4;  // call
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                   // add esp, 4
    *p++ = 0x61;                                             // popad
    *p++ = 0x9D;                                             // popfd
    *p++ = 0xFF; *p++ = 0x25; *(DWORD*)p = (DWORD)&g_jumpSavedRet; p += 4;  // jmp [ret]

    DWORD oldProtect;
    if (!VirtualProtect(table, sizeof(DWORD) * 0x40, PAGE_READWRITE, &oldProtect)) {
        Log("JUMP HOOKS: VirtualProtect(table) failed"); return false;
    }
    for (int idx = 0; idx < (int)(sizeof(kOps) / sizeof(kOps[0])); idx++) {
        const int   op   = kOps[idx];
        const DWORD orig = table[op];          // original handler address
        BYTE* stub = p;
        *p++ = 0xA1; *(DWORD*)p = (DWORD)(base + 0x91A0C); p += 4;  // mov eax,[ctxglobal]
        *p++ = 0x8B; *p++ = 0x00;                                  // mov eax,[eax]   (P0)
        *p++ = 0xA3; *(DWORD*)p = (DWORD)&g_jumpSavedP0;  p += 4;   // mov [P0],eax
        *p++ = 0x8B; *p++ = 0x04; *p++ = 0x24;                     // mov eax,[esp]   (ret)
        *p++ = 0xA3; *(DWORD*)p = (DWORD)&g_jumpSavedRet; p += 4;   // mov [ret],eax
        *p++ = 0xC7; *p++ = 0x04; *p++ = 0x24; *(DWORD*)p = (DWORD)post; p += 4;  // mov [esp],post
        *p++ = 0xE9; *(DWORD*)p = orig - ((DWORD)p + 4); p += 4;    // jmp orig handler
        table[op] = (DWORD)stub;
    }
    VirtualProtect(table, sizeof(DWORD) * 0x40, oldProtect, &oldProtect);

    Log("JUMP HOOKS: installed for opcodes 0x02/0x26/0x27/0x28/0x29 "
        "(tramp=0x%p, post=0x%p)", g_jumpTramp, post);
    return true;
}

// ── Engine bounds-check reporter hook ───────────────────────────────────
// The engine validates every buffer/variable index before use and, on
// failure, pops its OWN modal warning ("範囲外バッファアクセス … BufNo = %d …
// このまま処理を続けますか？"). That path raises no CPU exception, so the VEH
// above never sees it -- the log stays clean even though the cursor has
// derailed (a wrong dword got read as a buffer number, the same offset-shift
// failure family the jump relocation fixes). To make it observable we hook
// the engine's shared wide-sprintf at RVA 0xCFD0 -- through which all ~60
// inlined bounds checks funnel their error string -- and log the script state
// when the format string is one of the buffer/range-error templates. (0xCFD0
// has 639 callers total; the format-pointer filter keeps normal text
// formatting out of the log so only real bounds failures are recorded.)
//
// 0xCFD0 prologue (cdecl, variadic): 55 8B EC B8 10 22 00 00
//   push ebp / mov ebp,esp / mov eax,0x2210   (frame size for __chkstk)
// Steal those 8 bytes, resume at 0xCFD8. The stub is a pure observer: it
// saves state, calls the C logger with a pointer to the original stack args,
// restores, runs the stolen bytes, and jumps back -- it never alters the
// engine's arguments or control flow.

static BYTE* g_reporterTramp   = nullptr;
static int   g_reportLogCount  = 0;

// RVAs (static image base 0x400000) of the error-template format strings
// worth capturing -- the buffer/range/variable out-of-range family.
static const DWORD kReportFmtRVAs[] = {
    0x52490,  // 範囲外バッファアクセス\nBufNo = %d            (the observed crash)
    0x528E0,  // 範囲外へのアクセス\nBufNo = %d\nIndex = %d
    0x524D4,  // BufNo = %d\n要求タイプ = %d\nバッファタイプ = %d
    0x52324,  // 範囲外変数アクセス（読み取り）\n%s%d
    0x52350,  // 範囲外変数アクセス（書き込み）\n%s%d
    0x5237C,  // 範囲外変数アクセス（書き込み）
};

// a[0]=caller return addr, a[1]=format string, a[2..]=the variadic args.
static void __cdecl ReportLogHook(DWORD* a)
{
    if (!g_exeBase) return;
    const DWORD ret    = a[0];
    const DWORD fmt    = a[1];
    const DWORD fmtRva = fmt - (DWORD)g_exeBase;

    bool watched = false;
    for (DWORD r : kReportFmtRVAs) if (r == fmtRva) { watched = true; break; }
    if (!watched) return;                       // normal formatting -> ignore
    if (g_reportLogCount++ >= 40) return;       // cap (one dialog can repeat)

    __try {
        Log("=== ENGINE BOUNDS WARN (fmt RVA 0x%X) ===", fmtRva);
        Log("  caller=0x%08X RVA 0x%X  args: %08X %08X %08X (a1=%d)",
            ret, ret - (DWORD)g_exeBase, a[2], a[3], a[4], (int)a[2]);
        LogWideAt("fmt", fmt, 64);

        const DWORD stateP = *(DWORD*)(g_exeBase + 0x91A0C);
        if (stateP) {
            const DWORD pos    = *(DWORD*)(stateP + 0);
            const DWORD base   = *(DWORD*)(stateP + 4);
            const DWORD cursor = base + pos;
            DWORD bo = 0;
            const int bidx = BufIndexOf(base, &bo);
            Log("  ScriptState[0x491A0C]=0x%08X position=0x%X base=0x%08X "
                "cursor=0x%08X", stateP, pos, base, cursor);
            if (bidx >= 0)
                Log("        base = patched buf #%d + 0x%X (size=%u) -- "
                    "cursor at script offset 0x%X",
                    bidx, bo, g_patchedSize[bidx], pos);
            else
                Log("        base = NOT a tracked patched buffer "
                    "(unpatched script, or evicted from the %d-slot ring)",
                    kMaxPatchedBufs);
            LogBytesAt("cursor", cursor, 32);
            LogWideAt("cursor-as-text", cursor, 24);
        } else {
            Log("  ScriptState[0x491A0C]: NULL");
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("  ReportLogHook: exception while logging");
    }
}

static bool InstallReporterHook(BYTE* base)
{
    const DWORD  hookRVA = 0xCFD0;
    const size_t stolen  = 8;
    BYTE* site   = base + hookRVA;
    BYTE* resume = site + stolen;            // 0xCFD8

    const BYTE expected[] = {0x55, 0x8B, 0xEC, 0xB8, 0x10, 0x22, 0x00, 0x00};
    if (!patcher_logic::BytesMatch(site, expected, stolen)) {
        Log("REPORTER HOOK: bytes mismatch at RVA 0x%X "
            "(found %02X %02X %02X %02X %02X %02X %02X %02X)",
            hookRVA, site[0], site[1], site[2], site[3],
            site[4], site[5], site[6], site[7]);
        return false;
    }

    g_reporterTramp = (BYTE*)VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_EXECUTE_READWRITE);
    if (!g_reporterTramp) { Log("REPORTER HOOK: VirtualAlloc failed"); return false; }

    BYTE* p = g_reporterTramp;
    *p++ = 0x60;                                                 // pushad
    *p++ = 0x9C;                                                 // pushfd
    *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x24;          // lea eax,[esp+0x24]
    *p++ = 0x50;                                                 // push eax (&args)
    p = patcher_logic::EmitRelCall32(
            p, reinterpret_cast<std::uintptr_t>(&ReportLogHook));
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                       // add esp,4
    *p++ = 0x9D;                                                 // popfd
    *p++ = 0x61;                                                 // popad
    p = patcher_logic::EmitBytes(p, expected, stolen);           // stolen prologue
    p = patcher_logic::EmitRelJmp32(
            p, reinterpret_cast<std::uintptr_t>(resume));        // -> 0xCFD8

    DWORD oldProtect;
    if (!VirtualProtect(site, stolen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("REPORTER HOOK: VirtualProtect failed"); return false;
    }
    patcher_logic::EmitRelJmp32(
        site, reinterpret_cast<std::uintptr_t>(g_reporterTramp));
    patcher_logic::FillNops(site + 5, stolen - 5);
    VirtualProtect(site, stolen, oldProtect, &oldProtect);

    Log("REPORTER HOOK: installed at RVA 0x%X (tramp=0x%p)", hookRVA, g_reporterTramp);
    return true;
}

// ── Init / Shutdown ────────────────────────────────────────────────────

static DWORD WINAPI TranslatorThread(LPVOID)
{
    LoadTranslations();
    if (!g_translationsLoaded) {
        Log("Translator: no translations loaded, hook not installed");
        return 0;
    }

    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    Log("Translator: ImageBase=0x%p", base);

    if (InstallDecryptHook(base)) {
        InstallJumpHooks(base);
        InstallReporterHook(base);
        Log("Translator: runtime translation ACTIVE (%d entries)",
            (int)g_translations.size());
    } else {
        Log("Translator: hook installation FAILED");
    }
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

    Log("Translator stats: %d decrypt calls, %d scripts patched, %d strings replaced",
        g_decryptCalls, g_scriptsPatched, g_stringsReplaced);

    if (g_trampoline) {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = nullptr;
    }

    if (g_reporterTramp) {
        VirtualFree(g_reporterTramp, 0, MEM_RELEASE);
        g_reporterTramp = nullptr;
    }

    if (g_vehHandle) {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }
}
