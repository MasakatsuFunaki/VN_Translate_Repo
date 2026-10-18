// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator.h"
#include "translator_logic.h"
#include "text_display_abi.h"
#include "iat_hook.h"
#include "log.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>
#include <vector>

// ==========================================================================
// SakuraNoUta runtime translator (BGI / Buriko / Ethornell engine).
// Hook target: RVA 0x033900 -- ECX+EDX register params, eleven stack dwords,
// caller-cleans.  text_display_abi.h reproduces the convention.
// ==========================================================================

// ── Hook target ────────────────────────────────────────────────────
static const DWORD TEXT_DISPLAY_RVA = 0x00033900;
static const SIZE_T STOLEN_LEN      = 6;
static const BYTE   EXPECTED_BYTES[STOLEN_LEN] = {
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8
};

// ── Line pitch ──────────────────────────────────────────────────────
// English needs 4 lines; stock pitch only fits 3.  Substituted per call
// because the engine builds the value from font metrics -- no constant to
// patch.  The backlog gets the same treatment (26 px fits 4 rows in its clip).
static const int DIALOGUE_LINE_PITCH_STOCK = 0x2A;   // 42 px, what BGI passes
static const int DIALOGUE_LINE_PITCH       = 30;     // what the box gets
static const int NAME_PLATE_LINE_PITCH     = 0x18;   // 24 px, left alone
static const int HISTORY_LINE_PITCH_STOCK  = 0x28;   // 40 px, what BGI passes
static const int HISTORY_LINE_PITCH        = 26;     // what the backlog gets

// ── The renderer's rectangle array ──────────────────────────────────
// Callers allocate 4 rects/byte from the JP buffer length; the hook swaps in
// longer English, so the array overruns.  Patching the immediate 0x10 -> 0x80
// gives 8x room.  On mismatch the patch is skipped and the budget stays at 4.
static const DWORD RECT_ARRAY_SITES[] = { 0x00033232, 0x00029E9F };
static const BYTE  RECT_ARRAY_EXPECTED[5] = { 0xBA, 0x10, 0x00, 0x00, 0x00 };
static const BYTE  RECT_ARRAY_PATCHED     = 0x80;
static const size_t RECT_ARRAY_GROWTH     = RECT_ARRAY_PATCHED /
                                            RECT_ARRAY_EXPECTED[1];

static size_t g_rectsPerWireByte = translator_logic::RECTS_PER_WIRE_BYTE;

static bool PatchRectArraySites(BYTE* base) {
    const size_t n = sizeof(RECT_ARRAY_SITES) / sizeof(*RECT_ARRAY_SITES);
    for (size_t i = 0; i < n; ++i) {
        BYTE* site = base + RECT_ARRAY_SITES[i];
        if (memcmp(site, RECT_ARRAY_EXPECTED, sizeof(RECT_ARRAY_EXPECTED)) != 0) {
            Log("RectArray: RVA 0x%X is not the allocation this expects -- got "
                "%02X %02X %02X %02X %02X; leaving every site alone",
                RECT_ARRAY_SITES[i], site[0], site[1], site[2], site[3],
                site[4]);
            return false;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        BYTE* site = base + RECT_ARRAY_SITES[i] + 1;
        DWORD old = 0;
        if (!VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &old)) {
            Log("RectArray: VirtualProtect FAILED at RVA 0x%X",
                RECT_ARRAY_SITES[i]);
            return false;
        }
        *site = RECT_ARRAY_PATCHED;
        DWORD dummy = 0;
        VirtualProtect(site, 1, old, &dummy);
        FlushInstructionCache(GetCurrentProcess(), site, 1);
        Log("RectArray: RVA 0x%X rectangles per byte 0x%02X -> 0x%02X",
            RECT_ARRAY_SITES[i], RECT_ARRAY_EXPECTED[1], RECT_ARRAY_PATCHED);
    }
    return true;
}

// ── Surfaces ────────────────────────────────────────────────────────
// The line advance identifies the surface: 0x2A = dialogue panel, 0x18 = name
// plate, 0x28 = history.  Other advances come from script font-size changes;
// those are derived (width/lines from advance) and only take one-line values.
enum class SurfaceKind {
    Measured,   // a surface a screenshot fixed: the panel, the plate, the backlog
    Derived,    // width and lines derived from the advance; one short line only
    Refused     // no room for a line the engine might re-break
};

static translator_logic::Surface SurfaceForPitch(intptr_t engine_pitch,
                                                 SurfaceKind& kind) {
    kind = SurfaceKind::Measured;
    if (engine_pitch == HISTORY_LINE_PITCH_STOCK)
        return translator_logic::HISTORY_WINDOW;
    if (engine_pitch == DIALOGUE_LINE_PITCH_STOCK ||
        engine_pitch == NAME_PLATE_LINE_PITCH)
        return translator_logic::DIALOGUE_PANEL;
    const size_t pitch = engine_pitch > 0 ? static_cast<size_t>(engine_pitch) : 0;
    if (translator_logic::LinesAtPitch(pitch) < 2) {
        kind = SurfaceKind::Refused;
        return translator_logic::DIALOGUE_PANEL;
    }
    kind = SurfaceKind::Derived;
    translator_logic::Surface s;
    s.lines = 1;
    return s;
}

// Surface id for the line budget: layout_state alone is shared across font
// sizes, so the advance is mixed in to keep separate surfaces separate.
static uintptr_t SurfaceId(const void* layout_state, intptr_t engine_pitch) {
    return reinterpret_cast<uintptr_t>(layout_state) ^
           (static_cast<uintptr_t>(engine_pitch) * 0x9E3779B9u);
}

// ── Module / state ──────────────────────────────────────────────────

static BYTE* g_exeBase = nullptr;
static BYTE* g_trampoline = nullptr;

static translator_logic::TranslationMap g_translations;
static bool g_translationsLoaded = false;


static std::atomic<int> g_walkerCalls { 0 };
static std::atomic<int> g_hits        { 0 };
static std::atomic<int> g_misses      { 0 };
static std::atomic<int> g_passthrough { 0 };
static std::atomic<int> g_renderFaults { 0 };
static std::atomic<int> g_boxOverflows { 0 };
static std::atomic<int> g_pitchSubst   { 0 };
// Logged once per surface the first time its advance is substituted.
static std::atomic<int> g_panelPitchLogged   { 0 };
static std::atomic<int> g_historyPitchLogged { 0 };
static std::atomic<int> g_unmeasuredSurface { 0 };
static std::atomic<int> g_rectOverruns { 0 };

// Per-box line count, surviving across the chain of calls that fill one box.
static translator_logic::BoxLineBudget g_boxLines;

// ── TSV loading ─────────────────────────────────────────────────────

static bool LoadTranslations() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash) wcscpy(slash + 1, L"translations.tsv");
    else       wcscpy(exePath, L"translations.tsv");

    char pathA[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, exePath, -1,
                        pathA, sizeof(pathA), nullptr, nullptr);

    // Collect characters that have no CP932 form, so the log can name them.
    std::vector<translator_logic::UnmappableChar> unmappable;
    std::vector<std::string> markup_rows;
    bool ok = translator_logic::LoadTsvFile(pathA, g_translations, &unmappable,
                                            &markup_rows);
    if (!ok) {
        Log("Translator: translations.tsv NOT FOUND at %s -- JP passthrough", pathA);
        return false;
    }
    Log("Translator: loaded %zu entries from translations.tsv", g_translations.size());

    // Log rows whose English carried engine markup (stripped at load time).
    const size_t kMaxMarkup = 20;
    for (size_t i = 0; i < markup_rows.size() && i < kMaxMarkup; ++i)
        Log("Translator: ruby markup removed from the English for '%s'",
            markup_rows[i].c_str());
    if (markup_rows.size() > kMaxMarkup)
        Log("Translator: %zu further rows carried ruby markup",
            markup_rows.size() - kMaxMarkup);
    if (!markup_rows.empty())
        Log("Translator: %zu of %zu English values carried ruby markup",
            markup_rows.size(), g_translations.size());

    const size_t kMaxNamed = 50;
    for (size_t i = 0; i < unmappable.size() && i < kMaxNamed; ++i)
        Log("Translator: no CP932 form for U+%04X in the English for '%s' -- drawn as '?'",
            unmappable[i].codepoint, unmappable[i].jp_key.c_str());
    if (unmappable.size() > kMaxNamed)
        Log("Translator: %zu further characters had no CP932 form",
            unmappable.size() - kMaxNamed);

    // Log values that still draw a fullwidth glyph (fold table gaps).
    size_t wide_rows = 0;
    for (const auto& kv : g_translations) {
        if (!translator_logic::HasWideCharacter(kv.second)) continue;
        if (wide_rows < 20)
            Log("Translator: English value draws a fullwidth glyph: '%s'",
                translator_logic::Cp932ToUtf8(kv.second).c_str());
        ++wide_rows;
    }
    if (wide_rows)
        Log("Translator: %zu of %zu English values still draw a fullwidth glyph",
            wide_rows, g_translations.size());
    return true;
}

// ── Word wrap ───────────────────────────────────────────────────────
// The engine handles 0x0A as a line break, so inserting 0x0A at word
// boundaries wraps English safely.  Measurements in translator_logic.h.

// ── SEH exception capture ────────────────────────────────────────────

static volatile DWORD g_exCode      = 0;
static volatile DWORD g_exFaultAddr = 0;
static volatile DWORD g_exEip       = 0;
static volatile DWORD g_exEsp       = 0;
static volatile DWORD g_exEbp       = 0;
static volatile DWORD g_exEax       = 0;
static volatile DWORD g_exEcx       = 0;
static volatile DWORD g_exEdx       = 0;

static int CaptureException(EXCEPTION_POINTERS* ep) {
    g_exCode      = ep->ExceptionRecord->ExceptionCode;
    g_exFaultAddr = (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
                    ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    g_exEip = ep->ContextRecord->Eip;
    g_exEsp = ep->ContextRecord->Esp;
    g_exEbp = ep->ContextRecord->Ebp;
    g_exEax = ep->ContextRecord->Eax;
    g_exEcx = ep->ContextRecord->Ecx;
    g_exEdx = ep->ContextRecord->Edx;
    return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ dtors -- required for __try/__except under MSVC.
static bool CallTrampolineSEH(
    DWORD ecx, DWORD edx,
    void* p1, char* text, int* p3, unsigned short* p4, int p5, char* p6,
    int* p7, int p8, int p9, int p10, int p11, DWORD* out_result)
{
    __try {
        DWORD r = CallTextDisplayOriginal(ecx, edx, p1, text, p3, p4, p5, p6,
                                          p7, p8, p9, p10, p11);
        if (out_result) *out_result = r;
        return true;
    } __except(CaptureException(GetExceptionInformation())) {
        return false;
    }
}

// ── The engine's own ceiling on one render ───────────────────────────
// The render fills a segment list (linked at +0x44 off the header) with no
// count check, so we walk it after each call to detect rectangle overruns.
static const size_t SEGMENT_NEXT_OFFSET = 0x44;
static const int    SEGMENT_WALK_LIMIT  = 8192;

static int CountSegmentsSEH(const void* header) {
    int n = 0;
    __try {
        const char* node = *reinterpret_cast<const char* const*>(
            static_cast<const char*>(header) + SEGMENT_NEXT_OFFSET);
        while (node && n < SEGMENT_WALK_LIMIT) {
            ++n;
            node = *reinterpret_cast<const char* const*>(
                node + SEGMENT_NEXT_OFFSET);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}

// Log segments built vs. array capacity; detect overruns.
static void LogSegmentBudget(int call_n, const void* header,
                             const char* wire, size_t drawn_bytes) {
    const size_t wire_bytes = wire ? strlen(wire) : 0;
    const int segments = CountSegmentsSEH(header);
    const size_t capacity = wire_bytes * g_rectsPerWireByte;
    Log("HOOK[%d]: segments=%d of %zu (wire %zu bytes, drawn %zu)",
        call_n, segments, capacity, wire_bytes, drawn_bytes);
    if (segments > 0 && static_cast<size_t>(segments) > capacity) {
        g_rectOverruns.fetch_add(1, std::memory_order_relaxed);
        Log("HOOK[%d]: RECTANGLE ARRAY OVERRUN -- %d written into room for "
            "%zu; the heap is damaged and the process will die on some later "
            "allocation", call_n, segments, capacity);
    }
}

// ── First-chance exceptions inside the engine ────────────────────────
// A patch the engine does not accept kills the process without a word: the
// SEH around the trampoline only covers the render.  This names the faulting
// instruction and passes the exception on, so it changes nothing.  Faults
// raised by this DLL's own guarded reads are skipped by the address test.
static DWORD g_exeSize = 0;
static std::atomic<int> g_engineFaults { 0 };

static LONG CALLBACK LogFirstChance(EXCEPTION_POINTERS* ep) {
    const DWORD eip = ep->ContextRecord->Eip;
    const DWORD base = reinterpret_cast<DWORD>(g_exeBase);
    if (g_exeSize && eip >= base && eip < base + g_exeSize) {
        const int n = g_engineFaults.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 16)
            Log("EngineFault[%d]: code=0x%08X eip=0x%08X (RVA 0x%X) "
                "fault=0x%08X eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X",
                n, ep->ExceptionRecord->ExceptionCode, eip, eip - base,
                static_cast<DWORD>(ep->ExceptionRecord->ExceptionInformation[1]),
                ep->ContextRecord->Eax, ep->ContextRecord->Ecx,
                ep->ContextRecord->Edx, ep->ContextRecord->Esp);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void LogTrampolineException(int call_n) {
    g_renderFaults.fetch_add(1, std::memory_order_relaxed);
    Log("HOOK[%d]: EXCEPTION code=0x%08X eip=0x%08X (RVA 0x%X) fault=0x%08X esp=0x%08X ebp=0x%08X eax=0x%08X ecx=0x%08X edx=0x%08X",
        call_n, g_exCode, g_exEip, g_exEip - (DWORD)g_exeBase,
        g_exFaultAddr, g_exEsp, g_exEbp, g_exEax, g_exEcx, g_exEdx);
}

// ── Counters ─────────────────────────────────────────────────────────
// Printed periodically because the process is usually killed, skipping detach.
static const int STATS_EVERY = 100;

// Defined below, next to the GDI counters it also reports.
static void LogCounters(const char* when);

// ── Re-entrancy guard ────────────────────────────────────────────────
// The renderer's PeekMessage loop can trigger a nested call; skip it so
// the outer call's shared GDI state stays consistent.
static thread_local int t_hookDepth = 0;

// ── The hook ────────────────────────────────────────────────────────

static DWORD __cdecl HookedTextDisplay(
    DWORD ecx, DWORD edx,
    void* p1, char* text, int* p3, unsigned short* p4, int p5, char* p6,
    int* p7, int p8, int p9, int p10, int p11)
{
    DWORD result = 0;
    int call_n = g_walkerCalls.fetch_add(1, std::memory_order_relaxed) + 1;

    // Depth falls and counters print on every exit path.
    struct CallScope {
        int  call_n;
        ~CallScope() {
            --t_hookDepth;
            if (call_n % STATS_EVERY == 0) LogCounters("periodic");
        }
    } scope { call_n };
    ++t_hookDepth;

    // Substitute the line pitch unconditionally -- a box drawn by a chain of
    // calls needs the same advance on every segment or the lines come out uneven.
    const intptr_t pitch_in = reinterpret_cast<intptr_t>(p6);
    if (pitch_in == DIALOGUE_LINE_PITCH_STOCK ||
        pitch_in == HISTORY_LINE_PITCH_STOCK) {
        const bool panel = pitch_in == DIALOGUE_LINE_PITCH_STOCK;
        const int substituted = panel ? DIALOGUE_LINE_PITCH : HISTORY_LINE_PITCH;
        p6 = reinterpret_cast<char*>(static_cast<intptr_t>(substituted));
        g_pitchSubst.fetch_add(1, std::memory_order_relaxed);
        std::atomic<int>& announced =
            panel ? g_panelPitchLogged : g_historyPitchLogged;
        if (announced.fetch_add(1, std::memory_order_relaxed) == 0)
            Log("HOOK[%d]: line pitch %d -> %d for the %s, so it holds four "
                "lines; the name plate keeps its own",
                call_n, (int)pitch_in, substituted,
                panel ? "dialogue panel" : "backlog entry");
    }

    SurfaceKind surface_kind = SurfaceKind::Measured;
    translator_logic::Surface surface = SurfaceForPitch(pitch_in, surface_kind);
    const bool measured_surface = surface_kind != SurfaceKind::Refused;
    // Draw budget: the rectangle array the caller sized from the wire buffer.
    surface.max_bytes = translator_logic::DrawBudget(
        text ? strlen(text) : 0, g_rectsPerWireByte);
    const size_t surface_lines = surface.lines;

    Log("HOOK[%d]: ENTER depth=%d p1=%p text=%p p3=%p p4=%p p5=0x%X p6=%p (engine %p) p7=%p p8=0x%X p9=0x%X p10=0x%X p11=0x%X",
        call_n, t_hookDepth, p1, text, p3, p4, p5, p6, (void*)pitch_in, p7, p8, p9, p10, p11);
    Log("HOOK[%d]: ECX_entry=0x%08X EDX_entry=0x%08X", call_n, ecx, edx);

    if (text) {
        const unsigned char* t = (const unsigned char*)text;
        Log("HOOK[%d]: text_bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            call_n, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11]);
    } else {
        Log("HOOK[%d]: text=NULL", call_n);
    }

    // Re-entrant call: skip render so FUN_00433900 is never nested.
    if (t_hookDepth > 1) {
        g_passthrough.fetch_add(1, std::memory_order_relaxed);
        Log("HOOK[%d]: re-entrant depth=%d -- skipping render", call_n, t_hookDepth);
        return result;
    }

    if (!g_translationsLoaded || !text || !measured_surface) {
        g_passthrough.fetch_add(1, std::memory_order_relaxed);
        if (!measured_surface && g_translationsLoaded && text) {
            const int n = g_unmeasuredSurface.fetch_add(
                              1, std::memory_order_relaxed);
            if (n < 8)
                Log("HOOK[%d]: line advance 0x%X leaves the panel one line, "
                    "which the engine could re-break -- it draws its own text "
                    "here", call_n, (unsigned)pitch_in);
        }
        Log("HOOK[%d]: passthrough -- BEFORE_TRAMPOLINE", call_n);
        bool ok = CallTrampolineSEH(ecx, edx, p1, text, p3, p4, p5, p6,
                                    p7, p8, p9, p10, p11, &result);
        if (ok) {
            Log("HOOK[%d]: AFTER_TRAMPOLINE_OK", call_n);
            LogSegmentBudget(call_n, p1, text, text ? strlen(text) : 0);
        } else  LogTrampolineException(call_n);
        return result;
    }

    // Substitution logic lives in translator_logic; here we handle counters,
    // logging, and the guarded call into the engine.
    // box_id names the drawing context, not an individual box -- consecutive
    // boxes share one; an unprefixed segment starts a fresh count.
    const uintptr_t box_id = SurfaceId(p4, pitch_in);
    translator_logic::Substitution sub;
    translator_logic::SubstitutionStatus status =
        translator_logic::BuildSubstitution(text, g_translations, g_boxLines,
                                            box_id, surface, sub);

    // Derived surfaces only take one-line values that fit the width; anything
    // else is left to the engine.
    if (surface_kind == SurfaceKind::Derived &&
        (status != translator_logic::SubstitutionStatus::Substituted ||
         sub.truncated ||
         !translator_logic::FitsUnmeasuredSurface(
              sub.bytes, static_cast<size_t>(pitch_in)))) {
        g_passthrough.fetch_add(1, std::memory_order_relaxed);
        const int n = g_unmeasuredSurface.fetch_add(1, std::memory_order_relaxed);
        if (n < 8)
            Log("HOOK[%d]: line advance 0x%X holds %zu columns on %zu lines; "
                "this value is not one line of them -- the engine draws its "
                "own text here", call_n, (unsigned)pitch_in,
                translator_logic::ColumnsAtPitch(static_cast<size_t>(pitch_in)),
                translator_logic::LinesAtPitch(static_cast<size_t>(pitch_in)));
        Log("HOOK[%d]: passthrough -- BEFORE_TRAMPOLINE", call_n);
        bool ok = CallTrampolineSEH(ecx, edx, p1, text, p3, p4, p5, p6,
                                    p7, p8, p9, p10, p11, &result);
        if (ok) {
            Log("HOOK[%d]: AFTER_TRAMPOLINE_OK", call_n);
            LogSegmentBudget(call_n, p1, text, strlen(text));
        } else  LogTrampolineException(call_n);
        return result;
    }

    if (status == translator_logic::SubstitutionStatus::EmptyKey) {
        g_passthrough.fetch_add(1, std::memory_order_relaxed);
        Log("HOOK[%d]: empty text -- BEFORE_TRAMPOLINE", call_n);
        bool ok = CallTrampolineSEH(ecx, edx, p1, text, p3, p4, p5, p6,
                                    p7, p8, p9, p10, p11, &result);
        if (ok) {
            Log("HOOK[%d]: AFTER_TRAMPOLINE_OK", call_n);
            LogSegmentBudget(call_n, p1, text, strlen(text));
        } else  LogTrampolineException(call_n);
        return result;
    }

    if (status == translator_logic::SubstitutionStatus::NotFound) {
        g_misses.fetch_add(1, std::memory_order_relaxed);
        // Miss: draw the engine's own buffer (minus furigana if present,
        // which would overlap the line above at this pitch).
        char* to_draw = text;
        if (!sub.bytes.empty()) {
            to_draw = const_cast<char*>(sub.bytes.c_str());
            Log("HOOK[%d]: furigana removed -- it would draw over the line "
                "above at this pitch", call_n);
        }
        Log("HOOK[%d]: miss -- box %p lines=%zu/%zu (+%zu drawn by the engine)"
            " tail=%zu%s -- BEFORE_TRAMPOLINE",
            call_n, p4, sub.box_lines_after, surface_lines,
            sub.lines, sub.tail, sub.continues ? " cont" : "");
        bool ok = CallTrampolineSEH(ecx, edx, p1, to_draw, p3, p4, p5, p6,
                                    p7, p8, p9, p10, p11, &result);
        if (ok) {
            Log("HOOK[%d]: AFTER_TRAMPOLINE_OK", call_n);
            LogSegmentBudget(call_n, p1, text, strlen(to_draw));
        } else  LogTrampolineException(call_n);
        return result;
    }

    g_hits.fetch_add(1, std::memory_order_relaxed);

    // Log when the English was truncated or dropped -- only trace of overflow.
    if (sub.truncated || sub.dropped) {
        g_boxOverflows.fetch_add(1, std::memory_order_relaxed);
        if (sub.dropped)
            Log("HOOK[%d]: subst DROPPED -- box %p already holds %zu of %zu "
                "lines, nothing of this segment can be drawn",
                call_n, p4, sub.box_lines_before, surface_lines);
        else
            Log("HOOK[%d]: subst TRUNCATED to %zu lines -- box %p holds %zu of "
                "%zu lines with this segment%s",
                call_n, sub.lines, p4, sub.box_lines_after, surface_lines,
                sub.more_follows ? " and another follows it" : "");
    }

    Log("HOOK[%d]: HIT subst='%s' tail=%zu box=%p lines=%zu/%zu%s -- BEFORE_TRAMPOLINE",
        call_n, sub.bytes.c_str(), sub.tail, p4,
        sub.box_lines_after, surface_lines,
        sub.continues ? " cont" : "");
    bool ok = CallTrampolineSEH(ecx, edx, p1,
                                const_cast<char*>(sub.bytes.c_str()),
                                p3, p4, p5, p6, p7, p8, p9, p10, p11, &result);
    if (ok) {
        Log("HOOK[%d]: AFTER_TRAMPOLINE_OK", call_n);
        LogSegmentBudget(call_n, p1, text, sub.bytes.size());
    } else  LogTrampolineException(call_n);
    return result;
}

// ── Inline trampoline install ───────────────────────────────────────

static bool InstallTextDisplayHook(BYTE* base) {
    BYTE* site = base + TEXT_DISPLAY_RVA;
    if (memcmp(site, EXPECTED_BYTES, STOLEN_LEN) != 0) {
        Log("Hook: prologue mismatch at RVA 0x%X -- got %02X %02X %02X %02X %02X %02X",
            TEXT_DISPLAY_RVA,
            site[0], site[1], site[2], site[3], site[4], site[5]);
        return false;
    }

    g_trampoline = (BYTE*)VirtualAlloc(nullptr, 64,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampoline) { Log("Hook: VirtualAlloc FAILED"); return false; }

    memcpy(g_trampoline, EXPECTED_BYTES, STOLEN_LEN);
    BYTE* p = g_trampoline + STOLEN_LEN;
    *p++ = 0xE9;
    *(DWORD*)p = (DWORD)(site + STOLEN_LEN) - (DWORD)(p + 4);

    // Wire both halves before patching; the engine can call as soon as the JMP lands.
    g_textDisplayTrampoline = g_trampoline;
    g_textDisplayHandler    = &HookedTextDisplay;

    DWORD oldProtect;
    if (!VirtualProtect(site, STOLEN_LEN, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("Hook: VirtualProtect FAILED"); return false;
    }
    site[0] = 0xE9;
    *(DWORD*)(site + 1) = (DWORD)(BYTE*)&TextDisplayHookEntry - (DWORD)(site + 5);
    for (size_t i = 5; i < STOLEN_LEN; ++i) site[i] = 0x90;
    DWORD dummy;
    VirtualProtect(site, STOLEN_LEN, oldProtect, &dummy);
    FlushInstructionCache(GetCurrentProcess(), site, STOLEN_LEN);

    Log("Hook: installed at RVA 0x%X (site=%p, trampoline=%p, hookFn=%p)",
        TEXT_DISPLAY_RVA, site, g_trampoline, (void*)&TextDisplayHookEntry);
    return true;
}

// ── Init / shutdown ─────────────────────────────────────────────────

// ── GDI TextOut* IAT hooks (diagnostic) ──────────────────────────────
// IAT-hook TextOutA/W and ExtTextOutA/W to log GDI text calls.
typedef BOOL (WINAPI *PFN_TextOutA)(HDC, int, int, LPCSTR, int);
typedef BOOL (WINAPI *PFN_TextOutW)(HDC, int, int, LPCWSTR, int);
typedef BOOL (WINAPI *PFN_ExtTextOutA)(HDC, int, int, UINT, const RECT*, LPCSTR, UINT, const INT*);
typedef BOOL (WINAPI *PFN_ExtTextOutW)(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);

static PFN_TextOutA    g_origTextOutA    = nullptr;
static PFN_TextOutW    g_origTextOutW    = nullptr;
static PFN_ExtTextOutA g_origExtTextOutA = nullptr;
static PFN_ExtTextOutW g_origExtTextOutW = nullptr;

static std::atomic<int> g_textOutACalls   {0};
static std::atomic<int> g_textOutWCalls   {0};
static std::atomic<int> g_extTextOutACalls{0};
static std::atomic<int> g_extTextOutWCalls{0};

static BOOL WINAPI HookedTextOutA(HDC hdc, int x, int y, LPCSTR lpStr, int c) {
    int n = g_textOutACalls.fetch_add(1, std::memory_order_relaxed) + 1;
    DWORD retAddr = (DWORD)_ReturnAddress();
    DWORD textRva = retAddr - (DWORD)g_exeBase;
    if (lpStr && c > 0) {
        const unsigned char* p = (const unsigned char*)lpStr;
        char buf[64] = {0};
        int blen = c < 16 ? c : 16;
        for (int i = 0; i < blen; ++i)
            sprintf(buf + i*3, "%02X ", p[i]);
        Log("TextOutA[%d]: ret=0x%08X (RVA 0x%X) hdc=%p x=%d y=%d c=%d bytes: %s",
            n, retAddr, textRva, hdc, x, y, c, buf);
    } else {
        Log("TextOutA[%d]: ret=0x%08X (RVA 0x%X) hdc=%p x=%d y=%d c=%d (NULL/empty)",
            n, retAddr, textRva, hdc, x, y, c);
    }
    return g_origTextOutA(hdc, x, y, lpStr, c);
}

static BOOL WINAPI HookedTextOutW(HDC hdc, int x, int y, LPCWSTR lpStr, int c) {
    int n = g_textOutWCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    DWORD retAddr = (DWORD)_ReturnAddress();
    DWORD textRva = retAddr - (DWORD)g_exeBase;
    if (lpStr && c > 0) {
        char buf[80] = {0};
        int wlen = c < 12 ? c : 12;
        for (int i = 0; i < wlen; ++i)
            sprintf(buf + i*5, "%04X ", (unsigned)lpStr[i]);
        Log("TextOutW[%d]: ret=0x%08X (RVA 0x%X) hdc=%p x=%d y=%d c=%d wchars: %s",
            n, retAddr, textRva, hdc, x, y, c, buf);
    } else {
        Log("TextOutW[%d]: ret=0x%08X (RVA 0x%X) hdc=%p x=%d y=%d c=%d (NULL/empty)",
            n, retAddr, textRva, hdc, x, y, c);
    }
    return g_origTextOutW(hdc, x, y, lpStr, c);
}

static BOOL WINAPI HookedExtTextOutA(HDC hdc, int x, int y, UINT opts, const RECT* rc,
                                     LPCSTR lpStr, UINT c, const INT* dx) {
    int n = g_extTextOutACalls.fetch_add(1, std::memory_order_relaxed) + 1;
    DWORD retAddr = (DWORD)_ReturnAddress();
    DWORD textRva = retAddr - (DWORD)g_exeBase;
    if (lpStr && c > 0) {
        const unsigned char* p = (const unsigned char*)lpStr;
        char buf[64] = {0};
        UINT blen = c < 16 ? c : 16;
        for (UINT i = 0; i < blen; ++i)
            sprintf(buf + i*3, "%02X ", p[i]);
        Log("ExtTextOutA[%d]: ret=0x%08X (RVA 0x%X) hdc=%p x=%d y=%d opts=0x%X c=%u bytes: %s",
            n, retAddr, textRva, hdc, x, y, opts, c, buf);
    } else {
        Log("ExtTextOutA[%d]: ret=0x%08X (RVA 0x%X) hdc=%p x=%d y=%d opts=0x%X c=%u (NULL/empty)",
            n, retAddr, textRva, hdc, x, y, opts, c);
    }
    return g_origExtTextOutA(hdc, x, y, opts, rc, lpStr, c, dx);
}

static BOOL WINAPI HookedExtTextOutW(HDC hdc, int x, int y, UINT opts, const RECT* rc,
                                     LPCWSTR lpStr, UINT c, const INT* dx) {
    int n = g_extTextOutWCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    DWORD retAddr = (DWORD)_ReturnAddress();
    DWORD textRva = retAddr - (DWORD)g_exeBase;
    if (lpStr && c > 0) {
        char buf[80] = {0};
        UINT wlen = c < 12 ? c : 12;
        for (UINT i = 0; i < wlen; ++i)
            sprintf(buf + i*5, "%04X ", (unsigned)lpStr[i]);
        Log("ExtTextOutW[%d]: ret=0x%08X (RVA 0x%X) hdc=%p x=%d y=%d opts=0x%X c=%u wchars: %s",
            n, retAddr, textRva, hdc, x, y, opts, c, buf);
    } else {
        Log("ExtTextOutW[%d]: ret=0x%08X (RVA 0x%X) hdc=%p x=%d y=%d opts=0x%X c=%u (NULL/empty)",
            n, retAddr, textRva, hdc, x, y, opts, c);
    }
    return g_origExtTextOutW(hdc, x, y, opts, rc, lpStr, c, dx);
}

// ── Glyph rasterizer call sites (diagnostic) ─────────────────────────
// The engine reaches its rasterizer through function pointers, so a static
// call graph cannot say which code draws a given surface.  Naming each
// GetGlyphOutlineA return address once does, and it draws nothing itself.
typedef DWORD (WINAPI *PFN_GetGlyphOutlineA)(HDC, UINT, UINT, LPGLYPHMETRICS,
                                             DWORD, LPVOID, const MAT2*);

static PFN_GetGlyphOutlineA g_origGetGlyphOutlineA = nullptr;
static std::atomic<int> g_glyphCalls { 0 };
static std::mutex g_glyphSitesMutex;
static std::vector<DWORD> g_glyphSites;

static DWORD WINAPI HookedGetGlyphOutlineA(HDC hdc, UINT ch, UINT fmt,
                                           LPGLYPHMETRICS gm, DWORD cb,
                                           LPVOID buf, const MAT2* mat) {
    const DWORD ret = (DWORD)_ReturnAddress();
    const int n = g_glyphCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(g_glyphSitesMutex);
        if (std::find(g_glyphSites.begin(), g_glyphSites.end(), ret) ==
            g_glyphSites.end()) {
            g_glyphSites.push_back(ret);
            fresh = true;
        }
    }
    if (fresh)
        Log("GetGlyphOutlineA: call site %zu ret=0x%08X (RVA 0x%X) at glyph %d "
            "char=0x%04X", g_glyphSites.size(), ret,
            ret - (DWORD)g_exeBase, n, ch);
    return g_origGetGlyphOutlineA(hdc, ch, fmt, gm, cb, buf, mat);
}

static void InstallGdiTextHooks() {
    g_origGetGlyphOutlineA = (PFN_GetGlyphOutlineA)
        IATHook(nullptr, "gdi32.dll", "GetGlyphOutlineA",
                (void*)&HookedGetGlyphOutlineA);
    g_origTextOutA    = (PFN_TextOutA)   IATHook(nullptr, "gdi32.dll", "TextOutA",    (void*)&HookedTextOutA);
    g_origTextOutW    = (PFN_TextOutW)   IATHook(nullptr, "gdi32.dll", "TextOutW",    (void*)&HookedTextOutW);
    g_origExtTextOutA = (PFN_ExtTextOutA)IATHook(nullptr, "gdi32.dll", "ExtTextOutA", (void*)&HookedExtTextOutA);
    g_origExtTextOutW = (PFN_ExtTextOutW)IATHook(nullptr, "gdi32.dll", "ExtTextOutW", (void*)&HookedExtTextOutW);
    Log("GdiTextHooks: TextOutA=%p TextOutW=%p ExtTextOutA=%p ExtTextOutW=%p "
        "GetGlyphOutlineA=%p",
        g_origTextOutA, g_origTextOutW, g_origExtTextOutA, g_origExtTextOutW,
        g_origGetGlyphOutlineA);
}

static void LogCounters(const char* when) {
    Log("Translator: stats %s -- walkerCalls=%d hits=%d misses=%d passthrough=%d "
        "renderFaults=%d boxOverflows=%d pitchSubst=%d rectOverruns=%d "
        "TextOutA=%d TextOutW=%d ExtTextOutA=%d ExtTextOutW=%d "
        "glyphs=%d glyphSites=%zu engineFaults=%d",
        when, g_walkerCalls.load(), g_hits.load(), g_misses.load(),
        g_passthrough.load(), g_renderFaults.load(), g_boxOverflows.load(),
        g_pitchSubst.load(), g_rectOverruns.load(),
        g_textOutACalls.load(), g_textOutWCalls.load(),
        g_extTextOutACalls.load(), g_extTextOutWCalls.load(),
        g_glyphCalls.load(), g_glyphSites.size(), g_engineFaults.load());
}

void TranslatorInit() {
    g_exeBase = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exeBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        g_exeBase + dos->e_lfanew);
    g_exeSize = nt->OptionalHeader.SizeOfImage;
    AddVectoredExceptionHandler(1, &LogFirstChance);
    Log("Translator: BGI.exe base = %p, image 0x%X bytes", g_exeBase, g_exeSize);
    g_translationsLoaded = LoadTranslations();
    if (PatchRectArraySites(g_exeBase))
        g_rectsPerWireByte =
            translator_logic::RECTS_PER_WIRE_BYTE * RECT_ARRAY_GROWTH;
    else
        Log("Translator: the rectangle array keeps the engine's own size, so "
            "a substitution is budgeted at %zu rectangles per wire byte",
            g_rectsPerWireByte);
    if (!InstallTextDisplayHook(g_exeBase))
        Log("Translator: hook FAILED -- no translation will occur");
    InstallGdiTextHooks();
}

void TranslatorShutdown() {
    LogCounters("shutdown");
}
