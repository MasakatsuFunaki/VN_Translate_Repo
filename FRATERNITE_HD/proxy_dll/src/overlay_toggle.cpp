// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// F1 JP/EN toggle + overlay window.

#include "overlay_toggle.h"
#include "log.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace overlay_toggle {
namespace {

// ── Per-message TextOutA recording ─────────────────────────────────
struct RecordedCall {
    HDC      hdc;
    int      x, y;
    uint8_t  bytes[2];
    int      slot_idx;
    HFONT    font;
    COLORREF text_color;
    COLORREF bk_color;
    int      bk_mode;
};

std::vector<RecordedCall> g_lastFrameCalls;
uint32_t                  g_lastFrameHash = 0;
CRITICAL_SECTION          g_recordCs;
bool                      g_recordCsInit  = false;

// ── F1 toggle state ─────────────────────────────────────────────────
std::atomic<bool>  g_showJapanese  {false};
std::atomic<bool>  g_threadRunning {false};
HANDLE             g_threadHandle  = nullptr;

// ── English overlay state ───────────────────────────────────────────
CRITICAL_SECTION   g_ovlCs;
bool               g_ovlCsInit  = false;
std::string        g_curEnglish;            // body to draw in EN mode
std::string        g_curSpeaker;            // speaker name (drawn above body)
uint32_t           g_curEnHash  = 0;
std::atomic<bool>  g_enDirty    {false};    // new English waiting to paint
bool               g_ovlActive  = false;    // overlay currently showing text
int (*g_msgLenProvider)()       = nullptr;  // reads engine state[+0x60]
HFONT              g_ovlEnFont  = nullptr;
int                g_ovlEnFontH = 0;        // client height the font was sized for

// Dialogue-box geometry fractions, measured from in-game screenshots.
constexpr double OVL_BODY_LEFT_FRAC  = 0.18;
constexpr double OVL_BODY_RIGHT_FRAC = 0.84;
constexpr double OVL_BODY_TOP_FRAC   = 0.78;
constexpr double OVL_BODY_BOT_FRAC   = 0.97;
constexpr double OVL_BODY_FONT_FRAC  = 0.032;   // font em height / client height
constexpr double OVL_SPK_TOP_FRAC    = 0.725;   // speaker name baseline above the body

// Longer than the ~300ms inter-message gap to avoid flicker.
constexpr DWORD  OVL_CLEAR_DEBOUNCE_MS = 450;

// ── Overlay window ──────────────────────────────────────────────────
HWND      g_gameWindow  = nullptr;
HWND      g_ovlHwnd     = nullptr;
HDC       g_ovlBackDc   = nullptr;
HBITMAP   g_ovlBackBmp  = nullptr;
HBRUSH    g_ovlKeyBrush = nullptr;
HFONT     g_ovlJpFont   = nullptr;
int       g_ovlW        = 0;
int       g_ovlH        = 0;
// RGB(1,1,1) avoids punching holes in shadow strokes that use pure black.
const COLORREF OVL_COLOR_KEY = RGB(1, 1, 1);

// ── Game window discovery ───────────────────────────────────────────

BOOL CALLBACK DumpWndProc(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    char title[128] = {0}, cls[64] = {0};
    GetWindowTextA(h, title, sizeof(title));
    GetClassNameA(h, cls, sizeof(cls));
    RECT rc = {0}; GetClientRect(h, &rc);
    LONG style = GetWindowLongA(h, GWL_STYLE);
    Log("  Window: hwnd=%p parent=%p title=\"%s\" class=\"%s\" "
        "client=%dx%d visible=%d style=0x%08X",
        h, GetParent(h), title, cls,
        rc.right-rc.left, rc.bottom-rc.top,
        (int)IsWindowVisible(h), (unsigned)style);
    return TRUE;
}

BOOL CALLBACK FindGameWndProc(HWND h, LPARAM lp)
{
    DWORD wpid = 0;
    GetWindowThreadProcessId(h, &wpid);
    if (wpid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h))           return TRUE;
    if (GetWindow(h, GW_OWNER))        return TRUE;
    LONG style = GetWindowLongA(h, GWL_STYLE);
    if (style & WS_CHILD)              return TRUE;
    RECT rc; GetClientRect(h, &rc);
    if (rc.right - rc.left < 200 || rc.bottom - rc.top < 100) return TRUE;
    *(HWND*)lp = h;
    return FALSE;
}

HWND FindGameWindow()
{
    if (g_gameWindow && IsWindow(g_gameWindow)) return g_gameWindow;
    HWND found = nullptr;
    Log("FindGameWindow: enumerating top-level windows for PID %lu",
        GetCurrentProcessId());
    EnumWindows(DumpWndProc, 0);
    EnumWindows(FindGameWndProc, (LPARAM)&found);
    g_gameWindow = found;
    if (g_gameWindow) {
        char title[128] = {0};
        GetWindowTextA(g_gameWindow, title, sizeof(title));
        RECT rc; GetClientRect(g_gameWindow, &rc);
        Log("FindGameWindow: chose hwnd=%p title=\"%s\" client=%dx%d",
            g_gameWindow, title, rc.right-rc.left, rc.bottom-rc.top);
    } else {
        Log("FindGameWindow: no top-level visible window found for PID %lu",
            GetCurrentProcessId());
    }
    return g_gameWindow;
}

// ── Overlay window infrastructure ───────────────────────────────────

LRESULT CALLBACK OverlayWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        if (g_ovlBackDc) {
            BitBlt(hdc, 0, 0, g_ovlW, g_ovlH, g_ovlBackDc, 0, 0, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

bool EnsureOverlayBackBuffer(int w, int h)
{
    if (g_ovlBackDc && g_ovlW == w && g_ovlH == h) return true;
    if (g_ovlBackDc)  { DeleteDC(g_ovlBackDc); g_ovlBackDc = nullptr; }
    if (g_ovlBackBmp) { DeleteObject(g_ovlBackBmp); g_ovlBackBmp = nullptr; }
    HDC screen = GetDC(nullptr);
    g_ovlBackDc  = CreateCompatibleDC(screen);
    g_ovlBackBmp = CreateCompatibleBitmap(screen, w, h);
    ReleaseDC(nullptr, screen);
    if (!g_ovlBackDc || !g_ovlBackBmp) {
        Log("Overlay: failed to create back buffer %dx%d (err=%lu)",
            w, h, GetLastError());
        return false;
    }
    SelectObject(g_ovlBackDc, g_ovlBackBmp);
    if (!g_ovlKeyBrush) g_ovlKeyBrush = CreateSolidBrush(OVL_COLOR_KEY);
    RECT fr = {0, 0, w, h};
    FillRect(g_ovlBackDc, &fr, g_ovlKeyBrush);
    g_ovlW = w;
    g_ovlH = h;
    return true;
}

bool EnsureOverlayWindow()
{
    HWND game = FindGameWindow();
    if (!game) return false;
    RECT cr; GetClientRect(game, &cr);
    POINT pt = {0, 0}; ClientToScreen(game, &pt);
    int w = cr.right  - cr.left;
    int h = cr.bottom - cr.top;
    if (w <= 0 || h <= 0) return false;

    if (!g_ovlHwnd) {
        HINSTANCE hinst = GetModuleHandleW(nullptr);
        WNDCLASSA wc = {0};
        wc.lpfnWndProc   = OverlayWndProc;
        wc.hInstance     = hinst;
        wc.lpszClassName = "FraterniteOverlay";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        ATOM atom = RegisterClassA(&wc);
        if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Log("Overlay: RegisterClass failed err=%lu", GetLastError());
            return false;
        }
        g_ovlHwnd = CreateWindowExA(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST
                | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            "FraterniteOverlay", "",
            WS_POPUP,
            pt.x, pt.y, w, h,
            nullptr, nullptr, hinst, nullptr);
        if (!g_ovlHwnd) {
            Log("Overlay: CreateWindowEx failed err=%lu", GetLastError());
            return false;
        }
        SetLayeredWindowAttributes(g_ovlHwnd, OVL_COLOR_KEY, 0, LWA_COLORKEY);
        if (!EnsureOverlayBackBuffer(w, h)) {
            DestroyWindow(g_ovlHwnd);
            g_ovlHwnd = nullptr;
            return false;
        }
        ShowWindow(g_ovlHwnd, SW_SHOWNOACTIVATE);
        Log("Overlay: created hwnd=%p at (%d,%d) %dx%d",
            g_ovlHwnd, pt.x, pt.y, w, h);
    } else {
        EnsureOverlayBackBuffer(w, h);
        SetWindowPos(g_ovlHwnd, HWND_TOPMOST, pt.x, pt.y, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    return true;
}

void ClearOverlay()
{
    if (!g_ovlBackDc || !g_ovlHwnd) return;
    RECT fr = {0, 0, g_ovlW, g_ovlH};
    FillRect(g_ovlBackDc, &fr, g_ovlKeyBrush);
    InvalidateRect(g_ovlHwnd, nullptr, FALSE);
    UpdateWindow  (g_ovlHwnd);
}

HFONT GetOverlayJpFont()
{
    if (g_ovlJpFont) return g_ovlJpFont;
    LOGFONTA lf = {0};
    lf.lfHeight  = -28;
    lf.lfWeight  = FW_BOLD;
    lf.lfCharSet = SHIFTJIS_CHARSET;
    lf.lfOutPrecision  = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality        = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_MODERN;
    std::strncpy(lf.lfFaceName, "MS Gothic", LF_FACESIZE - 1);
    g_ovlJpFont = CreateFontIndirectA(&lf);
    return g_ovlJpFont;
}

// Yellow on black drop-shadow for contrast against arbitrary backgrounds.
void PaintOverlayString(int x, int y, const std::string& bytes)
{
    if (bytes.empty() || !g_ovlBackDc) return;
    HFONT jp_font = GetOverlayJpFont();
    HGDIOBJ saved = jp_font ? SelectObject(g_ovlBackDc, jp_font) : nullptr;
    SetBkMode(g_ovlBackDc, TRANSPARENT);
    SetTextColor(g_ovlBackDc, RGB(0, 0, 0));
    TextOutA(g_ovlBackDc, x + 2, y + 2, bytes.data(), (int)bytes.size());
    SetTextColor(g_ovlBackDc, RGB(255, 220, 60));
    TextOutA(g_ovlBackDc, x,     y,     bytes.data(), (int)bytes.size());
    if (saved) SelectObject(g_ovlBackDc, saved);
}

// Re-created when the client height changes.
HFONT GetOverlayEnFont(int clientH)
{
    int h = (int)(OVL_BODY_FONT_FRAC * clientH + 0.5);
    if (h < 12) h = 12;
    if (g_ovlEnFont && g_ovlEnFontH == h) return g_ovlEnFont;
    if (g_ovlEnFont) { DeleteObject(g_ovlEnFont); g_ovlEnFont = nullptr; }
    LOGFONTA lf = {0};
    lf.lfHeight        = -h;            // negative = em (character) height
    lf.lfWeight        = FW_BOLD;
    lf.lfCharSet       = ANSI_CHARSET;
    lf.lfOutPrecision  = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality       = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
    std::strncpy(lf.lfFaceName, "Segoe UI", LF_FACESIZE - 1);
    g_ovlEnFont  = CreateFontIndirectA(&lf);
    g_ovlEnFontH = h;
    return g_ovlEnFont;
}

// Caller must have a font selected into g_ovlBackDc.
void DrawShadowed(const std::string& s, RECT r, UINT fmt, COLORREF fg)
{
    RECT rs = r; OffsetRect(&rs, 2, 2);
    SetTextColor(g_ovlBackDc, RGB(0, 0, 0));
    DrawTextA(g_ovlBackDc, s.data(), (int)s.size(), &rs, fmt);
    SetTextColor(g_ovlBackDc, fg);
    DrawTextA(g_ovlBackDc, s.data(), (int)s.size(), &r, fmt);
}

// Caller holds g_ovlCs; back buffer is already cleared to the colour key.
void PaintEnglishBody(const std::string& speaker, const std::string& body)
{
    if (!g_ovlBackDc || body.empty()) return;
    int W = g_ovlW, H = g_ovlH;

    HFONT  f     = GetOverlayEnFont(H);
    HGDIOBJ saved = f ? SelectObject(g_ovlBackDc, f) : nullptr;
    SetBkMode(g_ovlBackDc, TRANSPARENT);
    const UINT fmt = DT_WORDBREAK | DT_LEFT | DT_TOP | DT_NOPREFIX
                   | DT_NOCLIP | DT_EXTERNALLEADING;

    if (!speaker.empty()) {
        RECT sr;
        sr.left   = (LONG)(OVL_BODY_LEFT_FRAC  * W);
        sr.right  = (LONG)(OVL_BODY_RIGHT_FRAC * W);
        sr.top    = (LONG)(OVL_SPK_TOP_FRAC    * H);
        sr.bottom = (LONG)(OVL_BODY_TOP_FRAC   * H);
        DrawShadowed(speaker, sr, fmt | DT_SINGLELINE, RGB(236, 226, 120)); // warm yellow
    }

    RECT r;
    r.left   = (LONG)(OVL_BODY_LEFT_FRAC  * W);
    r.right  = (LONG)(OVL_BODY_RIGHT_FRAC * W);
    r.top    = (LONG)(OVL_BODY_TOP_FRAC   * H);
    r.bottom = (LONG)(OVL_BODY_BOT_FRAC   * H);
    DrawShadowed(body, r, fmt, RGB(255, 255, 255));

    if (saved) SelectObject(g_ovlBackDc, saved);
}

// ── Repaint dispatch: JP banner (F1) or English body ────────────────

void RepaintOverlay()
{
    if (!EnsureOverlayWindow()) {
        Log("Overlay: can't ensure window -- skipping repaint");
        return;
    }
    if (!g_ovlBackDc) { Log("Overlay: back_dc=null - bailing"); return; }

    RECT fr = {0, 0, g_ovlW, g_ovlH};
    FillRect(g_ovlBackDc, &fr, g_ovlKeyBrush);

    if (g_showJapanese.load(std::memory_order_relaxed)) {
        EnterCriticalSection(&g_recordCs);
        std::vector<RecordedCall> snap = g_lastFrameCalls;
        LeaveCriticalSection(&g_recordCs);
        std::string jp_text;
        int last_slot = -1;
        for (const auto& rc : snap) {
            if (rc.slot_idx != last_slot) {
                jp_text.append((const char*)rc.bytes, 2);
                last_slot = rc.slot_idx;
            }
        }
        PaintOverlayString(30, 30, jp_text);
    } else {
        std::string speaker, body;
        EnterCriticalSection(&g_ovlCs);
        speaker = g_curSpeaker;
        body    = g_curEnglish;
        LeaveCriticalSection(&g_ovlCs);
        PaintEnglishBody(speaker, body);
    }

    GdiFlush();
    InvalidateRect(g_ovlHwnd, nullptr, FALSE);
    UpdateWindow  (g_ovlHwnd);
}

// ── Overlay driver thread (owns all painting) ──────────────────────

DWORD WINAPI OverlayDriverThread(LPVOID)
{
    Log("Overlay driver: thread started (50ms poll, default=ENGLISH)");
    bool  prev_down  = false;
    bool  zero_seen  = false;
    DWORD zero_since = 0;
    while (g_threadRunning.load(std::memory_order_relaxed)) {
        SHORT state = GetAsyncKeyState(VK_F1);
        bool now_down = (state & 0x8000) != 0;
        if (now_down && !prev_down) {
            bool was = g_showJapanese.load(std::memory_order_relaxed);
            g_showJapanese.store(!was, std::memory_order_relaxed);
            Log("F1 DOWN-EDGE: toggling display: %s -> %s",
                was ? "JAPANESE" : "ENGLISH", !was ? "JAPANESE" : "ENGLISH");
            RepaintOverlay();
        }
        prev_down = now_down;

        if (g_enDirty.exchange(false)) {
            g_ovlActive = true;
            zero_seen   = false;
            RepaintOverlay();
        }

        int msglen = g_msgLenProvider ? g_msgLenProvider() : -1;
        if (msglen == 0) {
            if (!zero_seen) { zero_seen = true; zero_since = GetTickCount(); }
            else if (g_ovlActive
                     && GetTickCount() - zero_since >= OVL_CLEAR_DEBOUNCE_MS) {
                ClearOverlay();
                g_ovlActive = false;
                Log("Overlay: cleared (engine msglen=0 held %ums)",
                    (unsigned)OVL_CLEAR_DEBOUNCE_MS);
            }
        } else {
            zero_seen = false;
        }

        Sleep(50);
    }
    Log("Overlay driver: thread exiting");
    return 0;
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────

void Init()
{
    if (!g_recordCsInit) {
        InitializeCriticalSection(&g_recordCs);
        g_recordCsInit = true;
    }
    if (!g_ovlCsInit) {
        InitializeCriticalSection(&g_ovlCs);
        g_ovlCsInit = true;
    }
    g_threadRunning.store(true, std::memory_order_relaxed);
    g_threadHandle = CreateThread(nullptr, 0, OverlayDriverThread, nullptr, 0, nullptr);
    if (!g_threadHandle) {
        Log("Overlay driver: CreateThread FAILED (err=%lu) -- overlay disabled",
            GetLastError());
    } else {
        Log("Overlay driver: thread spawned (handle=%p)", g_threadHandle);
    }
}

void Shutdown()
{
    g_threadRunning.store(false, std::memory_order_relaxed);
    if (g_threadHandle) {
        WaitForSingleObject(g_threadHandle, 1000);
        CloseHandle(g_threadHandle);
        g_threadHandle = nullptr;
    }
    if (g_ovlHwnd)     { DestroyWindow(g_ovlHwnd);     g_ovlHwnd     = nullptr; }
    if (g_ovlBackDc)   { DeleteDC(g_ovlBackDc);        g_ovlBackDc   = nullptr; }
    if (g_ovlBackBmp)  { DeleteObject(g_ovlBackBmp);   g_ovlBackBmp  = nullptr; }
    if (g_ovlKeyBrush) { DeleteObject(g_ovlKeyBrush);  g_ovlKeyBrush = nullptr; }
    if (g_ovlJpFont)   { DeleteObject(g_ovlJpFont);    g_ovlJpFont   = nullptr; }
    if (g_ovlEnFont)   { DeleteObject(g_ovlEnFont);    g_ovlEnFont   = nullptr; }
}

void SetCurrentEnglish(const std::string& speaker, const std::string& body,
                       uint32_t msg_hash)
{
    if (!g_ovlCsInit) return;
    EnterCriticalSection(&g_ovlCs);
    g_curSpeaker = speaker;
    g_curEnglish = body;
    g_curEnHash  = msg_hash;
    LeaveCriticalSection(&g_ovlCs);
    g_enDirty.store(true, std::memory_order_relaxed);
}

void SetMsgLenProvider(int (*fn)())
{
    g_msgLenProvider = fn;
}

void RecordDialogCall(HDC hdc, int x, int y,
                      uint8_t b0, uint8_t b1,
                      int slot_idx, uint32_t msg_hash)
{
    if (!g_recordCsInit) return;
    HFONT    cur_font = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
    COLORREF cur_tc   = GetTextColor(hdc);
    COLORREF cur_bc   = GetBkColor(hdc);
    int      cur_bm   = GetBkMode(hdc);
    EnterCriticalSection(&g_recordCs);
    if (msg_hash != g_lastFrameHash) {
        g_lastFrameHash = msg_hash;
        g_lastFrameCalls.clear();
    }
    g_lastFrameCalls.push_back({hdc, x, y, {b0, b1}, slot_idx,
                                cur_font, cur_tc, cur_bc, cur_bm});
    LeaveCriticalSection(&g_recordCs);
}

}  // namespace overlay_toggle
