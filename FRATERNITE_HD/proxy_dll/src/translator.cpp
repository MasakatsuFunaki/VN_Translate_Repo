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
#include "corpus.h"
#include "overlay_toggle.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ============================================================================
// FRATERNITE_HD runtime translator (YU-RIS engine, SoftDenchi DRM).
//
// All substitution happens in HookedTextOutA below. The engine renders
// dialog one CP932 glyph at a time via GDI32!TextOutA into a memory DC.
// On each call we read the engine's state struct -- state[+0x38] buf38
// (CP932 text bytes), state[+0x3C] buf3c (opcode stream), state[+0x60]
// msglen, state[+0x64] opcount -- to identify the current message.
//
// On the first call of a new message we build an EN render plan (one
// ASCII chunk per CP932 glyph slot) via translator_logic::BuildRenderPlan.
// Subsequent calls (PASSES_PER_GLYPH per slot for outline/fill passes)
// emit the chunk for the current slot using a narrower font so multiple
// ASCII chars fit in the engine's per-glyph blit width.
// ============================================================================

static const DWORD STATE_PP_RVA = 0x008095F0 - 0x00400000;  // 0x4095F0

static constexpr size_t OFF_BUF38   = 0x38;
static constexpr size_t OFF_BUF3C   = 0x3C;
static constexpr size_t OFF_MSGLEN  = 0x60;
static constexpr size_t OFF_OPCOUNT = 0x64;
static constexpr size_t BUF_CAP     = 0x2000;

static BYTE* g_exeBase = nullptr;

static translator_logic::TranslationMap g_translations;
static bool g_translationsLoaded = false;

static std::atomic<int> g_textOutCalls { 0 };

// SEH probe: read N bytes from a possibly-bad pointer. Returns 0 on
// fault, N on success. Isolated in its own function because __try
// can't coexist with C++ destructors.
static int SafeRead(const void* src, void* dst, int n)
{
    __try { std::memcpy(dst, src, (size_t)n); return n; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// FNV-1a 32-bit. Cheap, deterministic, plenty for distinguishing
// 0x2000-byte buffers in this app.
static uint32_t Fnv1a32(const void* data, size_t n)
{
    uint32_t h = 0x811C9DC5u;
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h ? h : 1;
}

// ── Per-message render plan (TextOutA-driven, buf38 lookup) ─────────────
//
// Key empirical findings (see proxy_log diag dumps):
//   - The final render goes through GDI32!TextOutA with c=2 and a
//     heap-allocated 1-glyph staging buffer that the engine repopulates
//     per glyph. xy=(0,0) for every call -- the per-screen position is
//     applied elsewhere.
//   - The state struct's buf38 (state[+0x38]) IS populated with the
//     full CP932 message during rendering, with msg_len bytes total
//     and opc_len opcodes in buf3c (state[+0x3c]).
//
// So: at each dialog TextOutA call, we read state→buf38, hash to detect
// new messages, reconstruct the JP, run SegmentMessage to split it into
// known TSV pieces, and build a flat render-plan ASCII string with one
// chunk per JP glyph slot. As subsequent TextOutA calls render each
// glyph, we look up the next slot in the plan and pass that ASCII chunk
// (with c=chunk.size()) to the real TextOutA.

static std::vector<std::string> g_msgPlanChunks;   // one chunk per CP932 glyph slot
static uint32_t g_msgPlanHash      = 0;            // FNV1a of buf38+buf3c+opc_len
static int      g_msgPlanCallCount = 0;            // CP932 TextOutA calls within current message
static int      g_msgPlanLastIdx   = -1;           // last logged glyph index, for sub-log dedup
static int      g_msgPlanCps       = 5;            // chars-per-slot used for the current message's plan
static int      g_msgMaxIdx        = -1;           // highest glyph idx the engine reached this message
static int      g_msgOorGlyphs     = 0;            // glyphs that rendered past the plan (raw-JP leak)
static std::atomic<int> g_textOutSubs   { 0 };
static std::atomic<int> g_msgPlanBuilds { 0 };
static std::atomic<int> g_msgPlanMisses { 0 };

// Overlay-render state for the current message. When g_msgHasOverlay is
// true, the full English body is drawn on the overlay window instead of
// squished per-glyph, so the hook suppresses (renders invisible spaces)
// the engine's BODY glyph slots while leaving the first g_msgSpeakerSlots
// slots (the speaker label) for the engine to render as before. When
// false (no TSV match), the hook passes the original JP glyph through so
// untranslated lines show JP rather than a silent blank.
static bool g_msgHasOverlay   = false;
static int  g_msgSpeakerSlots = 0;

// Engine fires TextOutA this many times per glyph slot (3 outline
// passes on hdc1 + 1 fill pass on hdc2). Verified empirically across
// all observed scenes.
static constexpr int PASSES_PER_GLYPH = 4;

// Per-message variable chars-per-slot. Most messages render at
// MIN_CHARS_PER_SLOT (4 ≈ 10px chars at outline h=40, comfortable
// letter spacing); only messages whose EN doesn't fit at 4 narrow
// further, up to MAX_CHARS_PER_SLOT (7 ≈ 5.7px chars). Anything still
// over at 7 gets truncated and is reported by the corpus replay test
// for retranslate.
static constexpr int MIN_CHARS_PER_SLOT = 4;
static constexpr int MAX_CHARS_PER_SLOT = 7;

// Cached narrow fonts keyed by (font height, chars_per_slot). The
// engine renders glyphs at two heights (h≈40 outline, h≈160 fill); the
// per-message chars_per_slot varies from MIN_CHARS_PER_SLOT to
// MAX_CHARS_PER_SLOT. Total cached fonts is small (≤ 2 × 4 = 8) so a
// flat array of slots is fine.
struct NarrowFontKey { int height; int cps; };
static constexpr int FONT_CACHE_CAP = 16;
static HFONT          g_narrowFontHandles[FONT_CACHE_CAP] = { 0 };
static NarrowFontKey  g_narrowFontKeys[FONT_CACHE_CAP]    = { {0,0} };
static int            g_narrowFontCount                   = 0;

static HFONT GetNarrowFontFor(int height, int cps)
{
    if (height <= 0 || cps <= 0) return nullptr;
    for (int i = 0; i < g_narrowFontCount; i++) {
        if (g_narrowFontKeys[i].height == height
            && g_narrowFontKeys[i].cps == cps) {
            return g_narrowFontHandles[i];
        }
    }
    LOGFONTA lf = {0};
    lf.lfHeight = height;
    // Consolas (monospace) honors explicit lfWidth reliably -- each
    // char renders at exactly the requested width, no variable kerning
    // surprises like Arial Narrow's "th at" / "shoud dc" artifacts at
    // forced narrow widths. lfWidth = height / cps means `cps` chars
    // collectively occupy ~height pixels horizontally, fitting the
    // engine's ~42px per-slot blit (at outline h=40) cleanly.
    lf.lfWidth = height / cps;
    lf.lfWeight = FW_BOLD;
    lf.lfCharSet = ANSI_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    std::strncpy(lf.lfFaceName, "Consolas", LF_FACESIZE - 1);
    HFONT hf = CreateFontIndirectA(&lf);
    if (g_narrowFontCount < FONT_CACHE_CAP) {
        g_narrowFontHandles[g_narrowFontCount] = hf;
        g_narrowFontKeys[g_narrowFontCount]    = {height, cps};
        g_narrowFontCount++;
    }
    return hf;
}

// ── TextOutA hook ──────────────────────────────────────────────────────────

typedef BOOL (WINAPI* PFN_TextOutA)(HDC, int, int, LPCSTR, int);
static PFN_TextOutA g_realTextOutA = nullptr;

static BOOL WINAPI HookedTextOutA(HDC hdc, int x, int y, LPCSTR s, int c)
{
    g_textOutCalls.fetch_add(1, std::memory_order_relaxed);

    // Substitute only on 2-byte CP932 glyphs (the dialog render path).
    if (!g_translationsLoaded || c != 2 || !s) {
        return g_realTextOutA(hdc, x, y, s, c);
    }
    unsigned char b0 = 0, b1 = 0;
    if (SafeRead(s, &b0, 1) <= 0) return g_realTextOutA(hdc, x, y, s, c);
    SafeRead((const char*)s + 1, &b1, 1);
    bool is_cp932 = (b0 >= 0x81 && b0 <= 0x9F) || (b0 >= 0xE0 && b0 <= 0xFC);
    if (!is_cp932) {
        return g_realTextOutA(hdc, x, y, s, c);
    }

    // Read state → buf38, buf3c, opc_len. The engine populates these
    // for the current message; we use buf38 contents to identify the
    // message and build an EN render plan.
    if (!g_exeBase) return g_realTextOutA(hdc, x, y, s, c);
    char** state_pp = (char**)(g_exeBase + STATE_PP_RVA);
    char*  state    = nullptr;
    if (SafeRead(state_pp, &state, sizeof(state)) == 0 || !state) {
        return g_realTextOutA(hdc, x, y, s, c);
    }
    char* buf38 = nullptr;
    char* buf3c = nullptr;
    int   opc_len = 0;
    int   msg_len = 0;
    SafeRead(state + OFF_BUF38,   &buf38,   sizeof(buf38));
    SafeRead(state + OFF_BUF3C,   &buf3c,   sizeof(buf3c));
    SafeRead(state + OFF_OPCOUNT, &opc_len, sizeof(opc_len));
    SafeRead(state + OFF_MSGLEN,  &msg_len, sizeof(msg_len));
    if (!buf38 || !buf3c || opc_len <= 0 || opc_len > (int)BUF_CAP
        || msg_len <= 0 || msg_len > (int)BUF_CAP) {
        return g_realTextOutA(hdc, x, y, s, c);
    }

    // Snapshot buf38+buf3c so the hash and JP reconstruction see a
    // stable copy.
    char snap38[BUF_CAP];
    char snap3c[BUF_CAP];
    int  used38 = msg_len < (int)BUF_CAP ? msg_len : (int)BUF_CAP;
    if (SafeRead(buf38, snap38, used38) == 0
        || SafeRead(buf3c, snap3c, opc_len) == 0) {
        return g_realTextOutA(hdc, x, y, s, c);
    }
    uint32_t h = Fnv1a32(snap38, (size_t)used38)
               ^ Fnv1a32(snap3c, (size_t)opc_len)
               ^ (uint32_t)opc_len;

    // New message? Rebuild plan.
    if (h != g_msgPlanHash) {
        // Diagnostic: report the message we just finished. If the engine
        // reached a glyph index past the plan (g_msgMaxIdx >= chunks),
        // those extra glyphs leaked through as raw JP -- the "still
        // Japanese" symptom. Helps distinguish OOR-leak from a no-match.
        if (g_msgPlanHash != 0) {
            int nd = g_msgPlanBuilds.load(std::memory_order_relaxed);
            if (nd <= 200 && (g_msgOorGlyphs > 0 || g_msgMaxIdx + 1 != (int)g_msgPlanChunks.size())) {
                Log("RDIAG done: overlay=%d maxIdx=%d chunks=%u oorGlyphs=%d",
                    (int)g_msgHasOverlay, g_msgMaxIdx,
                    (unsigned)g_msgPlanChunks.size(), g_msgOorGlyphs);
            }
        }
        g_msgPlanHash       = h;
        g_msgPlanCallCount  = 0;
        g_msgPlanLastIdx    = -1;
        g_msgMaxIdx         = -1;
        g_msgOorGlyphs      = 0;

        std::string jp = translator_logic::ReconstructJpFromBuffers(
            snap38, BUF_CAP, snap3c, BUF_CAP, opc_len);
        // Auto-pick chars-per-slot per message so longer ENs that
        // wouldn't fit at MIN narrow further (down to MAX_CHARS_PER_SLOT)
        // instead of truncating. This keeps the translation pristine
        // and only shrinks the font on the specific lines that need it.
        g_msgPlanCps = translator_logic::ComputeAutoFitCharsPerSlot(
            jp, g_translations, MIN_CHARS_PER_SLOT, MAX_CHARS_PER_SLOT);
        translator_logic::BuildRenderPlan(jp, g_translations,
                                          g_msgPlanCps, g_msgPlanChunks);

        // Reconstruct the full English line for the overlay renderer.
        // When valid, we draw it ourselves (proportional, word-wrapped)
        // and suppress the engine's squished body glyphs below; the
        // speaker label (first speaker_slots slots) stays engine-rendered.
        translator_logic::EnglishLine el =
            translator_logic::ReconstructEnglish(jp, g_translations);
        g_msgHasOverlay   = el.valid;
        g_msgSpeakerSlots = el.speaker_slots;
        if (el.valid) {
            overlay_toggle::SetCurrentEnglish(el.speaker, el.body, h);
            int nb = g_msgPlanBuilds.load(std::memory_order_relaxed);
            if (nb <= 200) {
                Log("EN #%d: spk=[%.20s] slots=%d body=[%.120s]",
                    nb, el.speaker.c_str(), el.speaker_slots, el.body.c_str());
            }
        } else {
            // No English for this message: clear the overlay so the
            // previous line's English doesn't linger over the engine's
            // (untranslated) JP, which the hook passes through below.
            overlay_toggle::SetCurrentEnglish("", "", h);
        }

        // Capture this buf38 into the corpus file (one line per
        // unique JP-byte string). The replay test loads this file
        // and runs BuildRenderPlan / DetectOverflows on every entry,
        // catching alignment and truncation regressions on every
        // future build without requiring a playthrough.
        CorpusCapture(jp, Fnv1a32(jp.data(), jp.size()));

        if (!g_msgPlanChunks.empty()) {
            int nb = g_msgPlanBuilds.fetch_add(1, std::memory_order_relaxed) + 1;
            if (nb <= 200) {
                std::string flat;
                for (const auto& c : g_msgPlanChunks) { flat += '['; flat += c; flat += ']'; }
                Log("Plan #%d: JP[%.60s] -> SLOTS=%u PLAN=%.180s",
                    nb, jp.c_str(), (unsigned)g_msgPlanChunks.size(), flat.c_str());
            }
        } else {
            int nm = g_msgPlanMisses.fetch_add(1, std::memory_order_relaxed) + 1;
            if (nm <= 200) {
                Log("PlanMiss #%d: JP[%.80s] (jp_bytes=%u, opc_len=%d)",
                    nm, jp.c_str(), (unsigned)jp.size(), opc_len);
            }
        }
    }

    // Glyph-index advancement: by CALL COUNT, not byte-change. The
    // engine fires PASSES_PER_GLYPH (4) TextOutA calls per glyph slot
    // for shadow/outline/fill render layers -- we MUST emit on every
    // pass so all layers paint. Byte-change detection breaks for
    // repeated identical glyphs like ？？？ (all bytes [81 48]),
    // making every call render at slot 0 → "??? ??? ???" leaks.
    g_msgPlanCallCount++;
    int idx = (g_msgPlanCallCount - 1) / PASSES_PER_GLYPH;
    if (idx > g_msgMaxIdx) g_msgMaxIdx = idx;

    // Hand the call to overlay_toggle so F1 can later replay this
    // message's JP onto its overlay window. We always proceed with
    // EN substitution below -- the F1 overlay sits on top, no need
    // to skip substitution here.
    overlay_toggle::RecordDialogCall(hdc, x, y, b0, b1, idx, h);

    // No reconstructed English for this message: pass the original JP
    // glyph through. Showing JP for untranslated lines is honest and
    // avoids the silent all-blank line the old chunk path produced when
    // segmentation matched nothing (chunks were all spaces).
    if (!g_msgHasOverlay) {
        return g_realTextOutA(hdc, x, y, s, c);
    }

    if (g_msgPlanChunks.empty()
        || idx < 0 || (size_t)idx >= g_msgPlanChunks.size())
    {
        // Engine rendered a glyph the plan doesn't cover. The overlay
        // shows this message's English, so this extra glyph would leak as
        // raw JP -- count it (diagnostic) and blank it instead.
        g_msgOorGlyphs++;
        static const char kBlankRun[] = "        ";  // 8 spaces, no ink
        return g_realTextOutA(hdc, x, y, kBlankRun, 2);
    }

    const std::string& chunk = g_msgPlanChunks[(size_t)idx];
    if (chunk.empty()) {
        return g_realTextOutA(hdc, x, y, s, c);
    }

    // The overlay draws BOTH the speaker and the body now, so suppress
    // every engine glyph for this message (render invisible spaces, same
    // width/advance as a substituted slot, no ink).
    std::string blank(chunk.size(), ' ');

    int subs = g_textOutSubs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (subs <= 200 && idx != g_msgPlanLastIdx) {
        Log("Sub #%d: glyph=[%02X %02X] idx=%d -> blank (len=%u)",
            subs, b0, b1, idx, (unsigned)blank.size());
        g_msgPlanLastIdx = idx;
    }

    // Swap to a narrower font face for ASCII rendering. The engine's
    // default font has avgW≈21px (CJK + ASCII average); only 2 ASCII
    // chars fit in the engine's ~42px per-slot blit. Consolas at the
    // same height with explicit lfWidth = height / cps lets `cps`
    // ASCII chars fit naturally in the same slot without overlap. We
    // size-match the current font's height so outline (h=40) and fill
    // (h=160) passes both look right.
    TEXTMETRICA tm = {0};
    GetTextMetricsA(hdc, &tm);
    HFONT narrow = GetNarrowFontFor(tm.tmHeight, g_msgPlanCps);
    HGDIOBJ saved_font = nullptr;
    if (narrow) saved_font = SelectObject(hdc, narrow);
    BOOL r = g_realTextOutA(hdc, x, y, blank.data(), (int)blank.size());
    if (saved_font) SelectObject(hdc, saved_font);
    return r;
}

// ── TSV loading ─────────────────────────────────────────────────────────────

static bool LoadTranslations()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) wcscpy(lastSlash + 1, L"translation_table.tsv");
    else           wcscpy(exePath, L"translation_table.tsv");

    FILE* f = _wfopen(exePath, L"rb");
    if (!f) {
        Log("Translator: translation_table.tsv NOT FOUND - JP passthrough");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return false; }
    std::vector<char> buf((size_t)size);
    fread(buf.data(), 1, buf.size(), f);
    fclose(f);

    int count = translator_logic::ParseTsvBuffer(
        buf.data(), buf.size(), g_translations);
    int split = translator_logic::SplitSpeakerQuote(g_translations);
    Log("Translator: loaded %d TSV entries (+%d speaker/quote splits) "
        "from translation_table.tsv (CP932)",
        count, split);
    g_translationsLoaded = (count > 0);
    return count > 0;
}

// Read the engine's message-length dword (state[+0x60]) for the overlay
// driver's clear logic: nonzero while a message is active, 0 between
// messages / when the box is dismissed. Returns -1 on a bad read so the
// driver treats faults as "not zero" and won't clear spuriously.
static int ReadEngineMsgLen()
{
    if (!g_exeBase) return -1;
    char** state_pp = (char**)(g_exeBase + STATE_PP_RVA);
    char*  state    = nullptr;
    if (SafeRead(state_pp, &state, sizeof(state)) == 0 || !state) return -1;
    int msglen = -1;
    if (SafeRead(state + OFF_MSGLEN, &msglen, sizeof(msglen)) == 0) return -1;
    return msglen;
}

// ── Public API ──────────────────────────────────────────────────────────────

void TranslatorInit()
{
    g_exeBase = (BYTE*)GetModuleHandleW(nullptr);
    Log("Translator (TextOutA IAT hook + render plan): "
        "fraternite_hd.exe base=%p, state pp RVA=0x%X",
        g_exeBase, (unsigned)STATE_PP_RVA);

    LoadTranslations();

    g_realTextOutA = (PFN_TextOutA)IATHook(
        (HMODULE)g_exeBase, "GDI32.dll", "TextOutA", (void*)HookedTextOutA);
    if (!g_realTextOutA) {
        Log("Translator: TextOutA IAT hook FAILED -- fraternite_hd.exe "
            "imports GDI32!TextOutA via the static IAT; if that changes "
            "(LoadLibrary/GetProcAddress) we'd need to find a different path.");
        return;
    }
    Log("Translator: TextOutA IAT hooked.");

    overlay_toggle::SetMsgLenProvider(&ReadEngineMsgLen);
    overlay_toggle::Init();
}

void TranslatorShutdown()
{
    overlay_toggle::Shutdown();
    Log("Translator stats: textout_calls=%d textout_subs=%d "
        "plan_builds=%d plan_misses=%d",
        g_textOutCalls.load(),  g_textOutSubs.load(),
        g_msgPlanBuilds.load(), g_msgPlanMisses.load());
}
