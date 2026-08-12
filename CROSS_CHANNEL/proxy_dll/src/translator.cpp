// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator.h"
#include "translator_logic.h"
#include "iat_hook.h"
#include "log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <vector>
#include <fstream>
#include <sstream>

// ============================================================================
// Cross-Channel runtime script patcher.
//
// Hooks FUN_00855910 (RVA 0x45910), the LZSS decompressor that unpacks
// sn.bin (and other engine resources) into a heap buffer. After the
// decompressor returns, we scan the buffer for JP byte sequences listed
// in translations.tsv and replace them with EN in place.
//
// CALLING CONVENTION (non-standard):
//   ECX = input pointer to [4-byte LE size] [LZSS stream]
//   EAX = output buffer (caller-allocated, size from input header)
// We save EAX/ECX at hook entry, let the decompressor run, then patch
// the output buffer once it's filled.
//
// HOOK ARCHITECTURE (mirrors shingakkou's decrypt_hxb hook):
//   1. Dispatcher (replaces FUN_00855910 entry):
//      - pop  edx                  ; original retaddr → edx
//      - mov  [g_savedRetAddr], edx
//      - push <PostHandler>         ; new retaddr — original returns here
//      - mov  [g_savedOutBuf], eax
//      - mov  [g_savedInPtr], ecx
//      - <stolen prologue>          ; 55 8B EC 83 EC 0C  (6 bytes)
//      - jmp  <site + 6>            ; continue original body
//   2. Original LZSS body runs and `ret`s — but the retaddr now points to
//      our PostHandler instead of the real caller.
//   3. PostHandler:
//      - pushad
//      - push [g_savedInPtr]; push [g_savedOutBuf]
//      - call PatchScriptBuffer (our C function)
//      - add  esp, 8
//      - popad
//      - jmp  [g_savedRetAddr]      ; back to the real caller
//
// FILTERING: FUN_00855910 is called for multiple resources (10 callers in
// the binary). We patch only when the input header says decompressed size
// = 5,702,000 bytes — sn.bin's known decompressed size, used to identify
// the script archive among other compressed resources.
//
// BYTE BUDGET: Text is replaced in-place where it fits in the original
// null-terminated slot. EN longer than the JP byte length is logged and
// skipped here; those strings are handled by the render-time hook
// (PreLoopHook / PostLoopHook) instead. Removing the in-place limit would
// require allocating a larger output buffer + fixing up any opcode-internal
// text-offset references, which is a separate workstream.
// ============================================================================

namespace {

// ── LZSS decompressor hook (in-place buffer patcher) ─────────────────────
constexpr DWORD  LZSS_RVA       = 0x45910;
constexpr int    STOLEN_LEN     = 6;
constexpr BYTE   EXPECTED[STOLEN_LEN] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };

constexpr DWORD  SN_BIN_DECOMPRESSED_SIZE = 5702000;

BYTE*  g_exeBase       = nullptr;
BYTE*  g_hookSite      = nullptr;
BYTE*  g_dispatcher    = nullptr;
BYTE*  g_postHandler   = nullptr;

BYTE*  g_savedOutBuf   = nullptr;
BYTE*  g_savedInPtr    = nullptr;
DWORD  g_savedRetAddr  = 0;

translator_logic::TranslationMap g_translations;
bool g_translationsLoaded = false;

int g_decompressCalls   = 0;
int g_snBinPatched      = 0;
int g_jpHits            = 0;
int g_jpTooLong         = 0;


// ── Render-time text substitution hook (FUN_0083a7a0, RVA 0x2A7A0) ────────
//
// FUN_0083a7a0 is the actual text renderer (39 calls per session confirmed).
// At offset +474 it reads the current text position from [0x06D92B78]:
//   A1 78 2B D9 06  MOV EAX, [0x06D92B78]
// then walks EAX char-by-char until null. At offset +493 it writes EAX back:
//   A3 78 2B D9 06  MOV [0x06D92B78], EAX   (position one-past-null)
//
// Hook strategy:
//   PreRenderHook  — read [0x06D92B78] (JP text ptr), look up in translations,
//                    redirect the global to a DLL buffer holding EN text.
//   PostRenderHook — restore [0x06D92B78] to jp_ptr+jp_len+1 (one past JP null)
//                    so the script interpreter advances correctly over JP bytes.
//
// Global address resolved at runtime from instruction bytes at tsite+475
// (the imm32 of the A1 opcode at offset +474), handling ASLR relocation.

constexpr DWORD  TEXT_RVA       = 0x2A7A0;
constexpr int    TEXT_STLEN     = 6;
constexpr BYTE   TEXT_EXPECTED[TEXT_STLEN] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24 };

// FUN_0083bcc0 — fullscreen/centered narration renderer (13 calls per session).
// Same engine global [0x06D92B78], different prologue and different render-loop offsets.
constexpr DWORD  NARR_RVA       = 0x2BCC0;
constexpr BYTE   NARR_EXPECTED[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18 };
constexpr int    NARR_PRE_OFF   = 468;   // A1 78 2B D9 06  MOV EAX,[0x06D92B78]
constexpr int    NARR_POST_OFF  = 491;   // A3 78 2B D9 06  MOV [0x06D92B78],EAX

// Choice opcode handlers — FUN_008360A0 / 00836500 / 00836AC0 share a common
// prologue that reads the script cursor and parses option records.  At +3 each
// has a 5-byte `MOV EAX, [DAT_06D92B78]` (A1 78 2B D9 06).  We patch that with
// `JMP <dispatcher>`; the dispatcher swaps the cursor to a buffer holding the
// same opcode structure but with full-EN strings replacing JP, then runs the
// stolen MOV (which now loads our buffer pointer).  A return-address swap
// makes a post-handler restore the cursor to the correct post-opcode position.
//
// Hooked at +3 means the original PUSH EBP / MOV EBP, ESP at +0..+2 still run
// before the JMP, so EBP is set up the way the function expects.  Stolen
// instruction is the same shape regardless of which of the 3 functions is hit,
// so a single dispatcher handles all of them; we install the same patch on
// each function entry.
constexpr DWORD CHOICE_FN_RVAS[] = { 0x260A0, 0x26500, 0x26AC0 };
constexpr int   CHOICE_FN_COUNT  = 3;
constexpr int   CHOICE_HOOK_OFF  = 3;
constexpr int   CHOICE_STOLEN_LEN = 5;
constexpr BYTE  CHOICE_STOLEN[CHOICE_STOLEN_LEN] = { 0xA1, 0x78, 0x2B, 0xD9, 0x06 };

// FUN_0089a0e0 — centering helper; called right after FUN_0081d8c0 in the narration
// renderer to compute the starting X for the first line.  At that call site it takes
// no arguments and reads the text from piVar10[0] (DAT_06d922D0 in Ghidra space).
// We find and replace that specific CALL within FUN_0083bcc0 so we can temporarily
// null-terminate the buffer at the first %N, giving the correct centering for the
// first line only, then restore the full wrapped string for the character loop.
constexpr DWORD  NARR_A0E0_RVA  = 0x8A0E0;   // Ghidra VA 0x0089a0e0, base 0x00810000

// piVar10 base — runtime VA of the render-state array (piVar10 in Ghidra decompilation).
// Derived at hook-install time from g_textPtrGlobal using the Ghidra-space delta.
//   Ghidra VA of DAT_06d92b78 (text-ptr global) : 0x06D92B78
//   Ghidra VA of DAT_06d922D0 (piVar10[0])       : 0x06D922D0
//   delta = g_textPtrGlobal - 0x06D92B78
//   g_piVar10Base = 0x06D922D0 + delta
static DWORD g_piVar10Base        = 0;
static DWORD g_origCenteringCallVA = 0;   // runtime VA of FUN_0089a0e0
static char* g_textWrapRestorePos  = nullptr;  // %N position to restore after centering
static int   g_centeringCalls     = 0;

// The engine's script text-position global [0x06D92B78].
// Set at hook-install time from the A1 imm32 at tsite+475 (handles ASLR).
static DWORD g_textPtrGlobal = 0;   // VA of the global (holds current text ptr)
static DWORD g_postLoopEAX   = 0;   // EAX saved by PostLoopTramp before pushad

BYTE*  g_textJpNullPtr     = nullptr;
bool   g_textSubstituted   = false;
char   g_textRenderBuf[4096];

int    g_renderSubs        = 0;
int    g_renderCalls       = 0;

// Choice hook state — see CHOICE_FN_RVAS above.
BYTE*  g_choicePostHandler  = nullptr;
DWORD  g_choiceSavedRetAddr = 0;
BYTE*  g_choiceRestoreCursor = nullptr;
BYTE   g_choiceBuf[8192];
int    g_choiceHookFires    = 0;

// ── TSV loader ────────────────────────────────────────────────────────────

bool LoadTranslations() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash) wcscpy(slash + 1, L"translations.tsv");
    else       wcscpy(exePath, L"translations.tsv");
    char pathA[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, exePath, -1, pathA, sizeof(pathA), nullptr, nullptr);
    bool ok = translator_logic::LoadTsvFile(pathA, g_translations);
    if (!ok) { Log("Translator: translations.tsv NOT FOUND at %s", pathA); return false; }
    Log("Translator: loaded %zu entries from translations.tsv", g_translations.size());
    return true;
}


// ── Word-wrap helper ─────────────────────────────────────────────────────
// The narration renderer (FUN_0083bcc0) recognises "%N" as an explicit line
// break: it resets X to 0, advances Y, and continues rendering the rest of
// the string centered on the new line (same as its own overflow-wrap logic).
// We pre-wrap at word boundaries at WRAP_COLS=60 so the engine's own hard-
// wrap (at ~65 chars) never fires mid-word.
static constexpr int WRAP_COLS = 72;

static void WordWrap(const char* src, char* dst, size_t dst_size) {
    size_t out = 0;
    int col = 0;
    const char* p = src;
    while (*p && out + 2 < dst_size) {
        if (*p == ' ') {
            const char* q = p + 1;
            int wlen = 0;
            while (*q && *q != ' ') { q++; wlen++; }
            if (col + 1 + wlen > WRAP_COLS) {
                dst[out++] = '%'; dst[out++] = 'N';  // engine newline
                col = 0;
            } else {
                dst[out++] = ' '; col++;
            }
            p++;
            continue;
        }
        dst[out++] = *p++; col++;
    }
    dst[out] = '\0';
}

// ── Mid-function render hooks ─────────────────────────────────────────────
//
// C_PreLoopHookText / C_PreLoopHookNarr: called by PreLoopTramp which replaces
//   A1 78 2B D9 06  (MOV EAX, [0x06D92B78])  at FUN_0083a7a0 +474 (textbox)
//   and FUN_0083bcc0 +468 (narration).
// At this point [0x06D92B78] holds the ACTUAL text pointer (the A1 instruction
// is right before the character-render loop, after all opcode dispatch).
// Both variants share DoTextSubstitution() for the lookup + wrap; the narration
// variant additionally truncates the wrapped buffer at the first %N for the
// centering fix (see C_CenteringHook below).
//
// C_PostLoopHook: called by PostLoopTramp which replaces the 5-byte instruction
//   A3 78 2B D9 06  (MOV [0x06D92B78], EAX)  at FUN_0083a7a0 offset +493.
// EAX at this point is position-past-null of the rendered string, which the
// engine uses to advance the script position. PostLoopHook either lets the
// original advance happen (no substitution) or corrects it to past JP null.

// Shared lookup + wrap.  Leaves the wrapped string (including any %N markers)
// intact in g_textRenderBuf so the character render loop renders all lines.
static void DoTextSubstitution() {
    if (!g_translationsLoaded || g_translations.empty() || !g_textPtrGlobal) return;
    BYTE* jp_ptr = *reinterpret_cast<BYTE**>(g_textPtrGlobal);
    if (!jp_ptr || jp_ptr[0] == 0xFF) { g_textSubstituted = false; return; }

    size_t jp_len = strnlen(reinterpret_cast<char*>(jp_ptr), 4096);
    g_renderCalls++;
    if (g_renderCalls <= 5) {
        Log("PRE-LOOP #%d: jp_ptr=%p len=%zu bytes=[%02X %02X %02X %02X %02X]",
            g_renderCalls, jp_ptr, jp_len,
            jp_ptr[0], jp_len>1?jp_ptr[1]:0, jp_len>2?jp_ptr[2]:0,
            jp_len>3?jp_ptr[3]:0, jp_len>4?jp_ptr[4]:0);
    }
    if (jp_len == 0) { g_textSubstituted = false; return; }
    std::string jp_key(reinterpret_cast<char*>(jp_ptr), jp_len);

    // Try literal key first; on miss, retry with known leading decorators
    // stripped (e.g. ※). The TSV build pipeline drops some leading
    // decorators from JP keys but the engine still sends them with the
    // decorator at runtime — a literal lookup misses, the stripped
    // variant hits.
    const std::string* enPtr =
        translator_logic::LookupWithStrippedPrefix(g_translations, jp_key);
    if (!enPtr) {
        // Log misses so we can see if choice text reaches the dialogue hook
        // but fails the TSV lookup (cap at 30 to avoid log explosion).
        static int missCount = 0;
        if (++missCount <= 30) {
            Log("PRE-LOOP MISS #%d: len=%zu bytes=[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
                missCount, jp_len,
                jp_len>0?jp_ptr[0]:0, jp_len>1?jp_ptr[1]:0,
                jp_len>2?jp_ptr[2]:0, jp_len>3?jp_ptr[3]:0,
                jp_len>4?jp_ptr[4]:0, jp_len>5?jp_ptr[5]:0,
                jp_len>6?jp_ptr[6]:0, jp_len>7?jp_ptr[7]:0,
                jp_len>8?jp_ptr[8]:0, jp_len>9?jp_ptr[9]:0);
        }
        g_textSubstituted = false;
        g_textWrapRestorePos = nullptr;
        return;
    }
    const std::string& en = *enPtr;
    g_textJpNullPtr = jp_ptr + jp_len + 1;  // one past JP null terminator
    char enBuf[4096];
    size_t copy_len = en.size() < sizeof(enBuf) - 1 ? en.size() : sizeof(enBuf) - 1;
    memcpy(enBuf, en.data(), copy_len);
    enBuf[copy_len] = '\0';
    WordWrap(enBuf, g_textRenderBuf, sizeof(g_textRenderBuf));
    *reinterpret_cast<BYTE**>(g_textPtrGlobal) = reinterpret_cast<BYTE*>(g_textRenderBuf);
    g_textSubstituted = true;
    g_textWrapRestorePos = nullptr;
    g_renderSubs++;
    Log("RENDER-SUB #%d jp_len=%zu en_len=%zu en=[%.40s]",
        g_renderSubs, jp_len, en.size(), en.c_str());
}

// Scale patch: write a smaller value to the engine's float scale factors
// at piVar10 + 0x14 (X) and piVar10 + 0x18 (Y) before each render.  These
// are stored as float (1.0f by default) and scale both the rendered glyph
// size AND the per-character advance, so reducing them shrinks both the
// text size and the wide CJK letter spacing in one knob.
static constexpr float kRenderScale = 0.7f;  // 70% of original size

static void TryScalePatch() {
    // piVar10 is heap-allocated; its base differs per run depending on
    // how much memory was committed before the engine allocated it (e.g.
    // larger translations.tsv shifts the address).  Use the runtime-
    // derived g_piVar10Base instead of hardcoded constants — those only
    // worked by luck in early trial runs with small TSVs.
    if (!g_piVar10Base) return;  // not derived yet (NARR-CENTER hook hasn't fired)
    __try {
        float* pX = reinterpret_cast<float*>(g_piVar10Base + 0x14);  // piVar10[+0x14]
        float* pY = reinterpret_cast<float*>(g_piVar10Base + 0x18);  // piVar10[+0x18]
        static int once = 0;
        if (!once) {
            Log("SCALE-PATCH: writing %.3f to [%p] and [%p] (was %.3f / %.3f)",
                kRenderScale, pX, pY, *pX, *pY);
            once = 1;
        }
        *pX = kRenderScale;
        *pY = kRenderScale;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("SCALE-PATCH: AV");
    }
}

// Choice-menu / UI font scale.  FUN_008360A0 / 00836500 / 00836AC0 / 00836EA0
// (the 4 choice opcode handlers) load _DAT_008d4698 into uVar7 and pass it
// twice as the X/Y scale arguments to the per-char glyph queuer FUN_0081D3A0.
// Default is 6/7 = 0.857143.  FUN_0083A7A0 (dialogue) does NOT use this
// constant — it uses piVar10[+0x14]/[+0x18] which TryScalePatch handles
// — so writing here shrinks choice text without touching dialogue.
//
// Side note: 26 cross-references total. 8 are inside the 4 choice funcs;
// the remaining ~18 are in unidentified UI/menu functions (FUN_0082F5B0,
// FUN_008179C0, FUN_00817DE0, FUN_00839CD0, FUN_00840860, FUN_008409B0).
// They likely share the same UI font and shrink together — that's a
// feature, not a bug, since the user's grievance was "choice text too big
// relative to dialogue".
static constexpr DWORD kGhidraImageBase     = 0x00810000;
static constexpr DWORD kGhidraVA_ChoiceFont = 0x008D4698;
static constexpr float kChoiceFontScale     = 0.6f;   // 60% glyph scale; 64-px cell advance unchanged (tighter visual spacing without touching frame layout)

static void PatchChoiceFontScale() {
    if (!g_exeBase) return;
    DWORD runtimeVA = reinterpret_cast<DWORD>(g_exeBase)
                    + (kGhidraVA_ChoiceFont - kGhidraImageBase);
    float* p = reinterpret_cast<float*>(runtimeVA);
    // Sanity check: this constant is 6/7 = 0.857143 in cc_dumped.exe.
    // If we read 0 or NaN, .rdata isn't unpacked yet — caller should
    // delay until after HookInstallerThread sees the LZSS prologue.
    float was = 0.0f;
    __try { was = *p; } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("CHOICE-FONT: AV reading %p — skip", p);
        return;
    }
    if (was < 0.7f || was > 1.0f) {
        Log("CHOICE-FONT: unexpected current %.6f at %p — refusing patch "
            "(packer may not have run yet)", was, p);
        return;
    }
    DWORD oldProt = 0;
    if (!VirtualProtect(p, sizeof(float), PAGE_READWRITE, &oldProt)) {
        Log("CHOICE-FONT: VirtualProtect failed @ %p gle=%lu", p, GetLastError());
        return;
    }
    __try {
        *p = kChoiceFontScale;
        Log("CHOICE-FONT: patched [%p] %.6f -> %.3f (Ghidra _DAT_008d4698)",
            p, was, kChoiceFontScale);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("CHOICE-FONT: AV writing %p", p);
    }
    DWORD tmp;
    VirtualProtect(p, sizeof(float), oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(float));
}

// Textbox renderer (FUN_0083a7a0): left-aligned, walks the buffer char-by-char
// until null and treats %N as a line break (Ghidra: strncmp("%N",2)==0 branch
// at FUN_0083a7a0 +0x123).  Leave the wrapped buffer intact — no centering call
// to fix up here, so truncating would clip the message at the first wrap.
extern "C" void __cdecl C_PreLoopHookText() {
    DoTextSubstitution();
    TryScalePatch();
}

// Narration renderer (FUN_0083bcc0): centered.  FUN_0081d8c0 + FUN_0089a0e0
// run between this hook and the character loop and measure strlen(buffer) to
// compute the first-line X offset.  Without truncation they'd centre the
// entire wrapped string as one line.  Truncate at the first %N so they only
// see the first-line text; C_CenteringHook restores the %N after the inner
// FUN_0089a0e0 call returns so the char loop still renders all lines.
extern "C" void __cdecl C_PreLoopHookNarr() {
    DoTextSubstitution();
    TryScalePatch();
    if (!g_textSubstituted) return;
    for (char* p = g_textRenderBuf; p[0] && p[1]; p++) {
        if (p[0] == '%' && p[1] == 'N') {
            *p = '\0';
            g_textWrapRestorePos = p;
            break;
        }
    }
}

// C_CenteringHook — replaces the CALL FUN_0089a0e0 instruction inside FUN_0083bcc0
// that computes the first-line centering X (piVar10[1]).  By the time we reach this
// point C_PreLoopHookNarr has already truncated g_textRenderBuf at the first %N, so
// FUN_0081d8c0 + FUN_0089a0e0 have both seen only the first-line text and produced
// the correct centering.  Our job is to restore the %N afterwards so the character
// loop (which immediately follows) renders the full wrapped string.
extern "C" int __cdecl C_CenteringHook() {
    typedef int (__cdecl* Fn0e0_t)();
    Fn0e0_t orig = reinterpret_cast<Fn0e0_t>(g_origCenteringCallVA);

    int result = orig ? orig() : 0;

    g_centeringCalls++;
    if (g_centeringCalls <= 5) {
        Log("CENTER #%d: result=%d restore_pos=%p",
            g_centeringCalls, result, g_textWrapRestorePos);
    }

    if (g_textWrapRestorePos) {
        *g_textWrapRestorePos = '%';
        g_textWrapRestorePos = nullptr;
    }
    return result;
}

// Choice hook handlers ────────────────────────────────────────────────────
// Pre-handler: read the script cursor, parse the choice opcode at it, build
// our_buf with full-EN strings replacing JP, swap the cursor to point at our
// buffer, swap the function's return address to point at our post-handler.
// Post-handler restores the cursor to (orig_cursor + total_orig_opcode_size)
// so the script interpreter advances past the choice opcode in the ORIGINAL
// script, not past our (longer) replacement buffer.
//
// Layout per option in the script bytecode (verified in cc_dumped.exe):
//   [2 bytes preamble (active flag + 0x80 marker)]
//   [8 bytes metadata (jump label / option index / flags)]
//   [JP CP932 bytes, variable]
//   [null terminator]
// Header before options (6 bytes):
//   [opcode byte +1 padding] [count ushort] [default-index ushort]
// FUN_00833920 returns uint16 read at cursor and advances cursor by 2 bytes
// (it operates on ushort* with +1 stride). Called twice for count + default,
// preceded by an explicit +2 advance for the opcode byte. Net: +6 bytes.
static int BuildChoiceOurBuf(BYTE* cursor) {
    BYTE* out = g_choiceBuf;
    BYTE* end = g_choiceBuf + sizeof(g_choiceBuf) - 64;  // safety margin
    BYTE* in  = cursor;

    // Header (6 bytes copied verbatim)
    memcpy(out, in, 6);
    int count = in[2];                     // low byte of count ushort
    if (count < 0 || count > 16) return 0; // sanity
    out += 6;
    in  += 6;

    for (int i = 0; i < count; ++i) {
        if (out + 11 > end) break;
        // 2 preamble + 8 metadata = 10 bytes copied verbatim (engine reads
        // them for active-flag check, jump labels, etc).
        memcpy(out, in, 10);
        out += 10;
        in  += 10;

        // Read bytes until null
        size_t jp_len = 0;
        while (jp_len < 64 && in[jp_len] != 0) ++jp_len;
        std::string current(reinterpret_cast<const char*>(in), jp_len);

        // PatchScriptBuffer (LZSS decompress hook) has already replaced JP
        // with truncated EN by the time we reach this hook — sn.bin is
        // patched at decompress time, well before any choice opcode runs.
        // The bytes here are typically `"Go up to  "` (truncated + 0x20
        // space padding), not 屋上に行く. So:
        //   1. Strip trailing space padding (PatchScriptBuffer pads to fill
        //      the original JP byte width).
        //   2. Try forward lookup (in case some entry was unpatched).
        //   3. Fall back to prefix lookup (truncated EN -> full EN).
        while (!current.empty() && current.back() == ' ') current.pop_back();

        std::string en;
        bool have = false;
        if (g_translationsLoaded && !current.empty()) {
            auto it = g_translations.find(current);
            if (it != g_translations.end()) {
                en = it->second; have = true;
            } else if (Translator_PrefixLookup(current, en)) {
                have = true;
            }
        }
        if (!have) en = current;   // copy unchanged if both lookups missed

        size_t en_size = en.size();
        if (out + en_size + 1 > end) en_size = (end - out) - 1;
        memcpy(out, en.data(), en_size);
        out += en_size;
        *out++ = 0;

        in += jp_len + 1;   // past JP and its null
    }
    return static_cast<int>(in - cursor);
}

extern "C" void __cdecl C_PreChoiceHook(DWORD orig_ebp) {
    ++g_choiceHookFires;
    if (!g_textPtrGlobal) return;
    BYTE* orig_cursor = *reinterpret_cast<BYTE**>(g_textPtrGlobal);
    if (!orig_cursor) return;

    int orig_size = BuildChoiceOurBuf(orig_cursor);
    if (orig_size <= 0) return;
    g_choiceRestoreCursor = orig_cursor + orig_size;

    *reinterpret_cast<BYTE**>(g_textPtrGlobal) = g_choiceBuf;

    // Swap return address: function will RET to our post-handler instead of
    // its real caller. Original retaddr is at [orig_ebp+4] (PUSH EBP at +0
    // saved EBP; MOV EBP,ESP at +1..2 set EBP=ESP, so +4 is the CALL retaddr).
    DWORD* retaddr_ptr = reinterpret_cast<DWORD*>(orig_ebp + 4);
    g_choiceSavedRetAddr = *retaddr_ptr;
    *retaddr_ptr = reinterpret_cast<DWORD>(g_choicePostHandler);

    if (g_choiceHookFires <= 5) {
        Log("CHOICE-HOOK #%d: orig_cursor=%p orig_size=%d our_buf=%p "
            "saved_ret=0x%X",
            g_choiceHookFires, orig_cursor, orig_size, g_choiceBuf,
            g_choiceSavedRetAddr);
    }
}

extern "C" void __cdecl C_PostChoiceHook() {
    if (!g_textPtrGlobal) return;
    *reinterpret_cast<BYTE**>(g_textPtrGlobal) = g_choiceRestoreCursor;
    if (g_choiceHookFires <= 5) {
        Log("CHOICE-HOOK POST #%d: cursor restored to %p",
            g_choiceHookFires, g_choiceRestoreCursor);
    }
}

extern "C" void __cdecl C_PostLoopHook() {
    if (g_textSubstituted) {
        // Correct the script advance: point past JP null, not EN null.
        *reinterpret_cast<BYTE**>(g_textPtrGlobal) = g_textJpNullPtr;
        g_textSubstituted = false;
    } else {
        // Execute original A3 behavior: write saved EAX to the global.
        *reinterpret_cast<BYTE**>(g_textPtrGlobal) = reinterpret_cast<BYTE*>(g_postLoopEAX);
    }
}

// SEH-protected DWORD read used by PatchScriptBuffer to validate the
// LZSS-decompressed buffer's size header without risking an AV during
// normal startup transients.
extern "C" DWORD __cdecl SehReadDword(BYTE* p) {
    __try { return *reinterpret_cast<DWORD*>(p); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
}

extern "C" void PatchScriptBuffer(BYTE* outBuf, BYTE* inPtr);  // forward decl

// ── Post-decompress patcher (called from PostHandler trampoline) ─────────

extern "C" void __cdecl PatchScriptBuffer(BYTE* outBuf, BYTE* inPtr) {
    g_decompressCalls++;

    if (!outBuf || !inPtr) return;

    DWORD size = SehReadDword(inPtr);
    if (size == 0xFFFFFFFFu) return;

    // Filter to sn.bin only.
    if (size != SN_BIN_DECOMPRESSED_SIZE) {
        if (g_decompressCalls <= 20) {
            Log("PATCH: skip decompress call #%d (size=%u, not sn.bin)",
                g_decompressCalls, size);
        }
        return;
    }

    if (!g_translationsLoaded || g_translations.empty()) {
        Log("PATCH: sn.bin decompressed but no translations loaded");
        return;
    }

    Log("PATCH: sn.bin decompressed (size=%u) — scanning for %zu JP keys",
        size, g_translations.size());

    auto stats = translator_logic::PatchTranslationsInPlace(
        outBuf, outBuf + size, g_translations);

    g_snBinPatched++;
    g_jpHits     += stats.hits;
    g_jpTooLong  += stats.too_long;
    Log("PATCH: applied %d in-place, skipped %d (EN too long for JP byte slot)",
        stats.hits, stats.too_long);
}

// ── CreateFontA hook — reduce font size to fit more text per line ─────────
//
// cc.exe's narration renderer uses a software text engine; it does NOT call
// ExtTextOutA. The only GDI import it makes is CreateFontA, so that is our
// sole hook point. Scaling the font height down by FONT_SCALE lets more
// characters fit before the engine's hard pixel-wrap triggers, and also
// reduces horizontal overflow so long lines stay centered on-screen.
//
// FONT_SCALE = 3/5 → 60% of original height (40% reduction).
// Adjust if text looks too small or word-wrap is still triggering.

// ── CreateFontA inline hook (trampoline detour) ───────────────────────────
// IATHook fails on this packer because it zeros the ILT at runtime.
// Instead we patch the first 5 bytes of CreateFontA in gdi32.dll directly
// (copy-on-write keeps it process-local) and call back through a trampoline.

// Shared scale: 60% of original height (3/5).
static inline int ScaleFontH(int h) {
    if (h > 0) return (h * 3 + 4) / 5;
    if (h < 0) return (h * 3 - 4) / 5;
    return h;
}

// ── CreateFontA ───────────────────────────────────────────────────────────
typedef HFONT (WINAPI* CreateFontA_t)(int,int,int,int,int,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,LPCSTR);
static CreateFontA_t g_origCreateFontA = nullptr;
static BYTE* g_createFontATramp = nullptr;
static int g_createFontACalls = 0;

static HFONT WINAPI Hook_CreateFontA(int cHeight, int cWidth, int cEscapement, int cOrientation,
    int cWeight, DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut,
    DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision,
    DWORD iQuality, DWORD iPitchAndFamily, LPCSTR pszFaceName) {
    int origH = cHeight;
    cHeight = ScaleFontH(cHeight);
    g_createFontACalls++;
    if (g_createFontACalls <= 10) {
        Log("FONT-CALL A #%d: cHeight %d -> %d face='%s'",
            g_createFontACalls, origH, cHeight, pszFaceName ? pszFaceName : "(null)");
    }
    return g_origCreateFontA(cHeight, cWidth, cEscapement, cOrientation,
        cWeight, bItalic, bUnderline, bStrikeOut,
        iCharSet, iOutPrecision, iClipPrecision,
        iQuality, iPitchAndFamily, pszFaceName);
}

// ── CreateFontW ───────────────────────────────────────────────────────────
typedef HFONT (WINAPI* CreateFontW_t)(int,int,int,int,int,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,LPCWSTR);
static CreateFontW_t g_origCreateFontW = nullptr;
static BYTE* g_createFontWTramp = nullptr;
static int g_createFontWCalls = 0;

static HFONT WINAPI Hook_CreateFontW(int cHeight, int cWidth, int cEscapement, int cOrientation,
    int cWeight, DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut,
    DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision,
    DWORD iQuality, DWORD iPitchAndFamily, LPCWSTR pszFaceName) {
    int origH = cHeight;
    cHeight = ScaleFontH(cHeight);
    g_createFontWCalls++;
    if (g_createFontWCalls <= 10) {
        Log("FONT-CALL W #%d: cHeight %d -> %d", g_createFontWCalls, origH, cHeight);
    }
    return g_origCreateFontW(cHeight, cWidth, cEscapement, cOrientation,
        cWeight, bItalic, bUnderline, bStrikeOut,
        iCharSet, iOutPrecision, iClipPrecision,
        iQuality, iPitchAndFamily, pszFaceName);
}

// ── CreateFontIndirectA ───────────────────────────────────────────────────
typedef HFONT (WINAPI* CreateFontIndirectA_t)(const LOGFONTA*);
static CreateFontIndirectA_t g_origCreateFontIndirectA = nullptr;
static BYTE* g_createFontIndirectATramp = nullptr;
static int g_createFontIndirectACalls = 0;

static HFONT WINAPI Hook_CreateFontIndirectA(const LOGFONTA* lf) {
    if (!lf) return g_origCreateFontIndirectA(lf);
    LOGFONTA mod = *lf;
    int origH = mod.lfHeight;
    mod.lfHeight = ScaleFontH(mod.lfHeight);
    g_createFontIndirectACalls++;
    if (g_createFontIndirectACalls <= 10) {
        Log("FONT-CALL IA #%d: lfHeight %d -> %d face='%s'",
            g_createFontIndirectACalls, origH, (int)mod.lfHeight, mod.lfFaceName);
    }
    return g_origCreateFontIndirectA(&mod);
}

// ── CreateFontIndirectW ───────────────────────────────────────────────────
typedef HFONT (WINAPI* CreateFontIndirectW_t)(const LOGFONTW*);
static CreateFontIndirectW_t g_origCreateFontIndirectW = nullptr;
static BYTE* g_createFontIndirectWTramp = nullptr;
static int g_createFontIndirectWCalls = 0;

static HFONT WINAPI Hook_CreateFontIndirectW(const LOGFONTW* lf) {
    if (!lf) return g_origCreateFontIndirectW(lf);
    LOGFONTW mod = *lf;
    int origH = mod.lfHeight;
    mod.lfHeight = ScaleFontH(mod.lfHeight);
    g_createFontIndirectWCalls++;
    if (g_createFontIndirectWCalls <= 10) {
        Log("FONT-CALL IW #%d: lfHeight %d -> %d",
            g_createFontIndirectWCalls, origH, (int)mod.lfHeight);
    }
    return g_origCreateFontIndirectW(&mod);
}

// ── Generic inline-detour installer ───────────────────────────────────────
// Patches first 5 bytes of `procName` in gdi32.dll with JMP to `hookFn`.
// Returns trampoline (stolen bytes + JMP back) via *outTramp; nullptr on failure.
static BYTE* InstallInlineDetour(const char* procName, void* hookFn, const char* tag) {
    HMODULE hGdi32 = GetModuleHandleA("gdi32.dll");
    if (!hGdi32) { Log("%s: gdi32 not loaded", tag); return nullptr; }
    BYTE* proc = reinterpret_cast<BYTE*>(GetProcAddress(hGdi32, procName));
    if (!proc) { Log("%s: %s not found", tag, procName); return nullptr; }

    BYTE* tramp = reinterpret_cast<BYTE*>(
        VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) { Log("%s: VirtualAlloc failed", tag); return nullptr; }

    memcpy(tramp, proc, 5);
    tramp[5] = 0xE9;
    *reinterpret_cast<DWORD*>(tramp + 6) = (DWORD)(proc + 5) - (DWORD)(tramp + 10);

    DWORD oldProt;
    VirtualProtect(proc, 5, PAGE_EXECUTE_READWRITE, &oldProt);
    proc[0] = 0xE9;
    *reinterpret_cast<DWORD*>(proc + 1) = (DWORD)hookFn - (DWORD)(proc + 5);
    VirtualProtect(proc, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), proc, 5);

    Log("%s: %s hooked (proc=%p tramp=%p)", tag, procName, proc, tramp);
    return tramp;
}

static void InstallCreateFontAHook() {
    g_createFontATramp = InstallInlineDetour("CreateFontA", (void*)Hook_CreateFontA, "FONT-HOOK");
    g_origCreateFontA = reinterpret_cast<CreateFontA_t>(g_createFontATramp);

    g_createFontWTramp = InstallInlineDetour("CreateFontW", (void*)Hook_CreateFontW, "FONT-HOOK");
    g_origCreateFontW = reinterpret_cast<CreateFontW_t>(g_createFontWTramp);

    g_createFontIndirectATramp = InstallInlineDetour("CreateFontIndirectA", (void*)Hook_CreateFontIndirectA, "FONT-HOOK");
    g_origCreateFontIndirectA = reinterpret_cast<CreateFontIndirectA_t>(g_createFontIndirectATramp);

    g_createFontIndirectWTramp = InstallInlineDetour("CreateFontIndirectW", (void*)Hook_CreateFontIndirectW, "FONT-HOOK");
    g_origCreateFontIndirectW = reinterpret_cast<CreateFontIndirectW_t>(g_createFontIndirectWTramp);
}

// ── GetGlyphOutline diagnostic hooks (logging only, no scaling) ───────────
// If the engine rasterizes its own glyphs from system TTF data, it'll call
// GetGlyphOutlineA/W per character. Hook + log both to find out.

typedef DWORD (WINAPI* GetGlyphOutlineA_t)(HDC,UINT,UINT,LPGLYPHMETRICS,DWORD,LPVOID,const MAT2*);
typedef DWORD (WINAPI* GetGlyphOutlineW_t)(HDC,UINT,UINT,LPGLYPHMETRICS,DWORD,LPVOID,const MAT2*);
static GetGlyphOutlineA_t g_origGetGlyphOutlineA = nullptr;
static GetGlyphOutlineW_t g_origGetGlyphOutlineW = nullptr;
static BYTE* g_getGlyphOutlineATramp = nullptr;
static BYTE* g_getGlyphOutlineWTramp = nullptr;
static int g_getGlyphOutlineACalls = 0;
static int g_getGlyphOutlineWCalls = 0;

static DWORD WINAPI Hook_GetGlyphOutlineA(HDC hdc, UINT uChar, UINT fuFormat,
    LPGLYPHMETRICS lpgm, DWORD cjBuffer, LPVOID pvBuffer, const MAT2* lpmat2) {
    g_getGlyphOutlineACalls++;
    if (g_getGlyphOutlineACalls <= 5) {
        Log("GLYPH-CALL A #%d: uChar=0x%X fuFormat=0x%X cjBuffer=%u",
            g_getGlyphOutlineACalls, uChar, fuFormat, cjBuffer);
    }
    return g_origGetGlyphOutlineA(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer, lpmat2);
}

static DWORD WINAPI Hook_GetGlyphOutlineW(HDC hdc, UINT uChar, UINT fuFormat,
    LPGLYPHMETRICS lpgm, DWORD cjBuffer, LPVOID pvBuffer, const MAT2* lpmat2) {
    g_getGlyphOutlineWCalls++;
    if (g_getGlyphOutlineWCalls <= 5) {
        Log("GLYPH-CALL W #%d: uChar=0x%X fuFormat=0x%X cjBuffer=%u",
            g_getGlyphOutlineWCalls, uChar, fuFormat, cjBuffer);
    }
    return g_origGetGlyphOutlineW(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer, lpmat2);
}

static void InstallGlyphOutlineHooks() {
    g_getGlyphOutlineATramp = InstallInlineDetour("GetGlyphOutlineA", (void*)Hook_GetGlyphOutlineA, "GLYPH-HOOK");
    g_origGetGlyphOutlineA = reinterpret_cast<GetGlyphOutlineA_t>(g_getGlyphOutlineATramp);
    g_getGlyphOutlineWTramp = InstallInlineDetour("GetGlyphOutlineW", (void*)Hook_GetGlyphOutlineW, "GLYPH-HOOK");
    g_origGetGlyphOutlineW = reinterpret_cast<GetGlyphOutlineW_t>(g_getGlyphOutlineWTramp);
}

// ── Hook installer ───────────────────────────────────────────────────────

DWORD WINAPI HookInstallerThread(LPVOID) {
    BYTE* site = g_exeBase + LZSS_RVA;
    DWORD waited = 0;
    while (waited < 30000) {
        if (memcmp(site, EXPECTED, STOLEN_LEN) == 0) break;
        Sleep(100);
        waited += 100;
    }
    if (waited >= 30000) {
        Log("HOOK: timed out waiting for LZSS prologue at RVA 0x%X", LZSS_RVA);
        return 0;
    }
    Log("HOOK: packer done after %u ms — installing LZSS hook at RVA 0x%X",
        waited, LZSS_RVA);

    // Now that .rdata is unpacked, the choice/UI font scale constant
    // (Ghidra _DAT_008d4698) is at its expected runtime VA with the
    // expected value (6/7 ≈ 0.857143). Patch it down here.
    PatchChoiceFontScale();

    BYTE* mem = reinterpret_cast<BYTE*>(VirtualAlloc(
        nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!mem) {
        Log("HOOK: VirtualAlloc failed gle=%lu", GetLastError());
        return 0;
    }
    g_dispatcher  = mem;
    g_postHandler = mem + 128;
    g_hookSite    = site;

    // ── Build dispatcher ──
    //   pop  edx
    //   mov  [g_savedRetAddr], edx
    //   push <PostHandler>
    //   mov  [g_savedOutBuf], eax
    //   mov  [g_savedInPtr], ecx
    //   <stolen 6 bytes>
    //   jmp  <site + 6>
    BYTE* p = g_dispatcher;
    *p++ = 0x5A;                                                            // pop edx
    *p++ = 0x89; *p++ = 0x15;
    *reinterpret_cast<DWORD*>(p) = (DWORD)&g_savedRetAddr; p += 4;          // mov [..], edx
    *p++ = 0x68;
    *reinterpret_cast<DWORD*>(p) = (DWORD)g_postHandler;   p += 4;          // push <PostHandler>
    *p++ = 0xA3;
    *reinterpret_cast<DWORD*>(p) = (DWORD)&g_savedOutBuf;  p += 4;          // mov [..], eax
    *p++ = 0x89; *p++ = 0x0D;
    *reinterpret_cast<DWORD*>(p) = (DWORD)&g_savedInPtr;   p += 4;          // mov [..], ecx
    memcpy(p, site, STOLEN_LEN);                                            // stolen prologue
    p += STOLEN_LEN;
    *p++ = 0xE9;
    *reinterpret_cast<DWORD*>(p) =
        (DWORD)(site + STOLEN_LEN) - (DWORD)(p + 4); p += 4;                // jmp original+6

    // ── Build post-handler ──
    //   pushad
    //   push [g_savedInPtr]; push [g_savedOutBuf]
    //   call PatchScriptBuffer
    //   add  esp, 8
    //   popad
    //   jmp  [g_savedRetAddr]
    BYTE* q = g_postHandler;
    *q++ = 0x60;                                                            // pushad
    *q++ = 0xFF; *q++ = 0x35;
    *reinterpret_cast<DWORD*>(q) = (DWORD)&g_savedInPtr;  q += 4;           // push [g_savedInPtr]
    *q++ = 0xFF; *q++ = 0x35;
    *reinterpret_cast<DWORD*>(q) = (DWORD)&g_savedOutBuf; q += 4;           // push [g_savedOutBuf]
    *q++ = 0xE8;
    *reinterpret_cast<DWORD*>(q) =
        (DWORD)(BYTE*)&PatchScriptBuffer - (DWORD)(q + 4); q += 4;          // call PatchScriptBuffer
    *q++ = 0x83; *q++ = 0xC4; *q++ = 0x08;                                  // add esp, 8
    *q++ = 0x61;                                                            // popad
    *q++ = 0xFF; *q++ = 0x25;
    *reinterpret_cast<DWORD*>(q) = (DWORD)&g_savedRetAddr; q += 4;          // jmp [g_savedRetAddr]

    // ── Patch site → JMP dispatcher + NOP pad ──
    DWORD oldProt;
    VirtualProtect(site, STOLEN_LEN, PAGE_EXECUTE_READWRITE, &oldProt);
    site[0] = 0xE9;
    *reinterpret_cast<DWORD*>(site + 1) =
        (DWORD)g_dispatcher - (DWORD)(site + 5);
    site[5] = 0x90;
    VirtualProtect(site, STOLEN_LEN, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), site, STOLEN_LEN);

    Log("HOOK: LZSS hook installed at RVA 0x%X (site=%p disp=%p post=%p)",
        LZSS_RVA, site, g_dispatcher, g_postHandler);



    // ── Install mid-function render hooks in FUN_0083a7a0 ─────────────────────
    // We do NOT hook the function entry. Instead we patch two 5-byte instructions
    // deep inside the function body:
    //   tsite+474: A1 78 2B D9 06  MOV EAX,[0x06D92B78]  → CALL PreLoopTramp
    //   tsite+493: A3 78 2B D9 06  MOV [0x06D92B78],EAX  → CALL PostLoopTramp
    // Both instructions are exactly 5 bytes, same as a CALL rel32.
    {
        BYTE* tsite = g_exeBase + TEXT_RVA;
        if (memcmp(tsite, TEXT_EXPECTED, TEXT_STLEN) != 0) {
            Log("TEXT-HOOK: unexpected prologue at RVA 0x%X — skip", TEXT_RVA);
        } else if (tsite[474] != 0xA1 || tsite[493] != 0xA3) {
            Log("TEXT-HOOK: unexpected opcodes at +474=%02X +493=%02X — skip",
                tsite[474], tsite[493]);
        } else {
            // Read relocated VA of [0x06D92B78] from the A1 instruction's imm32.
            g_textPtrGlobal = *reinterpret_cast<DWORD*>(tsite + 475);
            Log("TEXT-HOOK: g_textPtrGlobal=0x%08X (from tsite+475)", g_textPtrGlobal);

            BYTE* midMem = reinterpret_cast<BYTE*>(VirtualAlloc(
                nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (!midMem) {
                Log("TEXT-HOOK: VirtualAlloc failed gle=%lu", GetLastError());
            } else {
                // ── PreLoopTramp (replaces A1 at tsite+474) ──
                //   pushad
                //   call C_PreLoopHookText      ; may redirect [g_textPtrGlobal]
                //   popad
                //   mov eax, [g_textPtrGlobal]  ; re-execute original A1
                //   ret
                BYTE* pp = midMem;
                *pp++ = 0x60;                                                    // pushad
                *pp++ = 0xE8;
                *reinterpret_cast<DWORD*>(pp) =
                    (DWORD)(BYTE*)&C_PreLoopHookText - (DWORD)(pp+4); pp += 4;  // call
                *pp++ = 0x61;                                                    // popad
                *pp++ = 0xA1;
                *reinterpret_cast<DWORD*>(pp) = g_textPtrGlobal; pp += 4;       // mov eax,[global]
                *pp++ = 0xC3;                                                    // ret

                // ── PostLoopTramp (replaces A3 at tsite+493) ──
                //   mov [g_postLoopEAX], eax    ; save EAX before pushad clobbers it
                //   pushad
                //   call C_PostLoopHook         ; writes correct value to [global]
                //   popad
                //   ret                         ; original A3 NOT executed
                BYTE* pq = midMem + 64;
                *pq++ = 0xA3;
                *reinterpret_cast<DWORD*>(pq) = (DWORD)&g_postLoopEAX; pq += 4; // mov [g_postLoopEAX],eax
                *pq++ = 0x60;                                                    // pushad
                *pq++ = 0xE8;
                *reinterpret_cast<DWORD*>(pq) =
                    (DWORD)(BYTE*)&C_PostLoopHook - (DWORD)(pq+4); pq += 4;     // call
                *pq++ = 0x61;                                                    // popad
                *pq++ = 0xC3;                                                    // ret

                // ── Patch tsite+474 and tsite+493 → CALL trampolines ──
                BYTE* p474 = tsite + 474;
                BYTE* p493 = tsite + 493;
                DWORD oldProt;

                VirtualProtect(p474, 5, PAGE_EXECUTE_READWRITE, &oldProt);
                p474[0] = 0xE8;
                *reinterpret_cast<DWORD*>(p474+1) = (DWORD)midMem - (DWORD)(p474+5);
                VirtualProtect(p474, 5, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), p474, 5);

                VirtualProtect(p493, 5, PAGE_EXECUTE_READWRITE, &oldProt);
                p493[0] = 0xE8;
                *reinterpret_cast<DWORD*>(p493+1) = (DWORD)(midMem+64) - (DWORD)(p493+5);
                VirtualProtect(p493, 5, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), p493, 5);

                Log("TEXT-HOOK: mid-function hooks at RVA+474=%p RVA+493=%p tramp=%p",
                    p474, p493, midMem);
            }
        }
    }

    // ── Install mid-function render hooks in FUN_0083bcc0 (fullscreen narration) ──
    // Same engine global [0x06D92B78], same C hooks — different offsets.
    {
        BYTE* nsite = g_exeBase + NARR_RVA;
        Log("NARR-HOOK: probing RVA 0x%X at %p", NARR_RVA, nsite);
        BYTE probe[6] = {};
        bool readable = false;
        __try {
            memcpy(probe, nsite, 6);
            readable = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("NARR-HOOK: AV reading prologue at %p — page not readable, skip", nsite);
        }
        if (!readable) {
            // nothing — already logged above
        } else if (memcmp(probe, NARR_EXPECTED, 6) != 0) {
            Log("NARR-HOOK: unexpected prologue %02X %02X %02X %02X %02X %02X at RVA 0x%X — skip",
                probe[0],probe[1],probe[2],probe[3],probe[4],probe[5], NARR_RVA);
        } else if (nsite[NARR_PRE_OFF] != 0xA1 || nsite[NARR_POST_OFF] != 0xA3) {
            Log("NARR-HOOK: unexpected opcodes at +%d=%02X +%d=%02X — skip",
                NARR_PRE_OFF, nsite[NARR_PRE_OFF], NARR_POST_OFF, nsite[NARR_POST_OFF]);
        } else {
            // Grab the relocated VA from the A1 imm32 (should match TEXT_RVA's read).
            DWORD narrGlobal = *reinterpret_cast<DWORD*>(nsite + NARR_PRE_OFF + 1);
            if (!g_textPtrGlobal) g_textPtrGlobal = narrGlobal;
            Log("NARR-HOOK: textPtrGlobal=0x%08X narr_reads=0x%08X",
                g_textPtrGlobal, narrGlobal);

            BYTE* narrMem = reinterpret_cast<BYTE*>(VirtualAlloc(
                nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (!narrMem) {
                Log("NARR-HOOK: VirtualAlloc failed gle=%lu", GetLastError());
            } else {
                // PreLoopTramp — same structure as TEXT-HOOK's, but calls the
                // narration variant which truncates at first %N for centering.
                BYTE* pp = narrMem;
                *pp++ = 0x60;
                *pp++ = 0xE8;
                *reinterpret_cast<DWORD*>(pp) =
                    (DWORD)(BYTE*)&C_PreLoopHookNarr - (DWORD)(pp+4); pp += 4;
                *pp++ = 0x61;
                *pp++ = 0xA1;
                *reinterpret_cast<DWORD*>(pp) = g_textPtrGlobal; pp += 4;
                *pp++ = 0xC3;

                // PostLoopTramp
                BYTE* pq = narrMem + 64;
                *pq++ = 0xA3;
                *reinterpret_cast<DWORD*>(pq) = (DWORD)&g_postLoopEAX; pq += 4;
                *pq++ = 0x60;
                *pq++ = 0xE8;
                *reinterpret_cast<DWORD*>(pq) =
                    (DWORD)(BYTE*)&C_PostLoopHook - (DWORD)(pq+4); pq += 4;
                *pq++ = 0x61;
                *pq++ = 0xC3;

                BYTE* pPre  = nsite + NARR_PRE_OFF;
                BYTE* pPost = nsite + NARR_POST_OFF;
                DWORD oldProt;

                VirtualProtect(pPre, 5, PAGE_EXECUTE_READWRITE, &oldProt);
                pPre[0] = 0xE8;
                *reinterpret_cast<DWORD*>(pPre+1) = (DWORD)narrMem - (DWORD)(pPre+5);
                VirtualProtect(pPre, 5, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), pPre, 5);

                VirtualProtect(pPost, 5, PAGE_EXECUTE_READWRITE, &oldProt);
                pPost[0] = 0xE8;
                *reinterpret_cast<DWORD*>(pPost+1) = (DWORD)(narrMem+64) - (DWORD)(pPost+5);
                VirtualProtect(pPost, 5, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), pPost, 5);

                Log("NARR-HOOK: mid-function hooks at RVA+%d=%p RVA+%d=%p tramp=%p",
                    NARR_PRE_OFF, pPre, NARR_POST_OFF, pPost, narrMem);
            }
        }
    }

    // ── Choice render hook: patch +3 of FUN_008360A0 / 00836500 / 00836AC0 ──
    // Each function reads the script cursor at its prologue (`MOV EAX,
    // [DAT_06D92B78]` at offset +3, 5 bytes). We replace that with `JMP
    // dispatcher`. One shared dispatcher handles all 3 — they have identical
    // prologues so the patch and stolen instruction match.
    if (g_textPtrGlobal) {
        // Allocate one dispatcher and one post-handler — shared across all 3.
        BYTE* disp = reinterpret_cast<BYTE*>(VirtualAlloc(
            nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        BYTE* post = nullptr;
        if (disp) {
            post = reinterpret_cast<BYTE*>(VirtualAlloc(
                nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        }
        if (!disp || !post) {
            Log("CHOICE-HOOK: VirtualAlloc failed gle=%lu", GetLastError());
        } else {
            g_choicePostHandler = post;

            // Build the dispatcher. Lives at `disp`. Each hooked function's
            // patched JMP will point here.  Layout:
            //   60          pushad
            //   9C          pushfd
            //   55          push ebp                   ; arg for cdecl
            //   E8 imm32    call C_PreChoiceHook
            //   83 C4 04    add esp, 4
            //   9D          popfd
            //   61          popad
            //   A1 78 2B D9 06   mov eax, [g_textPtrGlobal]   (stolen — now
            //                    loads our_buf because the C handler swapped it)
            //   E9 imm32    jmp <hooked_fn + CHOICE_HOOK_OFF + CHOICE_STOLEN_LEN>
            //
            // The JMP target needs the per-function entry. Since we have 3
            // functions sharing one dispatcher, we instead make each hooked
            // site jump to a small per-function THUNK that calls the shared
            // dispatcher and then jumps back to its own +8.  Layout below.
            BYTE* d = disp;
            *d++ = 0x60;                                                       // pushad
            *d++ = 0x9C;                                                       // pushfd
            *d++ = 0x55;                                                       // push ebp
            *d++ = 0xE8;
            *reinterpret_cast<DWORD*>(d) =
                (DWORD)(BYTE*)&C_PreChoiceHook - (DWORD)(d + 4); d += 4;       // call pre
            *d++ = 0x83; *d++ = 0xC4; *d++ = 0x04;                             // add esp,4
            *d++ = 0x9D;                                                       // popfd
            *d++ = 0x61;                                                       // popad
            *d++ = 0xA1;                                                       // mov eax,[global]
            *reinterpret_cast<DWORD*>(d) = g_textPtrGlobal; d += 4;
            *d++ = 0xC3;                                                       // ret — caller (thunk) will jmp to fn+8

            // Build post-handler:
            //   60          pushad
            //   9C          pushfd
            //   E8 imm32    call C_PostChoiceHook
            //   9D          popfd
            //   61          popad
            //   FF 25 imm32 jmp dword ptr [g_choiceSavedRetAddr]
            BYTE* p = post;
            *p++ = 0x60;
            *p++ = 0x9C;
            *p++ = 0xE8;
            *reinterpret_cast<DWORD*>(p) =
                (DWORD)(BYTE*)&C_PostChoiceHook - (DWORD)(p + 4); p += 4;
            *p++ = 0x9D;
            *p++ = 0x61;
            *p++ = 0xFF; *p++ = 0x25;
            *reinterpret_cast<DWORD*>(p) = (DWORD)&g_choiceSavedRetAddr; p += 4;

            // Patch each function's +3..+7 with a JMP into a small per-fn
            // thunk that calls the shared dispatcher then jumps to its fn+8.
            // We allocate the thunks at disp+128 (one shared page).
            BYTE* thunkBase = disp + 128;
            int   thunkLen  = 11;     // call+jmp = 5+5 + slack
            int   thunksOk  = 0;

            for (int i = 0; i < CHOICE_FN_COUNT; ++i) {
                BYTE* fn   = g_exeBase + CHOICE_FN_RVAS[i];
                BYTE* site = fn + CHOICE_HOOK_OFF;
                // Validate: opcode must be A1 (mov eax, [imm32]) and the
                // imm32 must equal g_textPtrGlobal (the runtime-rebased
                // address — the loader rewrote the instruction's operand
                // when cc.exe was loaded at base 0x006A0000 instead of
                // its preferred 0x00400000-or-whatever).
                if (site[0] != 0xA1 ||
                    *reinterpret_cast<DWORD*>(site + 1) != g_textPtrGlobal) {
                    Log("CHOICE-HOOK: unexpected bytes at RVA 0x%X+%d "
                        "(site=%p) got=%02X %02X %02X %02X %02X "
                        "(want A1 + imm32=0x%08X) — skip",
                        CHOICE_FN_RVAS[i], CHOICE_HOOK_OFF, site,
                        site[0], site[1], site[2], site[3], site[4],
                        g_textPtrGlobal);
                    continue;
                }

                // Per-fn thunk:
                //   E8 imm32    call <shared dispatcher>   ; runs pre + stolen MOV EAX
                //   E9 imm32    jmp  <fn + CHOICE_HOOK_OFF + CHOICE_STOLEN_LEN>
                BYTE* t = thunkBase + i * 16;
                *t++ = 0xE8;
                *reinterpret_cast<DWORD*>(t) = (DWORD)disp - (DWORD)(t + 4); t += 4;
                *t++ = 0xE9;
                *reinterpret_cast<DWORD*>(t) =
                    (DWORD)(fn + CHOICE_HOOK_OFF + CHOICE_STOLEN_LEN)
                    - (DWORD)(t + 4); t += 4;

                // Patch site with `JMP <thunk>`
                BYTE patch[CHOICE_STOLEN_LEN];
                patch[0] = 0xE9;
                *reinterpret_cast<DWORD*>(patch + 1) =
                    (DWORD)(thunkBase + i * 16) - (DWORD)(site + 5);
                DWORD oldProt;
                if (!VirtualProtect(site, CHOICE_STOLEN_LEN,
                                     PAGE_EXECUTE_READWRITE, &oldProt)) {
                    Log("CHOICE-HOOK: VirtualProtect failed @ %p gle=%lu",
                        site, GetLastError());
                    continue;
                }
                memcpy(site, patch, CHOICE_STOLEN_LEN);
                DWORD tmp;
                VirtualProtect(site, CHOICE_STOLEN_LEN, oldProt, &tmp);
                FlushInstructionCache(GetCurrentProcess(), site, CHOICE_STOLEN_LEN);
                ++thunksOk;
                Log("CHOICE-HOOK: patched RVA 0x%X+%d -> thunk %p",
                    CHOICE_FN_RVAS[i], CHOICE_HOOK_OFF, thunkBase + i * 16);
            }
            Log("CHOICE-HOOK: %d/%d functions hooked (disp=%p post=%p)",
                thunksOk, CHOICE_FN_COUNT, disp, post);
        }
    }

    // ── Centering fix: patch CALL FUN_0089a0e0 inside FUN_0083bcc0 ────────────
    // The centering helper is called right after FUN_0081d8c0 (line 2315 in the
    // Ghidra decompilation) before the character loop starts.  We replace just the
    // 4-byte relative displacement in that CALL instruction so it calls
    // C_CenteringHook instead, which temporarily truncates the buffer at the first
    // %N so the engine centres only the first line's worth of text.
    if (g_textPtrGlobal) {
        // Derive runtime piVar10 base from the text-ptr global we already know.
        constexpr DWORD GHIDRA_TEXT_PTR = 0x06D92B78u;
        constexpr DWORD GHIDRA_PIVAR10  = 0x06D922D0u;
        DWORD delta = g_textPtrGlobal - GHIDRA_TEXT_PTR;
        g_piVar10Base = GHIDRA_PIVAR10 + delta;
        Log("NARR-CENTER: piVar10 base=%p", g_piVar10Base);

        BYTE* nsite = g_exeBase + NARR_RVA;
        DWORD targetVA = (DWORD)(g_exeBase + NARR_A0E0_RVA);
        g_origCenteringCallVA = targetVA;

        // Scan FUN_0083bcc0 body starting just past our A3 hook point for the
        // first CALL FUN_0089a0e0.  This is the centering call at line 2315.
        BYTE* cenSite = nullptr;
        for (int off = NARR_POST_OFF + 5; off < 3000; off++) {
            if (nsite[off] != 0xE8) continue;
            DWORD disp   = *reinterpret_cast<DWORD*>(nsite + off + 1);
            DWORD callVA = (DWORD)(nsite + off + 5) + disp;
            if (callVA == targetVA) {
                cenSite = nsite + off;
                Log("NARR-CENTER: CALL FUN_0089a0e0 at FUN_0083bcc0+%d (%p)", off, cenSite);
                break;
            }
        }

        if (cenSite) {
            DWORD newDisp = (DWORD)(BYTE*)&C_CenteringHook - (DWORD)(cenSite + 5);
            DWORD oldProt;
            VirtualProtect(cenSite + 1, 4, PAGE_EXECUTE_READWRITE, &oldProt);
            *reinterpret_cast<DWORD*>(cenSite + 1) = newDisp;
            VirtualProtect(cenSite + 1, 4, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), cenSite, 5);
            Log("NARR-CENTER: centering call patched → C_CenteringHook");
        } else {
            Log("NARR-CENTER: CALL FUN_0089a0e0 not found — centering not patched");
        }
    }


    return 0;
}

// Prefix map: truncated-EN-PatchScriptBuffer-might-have-written → full EN.
// Built from translations.tsv at startup using the same FitToSlot() that
// PatchScriptBuffer uses at decompress time, so every form the patcher
// could have written maps back to the full EN. Consumed by the choice
// entry hook (BuildChoiceOurBuf) to recover untruncated translations.
std::unordered_map<std::string, std::string> g_truncatedEnToFullEn;

void BuildTruncatedEnMap() {
    g_truncatedEnToFullEn.clear();
    if (g_translations.empty()) return;
    for (const auto& kv : g_translations) {
        const std::string& jp = kv.first;
        const std::string& en = kv.second;
        if (!translator_logic::IsChoiceLikeJp(jp)) continue;   // size 10..14
        if (en.size() <= jp.size()) continue;                  // not truncated
        // Slot in heap is jp.size() plus however many trailing nulls fit
        // before the next non-null metadata byte. [jp_len .. jp_len+6]
        // covers everything PatchScriptBuffer has been observed to write.
        size_t maxSlot = jp.size() + 7;
        if (maxSlot > en.size()) maxSlot = en.size();
        for (size_t slot = jp.size(); slot < maxSlot; ++slot) {
            std::string trunc = translator_logic::FitToSlot(en, slot);
            if (trunc.empty() || trunc.size() >= en.size()) continue;
            // First-write wins on collisions — needs two different JPs
            // whose ENs share an identical word-boundary truncation.
            g_truncatedEnToFullEn.emplace(std::move(trunc), en);
        }
    }
    Log("PREFIX-MAP: built %zu truncated-EN -> full-EN mappings",
        g_truncatedEnToFullEn.size());
}

}  // anonymous namespace

// ── Public API ────────────────────────────────────────────────────────────

void TranslatorInit() {
    g_exeBase = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    Log("Translator: cc.exe base=%p — RUNTIME SCRIPT PATCHER (sn.bin in-place)",
        g_exeBase);

    // Install font hook immediately — gdi32.dll is already loaded at this point,
    // and the game creates its font handles before the packer finishes.
    InstallCreateFontAHook();
    InstallGlyphOutlineHooks();

    // (Choice font scale is patched from HookInstallerThread once the
    // packer has unpacked .rdata — the constant we want lives in a section
    // that's still zeroed at DllMain time.)

    g_translationsLoaded = LoadTranslations();
    if (g_translationsLoaded) BuildTruncatedEnMap();

    HANDLE h = CreateThread(nullptr, 0, HookInstallerThread, nullptr, 0, nullptr);
    if (h) {
        CloseHandle(h);
        Log("HOOK: installer thread spawned");
    }

}

// Used by BuildChoiceOurBuf when the entry hook sees space-padded
// truncated EN at a slot (PatchScriptBuffer wrote it there at decompress
// time) and needs to recover the full untruncated EN.
bool Translator_PrefixLookup(const std::string& maybe_truncated_en,
                              std::string& full_en_out) {
    auto it = g_truncatedEnToFullEn.find(maybe_truncated_en);
    if (it == g_truncatedEnToFullEn.end()) return false;
    full_en_out = it->second;
    return true;
}

void TranslatorShutdown() {
    Log("Translator: shutdown — decompress_calls=%d sn_bin_patched=%d "
        "jp_hits=%d skipped_too_long=%d render_calls=%d render_subs=%d",
        g_decompressCalls, g_snBinPatched, g_jpHits, g_jpTooLong,
        g_renderCalls, g_renderSubs);
}
