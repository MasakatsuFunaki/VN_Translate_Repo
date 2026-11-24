// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "win32_chrome.h"

#include "chrome_text_logic.h"
#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr UINT kCodePage932 = 932u;
constexpr std::uint32_t kMaxPeHeaderOffset = 0x1000u;
constexpr int kDialogFontHeight9PtAt96Dpi = -12;
constexpr wchar_t kDialogFontFace[] = L"Segoe UI";

constexpr std::size_t kClassNameBufChars = 64u;
constexpr std::size_t kItemTextBufChars = 512u;
constexpr int kMaxTabItems = 32;
constexpr int kMaxListViewColumns = 16;
constexpr int kCheckBoxExtraWidthPx = 32;
constexpr int kControlRightMarginPx = 6;
constexpr int kSiblingOverlapSlackPx = 2;
constexpr UINT kRectEndpointCount = 2u;

using SetWindowTextWFn = BOOL(WINAPI*)(HWND, LPCWSTR);
using SetWindowTextAFn = BOOL(WINAPI*)(HWND, LPCSTR);
using CreateWindowExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int,
                                        int, int, int, HWND, HMENU, HINSTANCE,
                                        LPVOID);
using CreateWindowExAFn = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int,
                                        int, int, HWND, HMENU, HINSTANCE,
                                        LPVOID);
using FindWindowWFn = HWND(WINAPI*)(LPCWSTR, LPCWSTR);
using FindWindowAFn = HWND(WINAPI*)(LPCSTR, LPCSTR);
using MessageBoxWFn = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
using MessageBoxAFn = int(WINAPI*)(HWND, LPCSTR, LPCSTR, UINT);
using InsertMenuItemWFn = BOOL(WINAPI*)(HMENU, UINT, BOOL, LPCMENUITEMINFOW);
using InsertMenuItemAFn = BOOL(WINAPI*)(HMENU, UINT, BOOL, LPCMENUITEMINFOA);
using AppendMenuWFn = BOOL(WINAPI*)(HMENU, UINT, UINT_PTR, LPCWSTR);
using AppendMenuAFn = BOOL(WINAPI*)(HMENU, UINT, UINT_PTR, LPCSTR);
using ModifyMenuWFn = BOOL(WINAPI*)(HMENU, UINT, UINT, UINT_PTR, LPCWSTR);
using ModifyMenuAFn = BOOL(WINAPI*)(HMENU, UINT, UINT, UINT_PTR, LPCSTR);
using SetDlgItemTextWFn = BOOL(WINAPI*)(HWND, int, LPCWSTR);
using SetDlgItemTextAFn = BOOL(WINAPI*)(HWND, int, LPCSTR);
using DialogBoxParamWFn = INT_PTR(WINAPI*)(HINSTANCE, LPCWSTR, HWND, DLGPROC,
                                           LPARAM);
using DialogBoxParamAFn = INT_PTR(WINAPI*)(HINSTANCE, LPCSTR, HWND, DLGPROC,
                                           LPARAM);
using DialogBoxIndirectParamWFn = INT_PTR(WINAPI*)(HINSTANCE, LPCDLGTEMPLATEW,
                                                   HWND, DLGPROC, LPARAM);
using DialogBoxIndirectParamAFn = INT_PTR(WINAPI*)(HINSTANCE, LPCDLGTEMPLATEA,
                                                   HWND, DLGPROC, LPARAM);
using CreateDialogParamWFn = HWND(WINAPI*)(HINSTANCE, LPCWSTR, HWND, DLGPROC,
                                           LPARAM);
using CreateDialogParamAFn = HWND(WINAPI*)(HINSTANCE, LPCSTR, HWND, DLGPROC,
                                           LPARAM);
using CreateDialogIndirectParamWFn = HWND(WINAPI*)(HINSTANCE, LPCDLGTEMPLATEW,
                                                   HWND, DLGPROC, LPARAM);
using CreateDialogIndirectParamAFn = HWND(WINAPI*)(HINSTANCE, LPCDLGTEMPLATEA,
                                                   HWND, DLGPROC, LPARAM);

SetWindowTextWFn g_orig_SetWindowTextW = nullptr;
SetWindowTextAFn g_orig_SetWindowTextA = nullptr;
CreateWindowExWFn g_orig_CreateWindowExW = nullptr;
CreateWindowExAFn g_orig_CreateWindowExA = nullptr;
FindWindowWFn g_orig_FindWindowW = nullptr;
FindWindowAFn g_orig_FindWindowA = nullptr;
MessageBoxWFn g_orig_MessageBoxW = nullptr;
MessageBoxAFn g_orig_MessageBoxA = nullptr;
InsertMenuItemWFn g_orig_InsertMenuItemW = nullptr;
InsertMenuItemAFn g_orig_InsertMenuItemA = nullptr;
AppendMenuWFn g_orig_AppendMenuW = nullptr;
AppendMenuAFn g_orig_AppendMenuA = nullptr;
ModifyMenuWFn g_orig_ModifyMenuW = nullptr;
ModifyMenuAFn g_orig_ModifyMenuA = nullptr;
SetDlgItemTextWFn g_orig_SetDlgItemTextW = nullptr;
SetDlgItemTextAFn g_orig_SetDlgItemTextA = nullptr;
DialogBoxParamWFn g_orig_DialogBoxParamW = nullptr;
DialogBoxParamAFn g_orig_DialogBoxParamA = nullptr;
DialogBoxIndirectParamWFn g_orig_DialogBoxIndirectParamW = nullptr;
DialogBoxIndirectParamAFn g_orig_DialogBoxIndirectParamA = nullptr;
CreateDialogParamWFn g_orig_CreateDialogParamW = nullptr;
CreateDialogParamAFn g_orig_CreateDialogParamA = nullptr;
CreateDialogIndirectParamWFn g_orig_CreateDialogIndirectParamW = nullptr;
CreateDialogIndirectParamAFn g_orig_CreateDialogIndirectParamA = nullptr;

INIT_ONCE g_dialog_font_once = INIT_ONCE_STATIC_INIT;
HFONT g_dialog_font = nullptr;

struct ThreadDialogObserverState {
    HHOOK hook = nullptr;
    std::size_t active_depth = 0u;
    bool localizing = false;
};

thread_local ThreadDialogObserverState g_dialog_observer;

char AsciiLower(char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch + ('a' - 'A'));
    }
    return ch;
}

bool EqualsIgnoreCaseAscii(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (AsciiLower(*a) != AsciiLower(*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

bool IsValidStringPointer(const void* ptr) noexcept {
    return ptr != nullptr && !IS_INTRESOURCE(ptr);
}

std::wstring AnsiCp932ToWide(const char* ansi) {
    if (!IsValidStringPointer(ansi) || *ansi == '\0') {
        return {};
    }
    const int needed =
        ::MultiByteToWideChar(kCodePage932, 0u, ansi, -1, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed - 1), L'\0');
    ::MultiByteToWideChar(kCodePage932, 0u, ansi, -1, wide.data(), needed);
    return wide;
}

std::string WideToCp932(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        kCodePage932, 0u, wide.data(), static_cast<int>(wide.size()), nullptr,
        0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string narrow(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(kCodePage932, 0u, wide.data(),
                          static_cast<int>(wide.size()), narrow.data(), needed,
                          nullptr, nullptr);
    return narrow;
}

std::optional<std::wstring> TranslateAnyWindowOrControlWide(
    std::wstring_view text) {
    if (auto title = chrome_text::TranslateWindowTitle(text)) {
        return title;
    }
    return chrome_text::TranslateChromeWide(text);
}

std::optional<std::wstring> TranslateAnyWindowOrControlAnsi(const char* ansi) {
    const std::wstring wide = AnsiCp932ToWide(ansi);
    if (wide.empty()) {
        return std::nullopt;
    }
    return TranslateAnyWindowOrControlWide(wide);
}

BOOL CallOrigOrSystemSetWindowTextW(HWND hwnd, LPCWSTR text) {
    if (g_orig_SetWindowTextW != nullptr) {
        return g_orig_SetWindowTextW(hwnd, text);
    }
    return ::SetWindowTextW(hwnd, text);
}

BOOL CALLBACK InitDialogFontCallback(PINIT_ONCE /*init_once*/,
                                     PVOID /*param*/,
                                     PVOID* /*ctx*/) noexcept {
    LOGFONTW lf{};
    lf.lfHeight = kDialogFontHeight9PtAt96Dpi;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcsncpy_s(lf.lfFaceName, kDialogFontFace, _TRUNCATE);
    g_dialog_font = ::CreateFontIndirectW(&lf);
    return TRUE;
}

HFONT GetOrCreateDialogFont() noexcept {
    ::InitOnceExecuteOnce(&g_dialog_font_once, InitDialogFontCallback, nullptr,
                          nullptr);
    return g_dialog_font;
}

std::wstring ReadWindowTextString(HWND hwnd) {
    const int len = ::GetWindowTextLengthW(hwnd);
    if (len <= 0) {
        return {};
    }
    std::wstring buf(static_cast<std::size_t>(len) + 1u, L'\0');
    const int copied = ::GetWindowTextW(hwnd, buf.data(), len + 1);
    if (copied <= 0) {
        return {};
    }
    buf.resize(static_cast<std::size_t>(copied));
    return buf;
}

void LocalizeTabControlItems(HWND tab_hwnd) {
    const int count = static_cast<int>(
        ::SendMessageW(tab_hwnd, TCM_GETITEMCOUNT, 0, 0));
    const int capped = (count < kMaxTabItems) ? count : kMaxTabItems;

    wchar_t text_buf[kItemTextBufChars];
    for (int idx = 0; idx < capped; ++idx) {
        text_buf[0] = L'\0';
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = text_buf;
        item.cchTextMax = static_cast<int>(kItemTextBufChars);
        if (!::SendMessageW(tab_hwnd, TCM_GETITEMW, static_cast<WPARAM>(idx),
                            reinterpret_cast<LPARAM>(&item))) {
            continue;
        }
        const auto translated = chrome_text::TranslateChromeWide(text_buf);
        if (!translated.has_value()) {
            continue;
        }
        std::wstring mutable_en = *translated;
        TCITEMW update{};
        update.mask = TCIF_TEXT;
        update.pszText = mutable_en.data();
        ::SendMessageW(tab_hwnd, TCM_SETITEMW, static_cast<WPARAM>(idx),
                       reinterpret_cast<LPARAM>(&update));
    }
}

void LocalizeListViewColumns(HWND lv_hwnd) {
    const HWND header = reinterpret_cast<HWND>(
        ::SendMessageW(lv_hwnd, LVM_GETHEADER, 0, 0));
    int col_count = kMaxListViewColumns;
    if (header != nullptr) {
        const int reported = static_cast<int>(
            ::SendMessageW(header, HDM_GETITEMCOUNT, 0, 0));
        if (reported >= 0 && reported < col_count) {
            col_count = reported;
        }
    }

    wchar_t text_buf[kItemTextBufChars];
    for (int col = 0; col < col_count; ++col) {
        text_buf[0] = L'\0';
        LVCOLUMNW lvcol{};
        lvcol.mask = LVCF_TEXT;
        lvcol.pszText = text_buf;
        lvcol.cchTextMax = static_cast<int>(kItemTextBufChars);
        if (!::SendMessageW(lv_hwnd, LVM_GETCOLUMNW, static_cast<WPARAM>(col),
                            reinterpret_cast<LPARAM>(&lvcol))) {
            continue;
        }
        const auto translated = chrome_text::TranslateChromeWide(text_buf);
        if (!translated.has_value()) {
            continue;
        }
        std::wstring mutable_en = *translated;
        LVCOLUMNW update{};
        update.mask = LVCF_TEXT;
        update.pszText = mutable_en.data();
        ::SendMessageW(lv_hwnd, LVM_SETCOLUMNW, static_cast<WPARAM>(col),
                       reinterpret_cast<LPARAM>(&update));
    }
}

bool ShouldApplyDialogFont(const wchar_t* class_name) noexcept {
    return ::_wcsicmp(class_name, L"Button") == 0 ||
           ::_wcsicmp(class_name, L"Static") == 0 ||
           ::_wcsicmp(class_name, WC_TABCONTROLW) == 0 ||
           ::_wcsicmp(class_name, WC_LISTVIEWW) == 0 ||
           ::_wcsicmp(class_name, WC_HEADERW) == 0;
}

bool IsCheckOrRadioButton(HWND hwnd, const wchar_t* class_name) noexcept {
    if (::_wcsicmp(class_name, L"Button") != 0) {
        return false;
    }
    const LONG style = ::GetWindowLongW(hwnd, GWL_STYLE);
    if ((style & BS_PUSHLIKE) != 0) {
        return false;
    }
    const LONG btn_type = style & BS_TYPEMASK;
    return btn_type == BS_CHECKBOX || btn_type == BS_AUTOCHECKBOX ||
           btn_type == BS_RADIOBUTTON || btn_type == BS_AUTORADIOBUTTON ||
           btn_type == BS_3STATE || btn_type == BS_AUTO3STATE;
}

bool IsGroupBoxButton(HWND hwnd) noexcept {
    wchar_t cls[kClassNameBufChars] = {};
    if (::GetClassNameW(hwnd, cls, static_cast<int>(kClassNameBufChars)) <= 0 ||
        ::_wcsicmp(cls, L"Button") != 0) {
        return false;
    }
    return (::GetWindowLongW(hwnd, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX;
}

class ScopedScreenDcFont {
public:
    explicit ScopedScreenDcFont(HFONT font) noexcept : hdc_(::GetDC(nullptr)) {
        if (hdc_ != nullptr && font != nullptr) {
            prev_font_ = ::SelectObject(hdc_, font);
        }
    }

    ~ScopedScreenDcFont() noexcept {
        if (hdc_ != nullptr) {
            if (prev_font_ != nullptr) {
                ::SelectObject(hdc_, prev_font_);
            }
            ::ReleaseDC(nullptr, hdc_);
        }
    }

    ScopedScreenDcFont(const ScopedScreenDcFont&) = delete;
    ScopedScreenDcFont& operator=(const ScopedScreenDcFont&) = delete;

    HDC get() const noexcept { return hdc_; }

private:
    HDC hdc_ = nullptr;
    HGDIOBJ prev_font_ = nullptr;
};

RECT ReadRectInParentClient(HWND hwnd, HWND parent) noexcept {
    RECT rc{};
    ::GetWindowRect(hwnd, &rc);
    ::MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<LPPOINT>(&rc),
                      kRectEndpointCount);
    return rc;
}

void WidenClippedCheckOrRadioControl(HWND child,
                                     HFONT dialog_font,
                                     std::wstring_view text) noexcept {
    if (text.empty()) {
        return;
    }
    const HWND parent = ::GetParent(child);
    if (parent == nullptr) {
        return;
    }

    HFONT effective_font = dialog_font;
    if (effective_font == nullptr) {
        effective_font =
            reinterpret_cast<HFONT>(::SendMessageW(child, WM_GETFONT, 0, 0));
    }

    SIZE text_extent{};
    {
        ScopedScreenDcFont dc(effective_font);
        if (dc.get() == nullptr ||
            !::GetTextExtentPoint32W(dc.get(), text.data(),
                                     static_cast<int>(text.size()),
                                     &text_extent)) {
            return;
        }
    }

    const int needed_width = text_extent.cx +
                             ::GetSystemMetrics(SM_CXMENUCHECK) +
                             kCheckBoxExtraWidthPx;
    const RECT child_rc = ReadRectInParentClient(child, parent);
    const int current_width = child_rc.right - child_rc.left;
    if (current_width >= needed_width) {
        return;
    }

    RECT parent_client{};
    if (!::GetClientRect(parent, &parent_client)) {
        return;
    }
    int max_right = parent_client.right - kControlRightMarginPx;

    for (HWND sib = ::GetWindow(parent, GW_CHILD); sib != nullptr;
         sib = ::GetWindow(sib, GW_HWNDNEXT)) {
        if (sib == child) {
            continue;
        }
        const RECT sib_rc = ReadRectInParentClient(sib, parent);
        if (sib_rc.bottom <= child_rc.top || sib_rc.top >= child_rc.bottom) {
            continue;
        }
        if (IsGroupBoxButton(sib) && sib_rc.left <= child_rc.left &&
            sib_rc.right >= child_rc.right) {
            max_right =
                std::min(max_right,
                         static_cast<int>(sib_rc.right) - kControlRightMarginPx);
            continue;
        }
        if ((::GetWindowLongW(sib, GWL_STYLE) & WS_VISIBLE) != 0 &&
            sib_rc.left >= child_rc.right - kSiblingOverlapSlackPx) {
            max_right =
                std::min(max_right,
                         static_cast<int>(sib_rc.left) - kControlRightMarginPx);
        }
    }

    const int target_right =
        std::min(static_cast<int>(child_rc.left) + needed_width, max_right);
    if (target_right > child_rc.right) {
        ::SetWindowPos(child, nullptr, 0, 0, target_right - child_rc.left,
                       child_rc.bottom - child_rc.top,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

BOOL CALLBACK LocalizeChildControlCallback(HWND child, LPARAM lparam) {
    wchar_t class_name[kClassNameBufChars] = {};
    const bool has_class =
        ::GetClassNameW(child, class_name,
                        static_cast<int>(kClassNameBufChars)) > 0;

    const auto dialog_font = reinterpret_cast<HFONT>(lparam);
    if (dialog_font != nullptr && has_class &&
        ShouldApplyDialogFont(class_name)) {
        const auto current_font =
            reinterpret_cast<HFONT>(::SendMessageW(child, WM_GETFONT, 0, 0));
        if (current_font != dialog_font) {
            ::SendMessageW(child, WM_SETFONT,
                           reinterpret_cast<WPARAM>(dialog_font), TRUE);
        }
    }

    std::wstring final_text = ReadWindowTextString(child);
    if (!final_text.empty()) {
        if (const auto en = chrome_text::TranslateChromeWide(final_text)) {
            CallOrigOrSystemSetWindowTextW(child, en->c_str());
            final_text = *en;
        }
        if (has_class && IsCheckOrRadioButton(child, class_name)) {
            WidenClippedCheckOrRadioControl(child, dialog_font, final_text);
        }
    }

    if (has_class && ::_wcsicmp(class_name, WC_TABCONTROLW) == 0) {
        LocalizeTabControlItems(child);
        return TRUE;
    }
    if (has_class && ::_wcsicmp(class_name, WC_LISTVIEWW) == 0) {
        LocalizeListViewColumns(child);
    }
    return TRUE;
}

void LocalizeInitializedDialog(HWND hdlg) {
    const HFONT font = GetOrCreateDialogFont();
    const std::wstring caption = ReadWindowTextString(hdlg);
    if (!caption.empty()) {
        if (const auto en = TranslateAnyWindowOrControlWide(caption)) {
            CallOrigOrSystemSetWindowTextW(hdlg, en->c_str());
        }
    }
    ::EnumChildWindows(hdlg, LocalizeChildControlCallback,
                       reinterpret_cast<LPARAM>(font));
}

class ScopedLocalizingGuard {
public:
    explicit ScopedLocalizingGuard(bool& flag) noexcept : flag_(flag) {
        flag_ = true;
    }
    ~ScopedLocalizingGuard() noexcept { flag_ = false; }

    ScopedLocalizingGuard(const ScopedLocalizingGuard&) = delete;
    ScopedLocalizingGuard& operator=(const ScopedLocalizingGuard&) = delete;

private:
    bool& flag_;
};

// Observes WM_INITDIALOG completion via WH_CALLWNDPROCRET without replacing
// `dialog_func` or mutating DWLP_DLGPROC.
LRESULT CALLBACK DialogInitCallWndRetProc(int code,
                                          WPARAM wparam,
                                          LPARAM lparam) noexcept {
    const HHOOK current_hook = g_dialog_observer.hook;
    if (code >= 0 && !g_dialog_observer.localizing && lparam != 0) {
        const auto* ret = reinterpret_cast<const CWPRETSTRUCT*>(lparam);
        if (ret->message == WM_INITDIALOG && ret->hwnd != nullptr) {
            ScopedLocalizingGuard reentrancy_guard(
                g_dialog_observer.localizing);
            try {
                LocalizeInitializedDialog(ret->hwnd);
            } catch (...) {
            }
        }
    }
    return ::CallNextHookEx(current_hook, code, wparam, lparam);
}

class ScopedDialogInitObserver {
public:
    ScopedDialogInitObserver() noexcept {
        if (g_dialog_observer.active_depth == 0u) {
            g_dialog_observer.hook = ::SetWindowsHookExW(
                WH_CALLWNDPROCRET, &DialogInitCallWndRetProc, nullptr,
                ::GetCurrentThreadId());
        }
        ++g_dialog_observer.active_depth;
    }

    ~ScopedDialogInitObserver() noexcept {
        if (g_dialog_observer.active_depth > 0u) {
            --g_dialog_observer.active_depth;
        }
        if (g_dialog_observer.active_depth == 0u &&
            g_dialog_observer.hook != nullptr) {
            ::UnhookWindowsHookEx(g_dialog_observer.hook);
            g_dialog_observer.hook = nullptr;
        }
    }

    ScopedDialogInitObserver(const ScopedDialogInitObserver&) = delete;
    ScopedDialogInitObserver& operator=(const ScopedDialogInitObserver&) =
        delete;
};

BOOL WINAPI HookedSetWindowTextW(HWND hwnd, LPCWSTR text) {
    if (IsValidStringPointer(text)) {
        if (const auto en = TranslateAnyWindowOrControlWide(text)) {
            return g_orig_SetWindowTextW(hwnd, en->c_str());
        }
    }
    return g_orig_SetWindowTextW(hwnd, text);
}

BOOL WINAPI HookedSetWindowTextA(HWND hwnd, LPCSTR text) {
    if (const auto en = TranslateAnyWindowOrControlAnsi(text)) {
        return CallOrigOrSystemSetWindowTextW(hwnd, en->c_str());
    }
    return g_orig_SetWindowTextA(hwnd, text);
}

HWND WINAPI HookedCreateWindowExW(DWORD ex_style,
                                  LPCWSTR class_name,
                                  LPCWSTR window_name,
                                  DWORD style,
                                  int x,
                                  int y,
                                  int width,
                                  int height,
                                  HWND parent,
                                  HMENU menu,
                                  HINSTANCE instance,
                                  LPVOID param) {
    if (IsValidStringPointer(window_name)) {
        if (const auto en = TranslateAnyWindowOrControlWide(window_name)) {
            return g_orig_CreateWindowExW(ex_style, class_name, en->c_str(),
                                          style, x, y, width, height, parent,
                                          menu, instance, param);
        }
    }
    return g_orig_CreateWindowExW(ex_style, class_name, window_name, style, x,
                                  y, width, height, parent, menu, instance,
                                  param);
}

HWND WINAPI HookedCreateWindowExA(DWORD ex_style,
                                  LPCSTR class_name,
                                  LPCSTR window_name,
                                  DWORD style,
                                  int x,
                                  int y,
                                  int width,
                                  int height,
                                  HWND parent,
                                  HMENU menu,
                                  HINSTANCE instance,
                                  LPVOID param) {
    const HWND hwnd =
        g_orig_CreateWindowExA(ex_style, class_name, window_name, style, x, y,
                               width, height, parent, menu, instance, param);
    if (hwnd != nullptr) {
        if (const auto en = TranslateAnyWindowOrControlAnsi(window_name)) {
            CallOrigOrSystemSetWindowTextW(hwnd, en->c_str());
        }
    }
    return hwnd;
}

HWND WINAPI HookedFindWindowW(LPCWSTR class_name, LPCWSTR window_name) {
    const HWND found = g_orig_FindWindowW(class_name, window_name);
    if (found != nullptr || !IsValidStringPointer(window_name)) {
        return found;
    }
    if (const auto en = chrome_text::TranslateWindowTitle(window_name)) {
        return g_orig_FindWindowW(class_name, en->c_str());
    }
    return nullptr;
}

HWND WINAPI HookedFindWindowA(LPCSTR class_name, LPCSTR window_name) {
    const HWND found = g_orig_FindWindowA(class_name, window_name);
    if (found != nullptr || !IsValidStringPointer(window_name)) {
        return found;
    }
    const std::wstring wide_name = AnsiCp932ToWide(window_name);
    const auto en = chrome_text::TranslateWindowTitle(wide_name);
    if (!en.has_value()) {
        return nullptr;
    }
    const FindWindowWFn find_w =
        (g_orig_FindWindowW != nullptr) ? g_orig_FindWindowW : &::FindWindowW;
    if (!IsValidStringPointer(class_name)) {
        return find_w(reinterpret_cast<LPCWSTR>(class_name), en->c_str());
    }
    const std::wstring wide_class = AnsiCp932ToWide(class_name);
    return find_w(wide_class.c_str(), en->c_str());
}

int WINAPI HookedMessageBoxW(HWND hwnd,
                             LPCWSTR text,
                             LPCWSTR caption,
                             UINT type) {
    std::optional<std::wstring> en_text;
    std::optional<std::wstring> en_caption;
    if (IsValidStringPointer(text)) {
        en_text = TranslateAnyWindowOrControlWide(text);
    }
    if (IsValidStringPointer(caption)) {
        en_caption = TranslateAnyWindowOrControlWide(caption);
    }
    const LPCWSTR final_text = en_text.has_value() ? en_text->c_str() : text;
    const LPCWSTR final_cap =
        en_caption.has_value() ? en_caption->c_str() : caption;
    return g_orig_MessageBoxW(hwnd, final_text, final_cap, type);
}

int WINAPI HookedMessageBoxA(HWND hwnd,
                             LPCSTR text,
                             LPCSTR caption,
                             UINT type) {
    const std::wstring wide_text = AnsiCp932ToWide(text);
    const std::wstring wide_caption = AnsiCp932ToWide(caption);
    const auto en_text = TranslateAnyWindowOrControlWide(wide_text);
    const auto en_caption = TranslateAnyWindowOrControlWide(wide_caption);
    if (!en_text.has_value() && !en_caption.has_value()) {
        return g_orig_MessageBoxA(hwnd, text, caption, type);
    }
    const LPCWSTR final_text =
        en_text.has_value()
            ? en_text->c_str()
            : (wide_text.empty() ? nullptr : wide_text.c_str());
    const LPCWSTR final_cap =
        en_caption.has_value()
            ? en_caption->c_str()
            : (wide_caption.empty() ? nullptr : wide_caption.c_str());
    const MessageBoxWFn msg_w =
        (g_orig_MessageBoxW != nullptr) ? g_orig_MessageBoxW : &::MessageBoxW;
    return msg_w(hwnd, final_text, final_cap, type);
}

BOOL WINAPI HookedInsertMenuItemW(HMENU hmenu,
                                  UINT item,
                                  BOOL by_position,
                                  LPCMENUITEMINFOW mii) {
    if (mii == nullptr) {
        return g_orig_InsertMenuItemW(hmenu, item, by_position, nullptr);
    }
    const bool has_string_mask = (mii->fMask & MIIM_STRING) != 0u;
    const bool has_legacy_type_string =
        (mii->fMask & MIIM_TYPE) != 0u &&
        (mii->fType & (MFT_SEPARATOR | MFT_BITMAP | MFT_OWNERDRAW)) == 0u;

    if ((has_string_mask || has_legacy_type_string) &&
        IsValidStringPointer(mii->dwTypeData)) {
        if (const auto en = chrome_text::TranslateChromeWide(mii->dwTypeData)) {
            std::wstring mutable_en = *en;
            MENUITEMINFOW translated_mii = *mii;
            translated_mii.dwTypeData = mutable_en.data();
            translated_mii.cch = static_cast<UINT>(mutable_en.size());
            return g_orig_InsertMenuItemW(hmenu, item, by_position,
                                          &translated_mii);
        }
    }
    return g_orig_InsertMenuItemW(hmenu, item, by_position, mii);
}

BOOL WINAPI HookedInsertMenuItemA(HMENU hmenu,
                                  UINT item,
                                  BOOL by_position,
                                  LPCMENUITEMINFOA mii) {
    if (mii == nullptr) {
        return g_orig_InsertMenuItemA(hmenu, item, by_position, nullptr);
    }
    const bool has_string_mask = (mii->fMask & MIIM_STRING) != 0u;
    const bool has_legacy_type_string =
        (mii->fMask & MIIM_TYPE) != 0u &&
        (mii->fType & (MFT_SEPARATOR | MFT_BITMAP | MFT_OWNERDRAW)) == 0u;

    if ((has_string_mask || has_legacy_type_string) &&
        IsValidStringPointer(mii->dwTypeData)) {
        const std::wstring wide = AnsiCp932ToWide(mii->dwTypeData);
        if (const auto en = chrome_text::TranslateChromeWide(wide)) {
            std::string mutable_cp932 = WideToCp932(*en);
            if (!mutable_cp932.empty()) {
                MENUITEMINFOA translated_mii = *mii;
                translated_mii.dwTypeData = mutable_cp932.data();
                translated_mii.cch = static_cast<UINT>(mutable_cp932.size());
                return g_orig_InsertMenuItemA(hmenu, item, by_position,
                                              &translated_mii);
            }
        }
    }
    return g_orig_InsertMenuItemA(hmenu, item, by_position, mii);
}

BOOL WINAPI HookedAppendMenuW(HMENU hmenu,
                              UINT flags,
                              UINT_PTR new_item_id,
                              LPCWSTR new_item) {
    if ((flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)) == 0u &&
        IsValidStringPointer(new_item)) {
        if (const auto en = chrome_text::TranslateChromeWide(new_item)) {
            return g_orig_AppendMenuW(hmenu, flags, new_item_id, en->c_str());
        }
    }
    return g_orig_AppendMenuW(hmenu, flags, new_item_id, new_item);
}

BOOL WINAPI HookedAppendMenuA(HMENU hmenu,
                              UINT flags,
                              UINT_PTR new_item_id,
                              LPCSTR new_item) {
    if ((flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)) == 0u &&
        IsValidStringPointer(new_item)) {
        const std::wstring wide = AnsiCp932ToWide(new_item);
        if (const auto en = chrome_text::TranslateChromeWide(wide)) {
            const AppendMenuWFn append_w = (g_orig_AppendMenuW != nullptr)
                                               ? g_orig_AppendMenuW
                                               : &::AppendMenuW;
            return append_w(hmenu, flags, new_item_id, en->c_str());
        }
    }
    return g_orig_AppendMenuA(hmenu, flags, new_item_id, new_item);
}

BOOL WINAPI HookedModifyMenuW(HMENU hmenu,
                              UINT pos,
                              UINT flags,
                              UINT_PTR new_item_id,
                              LPCWSTR new_item) {
    if ((flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)) == 0u &&
        IsValidStringPointer(new_item)) {
        if (const auto en = chrome_text::TranslateChromeWide(new_item)) {
            return g_orig_ModifyMenuW(hmenu, pos, flags, new_item_id,
                                      en->c_str());
        }
    }
    return g_orig_ModifyMenuW(hmenu, pos, flags, new_item_id, new_item);
}

BOOL WINAPI HookedModifyMenuA(HMENU hmenu,
                              UINT pos,
                              UINT flags,
                              UINT_PTR new_item_id,
                              LPCSTR new_item) {
    if ((flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)) == 0u &&
        IsValidStringPointer(new_item)) {
        const std::wstring wide = AnsiCp932ToWide(new_item);
        if (const auto en = chrome_text::TranslateChromeWide(wide)) {
            const ModifyMenuWFn modify_w = (g_orig_ModifyMenuW != nullptr)
                                               ? g_orig_ModifyMenuW
                                               : &::ModifyMenuW;
            return modify_w(hmenu, pos, flags, new_item_id, en->c_str());
        }
    }
    return g_orig_ModifyMenuA(hmenu, pos, flags, new_item_id, new_item);
}

BOOL WINAPI HookedSetDlgItemTextW(HWND hdlg, int ctrl_id, LPCWSTR text) {
    if (IsValidStringPointer(text)) {
        if (const auto en = chrome_text::TranslateChromeWide(text)) {
            return g_orig_SetDlgItemTextW(hdlg, ctrl_id, en->c_str());
        }
    }
    return g_orig_SetDlgItemTextW(hdlg, ctrl_id, text);
}

BOOL WINAPI HookedSetDlgItemTextA(HWND hdlg, int ctrl_id, LPCSTR text) {
    if (IsValidStringPointer(text)) {
        const std::wstring wide = AnsiCp932ToWide(text);
        if (const auto en = chrome_text::TranslateChromeWide(wide)) {
            const SetDlgItemTextWFn set_w = (g_orig_SetDlgItemTextW != nullptr)
                                                ? g_orig_SetDlgItemTextW
                                                : &::SetDlgItemTextW;
            return set_w(hdlg, ctrl_id, en->c_str());
        }
    }
    return g_orig_SetDlgItemTextA(hdlg, ctrl_id, text);
}

INT_PTR WINAPI HookedDialogBoxParamW(HINSTANCE instance,
                                     LPCWSTR template_name,
                                     HWND parent,
                                     DLGPROC dialog_func,
                                     LPARAM init_param) {
    ScopedDialogInitObserver observer;
    return g_orig_DialogBoxParamW(instance, template_name, parent, dialog_func,
                                  init_param);
}

INT_PTR WINAPI HookedDialogBoxParamA(HINSTANCE instance,
                                     LPCSTR template_name,
                                     HWND parent,
                                     DLGPROC dialog_func,
                                     LPARAM init_param) {
    ScopedDialogInitObserver observer;
    return g_orig_DialogBoxParamA(instance, template_name, parent, dialog_func,
                                  init_param);
}

INT_PTR WINAPI HookedDialogBoxIndirectParamW(
    HINSTANCE instance,
    LPCDLGTEMPLATEW dialog_template,
    HWND parent,
    DLGPROC dialog_func,
    LPARAM init_param) {
    ScopedDialogInitObserver observer;
    return g_orig_DialogBoxIndirectParamW(instance, dialog_template, parent,
                                          dialog_func, init_param);
}

INT_PTR WINAPI HookedDialogBoxIndirectParamA(
    HINSTANCE instance,
    LPCDLGTEMPLATEA dialog_template,
    HWND parent,
    DLGPROC dialog_func,
    LPARAM init_param) {
    ScopedDialogInitObserver observer;
    return g_orig_DialogBoxIndirectParamA(instance, dialog_template, parent,
                                          dialog_func, init_param);
}

HWND WINAPI HookedCreateDialogParamW(HINSTANCE instance,
                                     LPCWSTR template_name,
                                     HWND parent,
                                     DLGPROC dialog_func,
                                     LPARAM init_param) {
    ScopedDialogInitObserver observer;
    return g_orig_CreateDialogParamW(instance, template_name, parent,
                                     dialog_func, init_param);
}

HWND WINAPI HookedCreateDialogParamA(HINSTANCE instance,
                                     LPCSTR template_name,
                                     HWND parent,
                                     DLGPROC dialog_func,
                                     LPARAM init_param) {
    ScopedDialogInitObserver observer;
    return g_orig_CreateDialogParamA(instance, template_name, parent,
                                     dialog_func, init_param);
}

HWND WINAPI HookedCreateDialogIndirectParamW(HINSTANCE instance,
                                             LPCDLGTEMPLATEW dialog_template,
                                             HWND parent,
                                             DLGPROC dialog_func,
                                             LPARAM init_param) {
    ScopedDialogInitObserver observer;
    return g_orig_CreateDialogIndirectParamW(instance, dialog_template, parent,
                                             dialog_func, init_param);
}

HWND WINAPI HookedCreateDialogIndirectParamA(HINSTANCE instance,
                                             LPCDLGTEMPLATEA dialog_template,
                                             HWND parent,
                                             DLGPROC dialog_func,
                                             LPARAM init_param) {
    ScopedDialogInitObserver observer;
    return g_orig_CreateDialogIndirectParamA(instance, dialog_template, parent,
                                             dialog_func, init_param);
}

struct IatHookBinding {
    const char* export_name;
    void* detour_fn;
    void** orig_slot;
};

bool PatchSingleIatEntry(IMAGE_THUNK_DATA* iat_thunk,
                         void* detour_fn,
                         void** orig_slot) noexcept {
    auto* current_fn = reinterpret_cast<void*>(iat_thunk->u1.Function);
    if (current_fn == nullptr || current_fn == detour_fn) {
        return false;
    }

    DWORD old_protect = 0u;
    if (!::VirtualProtect(&iat_thunk->u1.Function, sizeof(ULONG_PTR),
                          PAGE_READWRITE, &old_protect)) {
        return false;
    }

    if (*orig_slot == nullptr) {
        *orig_slot = current_fn;
    }
    iat_thunk->u1.Function = reinterpret_cast<ULONG_PTR>(detour_fn);

    DWORD dummy = 0u;
    ::VirtualProtect(&iat_thunk->u1.Function, sizeof(ULONG_PTR), old_protect,
                     &dummy);
    return true;
}

}  // namespace

namespace win32_chrome {

bool InstallWindowChromeHooks(void* host_module_base) noexcept {
    if (host_module_base == nullptr) {
        return false;
    }

    auto* base = static_cast<std::uint8_t*>(host_module_base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<std::uint32_t>(dos->e_lfanew) > kMaxPeHeaderOffset) {
        return false;
    }

    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const std::uint32_t image_size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY& imp_dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp_dir.VirtualAddress == 0u || imp_dir.Size == 0u ||
        imp_dir.VirtualAddress >= image_size) {
        return true;
    }

    const IatHookBinding bindings[] = {
        {"SetWindowTextW", reinterpret_cast<void*>(&HookedSetWindowTextW),
         reinterpret_cast<void**>(&g_orig_SetWindowTextW)},
        {"SetWindowTextA", reinterpret_cast<void*>(&HookedSetWindowTextA),
         reinterpret_cast<void**>(&g_orig_SetWindowTextA)},
        {"CreateWindowExW", reinterpret_cast<void*>(&HookedCreateWindowExW),
         reinterpret_cast<void**>(&g_orig_CreateWindowExW)},
        {"CreateWindowExA", reinterpret_cast<void*>(&HookedCreateWindowExA),
         reinterpret_cast<void**>(&g_orig_CreateWindowExA)},
        {"FindWindowW", reinterpret_cast<void*>(&HookedFindWindowW),
         reinterpret_cast<void**>(&g_orig_FindWindowW)},
        {"FindWindowA", reinterpret_cast<void*>(&HookedFindWindowA),
         reinterpret_cast<void**>(&g_orig_FindWindowA)},
        {"MessageBoxW", reinterpret_cast<void*>(&HookedMessageBoxW),
         reinterpret_cast<void**>(&g_orig_MessageBoxW)},
        {"MessageBoxA", reinterpret_cast<void*>(&HookedMessageBoxA),
         reinterpret_cast<void**>(&g_orig_MessageBoxA)},
        {"InsertMenuItemW", reinterpret_cast<void*>(&HookedInsertMenuItemW),
         reinterpret_cast<void**>(&g_orig_InsertMenuItemW)},
        {"InsertMenuItemA", reinterpret_cast<void*>(&HookedInsertMenuItemA),
         reinterpret_cast<void**>(&g_orig_InsertMenuItemA)},
        {"AppendMenuW", reinterpret_cast<void*>(&HookedAppendMenuW),
         reinterpret_cast<void**>(&g_orig_AppendMenuW)},
        {"AppendMenuA", reinterpret_cast<void*>(&HookedAppendMenuA),
         reinterpret_cast<void**>(&g_orig_AppendMenuA)},
        {"ModifyMenuW", reinterpret_cast<void*>(&HookedModifyMenuW),
         reinterpret_cast<void**>(&g_orig_ModifyMenuW)},
        {"ModifyMenuA", reinterpret_cast<void*>(&HookedModifyMenuA),
         reinterpret_cast<void**>(&g_orig_ModifyMenuA)},
        {"SetDlgItemTextW", reinterpret_cast<void*>(&HookedSetDlgItemTextW),
         reinterpret_cast<void**>(&g_orig_SetDlgItemTextW)},
        {"SetDlgItemTextA", reinterpret_cast<void*>(&HookedSetDlgItemTextA),
         reinterpret_cast<void**>(&g_orig_SetDlgItemTextA)},
        {"DialogBoxParamW", reinterpret_cast<void*>(&HookedDialogBoxParamW),
         reinterpret_cast<void**>(&g_orig_DialogBoxParamW)},
        {"DialogBoxParamA", reinterpret_cast<void*>(&HookedDialogBoxParamA),
         reinterpret_cast<void**>(&g_orig_DialogBoxParamA)},
        {"DialogBoxIndirectParamW",
         reinterpret_cast<void*>(&HookedDialogBoxIndirectParamW),
         reinterpret_cast<void**>(&g_orig_DialogBoxIndirectParamW)},
        {"DialogBoxIndirectParamA",
         reinterpret_cast<void*>(&HookedDialogBoxIndirectParamA),
         reinterpret_cast<void**>(&g_orig_DialogBoxIndirectParamA)},
        {"CreateDialogParamW",
         reinterpret_cast<void*>(&HookedCreateDialogParamW),
         reinterpret_cast<void**>(&g_orig_CreateDialogParamW)},
        {"CreateDialogParamA",
         reinterpret_cast<void*>(&HookedCreateDialogParamA),
         reinterpret_cast<void**>(&g_orig_CreateDialogParamA)},
        {"CreateDialogIndirectParamW",
         reinterpret_cast<void*>(&HookedCreateDialogIndirectParamW),
         reinterpret_cast<void**>(&g_orig_CreateDialogIndirectParamW)},
        {"CreateDialogIndirectParamA",
         reinterpret_cast<void*>(&HookedCreateDialogIndirectParamA),
         reinterpret_cast<void**>(&g_orig_CreateDialogIndirectParamA)},
    };

    std::size_t patched_count = 0u;
    const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + imp_dir.VirtualAddress);

    for (; desc->Name != 0u && desc->Name < image_size; ++desc) {
        const auto* dll_name = reinterpret_cast<const char*>(base + desc->Name);
        if (!EqualsIgnoreCaseAscii(dll_name, "USER32.dll")) {
            continue;
        }
        if (desc->OriginalFirstThunk == 0u || desc->FirstThunk == 0u ||
            desc->OriginalFirstThunk >= image_size ||
            desc->FirstThunk >= image_size) {
            continue;
        }

        const auto* int_thunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(
            base + desc->OriginalFirstThunk);
        auto* iat_thunk =
            reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

        for (; int_thunk->u1.AddressOfData != 0u &&
               iat_thunk->u1.Function != 0u;
             ++int_thunk, ++iat_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(int_thunk->u1.Ordinal)) {
                continue;
            }
            const ULONG_PTR name_rva = int_thunk->u1.AddressOfData;
            if (name_rva >= image_size) {
                continue;
            }
            const auto* by_name =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name_rva);
            const char* func_name =
                reinterpret_cast<const char*>(by_name->Name);

            for (const auto& binding : bindings) {
                if (std::strcmp(func_name, binding.export_name) == 0) {
                    if (PatchSingleIatEntry(iat_thunk, binding.detour_fn,
                                            binding.orig_slot)) {
                        ++patched_count;
                    }
                    break;
                }
            }
        }
    }

    Log("[Chrome] Patched %zu USER32.dll IAT slot(s) in host executable",
        patched_count);
    return true;
}

bool Win32ChromeInit() noexcept {
    return InstallWindowChromeHooks(::GetModuleHandleW(nullptr));
}

void Win32ChromeShutdown() noexcept {
    if (g_dialog_font != nullptr) {
        ::DeleteObject(g_dialog_font);
        g_dialog_font = nullptr;
    }
    ::InitOnceInitialize(&g_dialog_font_once);
}

}  // namespace win32_chrome
