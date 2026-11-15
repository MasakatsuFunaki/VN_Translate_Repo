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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace win32_chrome {

namespace {

constexpr UINT kCodePage932 = 932u;
constexpr int kDialogFontHeight9PtAt96Dpi = -12;  // -MulDiv(9, 96, 72)
constexpr wchar_t kDialogFontFace[] = L"Segoe UI";
constexpr std::size_t kClassNameBufChars = 64u;
constexpr std::size_t kMaxStolenBytes = 12u;
constexpr std::size_t kTrampolineCapacity = 32u;

using MessageBoxWFn = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
using MessageBoxAFn = int(WINAPI*)(HWND, LPCSTR, LPCSTR, UINT);
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
using SetDlgItemTextWFn = BOOL(WINAPI*)(HWND, int, LPCWSTR);

MessageBoxWFn g_orig_MessageBoxW = nullptr;
MessageBoxAFn g_orig_MessageBoxA = nullptr;
DialogBoxParamWFn g_orig_DialogBoxParamW = nullptr;
DialogBoxParamAFn g_orig_DialogBoxParamA = nullptr;
DialogBoxIndirectParamWFn g_orig_DialogBoxIndirectParamW = nullptr;
DialogBoxIndirectParamAFn g_orig_DialogBoxIndirectParamA = nullptr;
CreateDialogParamWFn g_orig_CreateDialogParamW = nullptr;
CreateDialogParamAFn g_orig_CreateDialogParamA = nullptr;
CreateDialogIndirectParamWFn g_orig_CreateDialogIndirectParamW = nullptr;
CreateDialogIndirectParamAFn g_orig_CreateDialogIndirectParamA = nullptr;
SetDlgItemTextWFn g_orig_SetDlgItemTextW = nullptr;

INIT_ONCE g_dialog_font_once = INIT_ONCE_STATIC_INIT;
HFONT g_dialog_font = nullptr;

struct ThreadDialogObserverState {
    HHOOK hook = nullptr;
    std::size_t depth = 0u;
    bool localizing = false;
};

thread_local ThreadDialogObserverState g_tls_dialog_state;

bool IsValidStringPtr(const void* ptr) noexcept {
    return ptr != nullptr && !IS_INTRESOURCE(ptr);
}

std::wstring AnsiCp932ToWide(const char* ansi) {
    if (!IsValidStringPtr(ansi) || *ansi == '\0') {
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

BOOL CALLBACK InitDialogFontCallback(PINIT_ONCE /*init_once*/,
                                     PVOID /*param*/,
                                     PVOID* /*ctx*/) noexcept {
    // Use CreateFontIndirectExW so translator.cpp's CreateFontIndirectW hook
    // (which scales game fonts to 60%) does not shrink Win32 dialog UI text.
    ENUMLOGFONTEXDVW elf{};
    LOGFONTW& lf = elf.elfEnumLogfontEx.elfLogFont;
    lf.lfHeight = kDialogFontHeight9PtAt96Dpi;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcsncpy_s(lf.lfFaceName, kDialogFontFace, _TRUNCATE);
    g_dialog_font = ::CreateFontIndirectExW(&elf);
    return TRUE;
}

HFONT GetOrCreateDialogFont() noexcept {
    ::InitOnceExecuteOnce(&g_dialog_font_once, InitDialogFontCallback, nullptr,
                          nullptr);
    return g_dialog_font;
}

std::wstring ReadWindowTextWide(HWND hwnd) {
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

bool IsStandardDialogControlClass(const wchar_t* cls) noexcept {
    return ::_wcsicmp(cls, L"Static") == 0 || ::_wcsicmp(cls, L"Button") == 0;
}

BOOL CALLBACK LocalizeDialogChildProc(HWND child, LPARAM lparam) {
    const auto font = reinterpret_cast<HFONT>(lparam);
    wchar_t cls[kClassNameBufChars] = {};
    if (::GetClassNameW(child, cls, static_cast<int>(kClassNameBufChars)) > 0 &&
        IsStandardDialogControlClass(cls)) {
        if (font != nullptr) {
            ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                           TRUE);
        }
        const std::wstring current = ReadWindowTextWide(child);
        if (!current.empty()) {
            if (const auto en = chrome_text_logic::TranslateDialogText(current)) {
                ::SetWindowTextW(child, en->c_str());
            }
        }
    }
    return TRUE;
}

void LocalizeDialogWindow(HWND dlg_hwnd) {
    if (dlg_hwnd == nullptr || g_tls_dialog_state.localizing) {
        return;
    }
    g_tls_dialog_state.localizing = true;

    const HFONT font = GetOrCreateDialogFont();
    const std::wstring caption = ReadWindowTextWide(dlg_hwnd);
    if (!caption.empty()) {
        if (const auto en = chrome_text_logic::TranslateDialogText(caption)) {
            ::SetWindowTextW(dlg_hwnd, en->c_str());
        }
    }
    ::EnumChildWindows(dlg_hwnd, LocalizeDialogChildProc,
                       reinterpret_cast<LPARAM>(font));

    g_tls_dialog_state.localizing = false;
}

LRESULT CALLBACK DialogCallWndProcRet(int code,
                                      WPARAM wparam,
                                      LPARAM lparam) noexcept {
    if (code >= 0 && lparam != 0) {
        const auto* msg = reinterpret_cast<const CWPRETSTRUCT*>(lparam);
        if (msg->message == WM_INITDIALOG && msg->hwnd != nullptr) {
            LocalizeDialogWindow(msg->hwnd);
        }
    }
    return ::CallNextHookEx(g_tls_dialog_state.hook, code, wparam, lparam);
}

class ScopedDialogObserver {
public:
    ScopedDialogObserver() noexcept {
        if (g_tls_dialog_state.depth == 0u &&
            g_tls_dialog_state.hook == nullptr) {
            g_tls_dialog_state.hook = ::SetWindowsHookExW(
                WH_CALLWNDPROCRET, DialogCallWndProcRet, nullptr,
                ::GetCurrentThreadId());
        }
        ++g_tls_dialog_state.depth;
    }

    ~ScopedDialogObserver() noexcept {
        if (g_tls_dialog_state.depth > 0u) {
            --g_tls_dialog_state.depth;
        }
        if (g_tls_dialog_state.depth == 0u &&
            g_tls_dialog_state.hook != nullptr) {
            ::UnhookWindowsHookEx(g_tls_dialog_state.hook);
            g_tls_dialog_state.hook = nullptr;
        }
    }

    ScopedDialogObserver(const ScopedDialogObserver&) = delete;
    ScopedDialogObserver& operator=(const ScopedDialogObserver&) = delete;
};

int WINAPI HookedMessageBoxW(HWND hwnd,
                             LPCWSTR text,
                             LPCWSTR caption,
                             UINT type) {
    std::optional<std::wstring> en_text;
    std::optional<std::wstring> en_caption;

    if (IsValidStringPtr(text)) {
        en_text = chrome_text_logic::TranslateMessageBoxBody(text);
    }
    if (IsValidStringPtr(caption)) {
        en_caption = chrome_text_logic::TranslateMessageBoxCaption(caption);
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
    const std::wstring wide_cap = AnsiCp932ToWide(caption);
    const auto en_text =
        wide_text.empty()
            ? std::nullopt
            : chrome_text_logic::TranslateMessageBoxBody(wide_text);
    const auto en_cap =
        wide_cap.empty()
            ? std::nullopt
            : chrome_text_logic::TranslateMessageBoxCaption(wide_cap);

    if (!en_text.has_value() && !en_cap.has_value()) {
        return g_orig_MessageBoxA(hwnd, text, caption, type);
    }

    const LPCWSTR final_text =
        en_text.has_value()
            ? en_text->c_str()
            : (wide_text.empty() ? nullptr : wide_text.c_str());
    const LPCWSTR final_cap =
        en_cap.has_value()
            ? en_cap->c_str()
            : (wide_cap.empty() ? nullptr : wide_cap.c_str());
    return HookedMessageBoxW(hwnd, final_text, final_cap, type);
}

INT_PTR WINAPI HookedDialogBoxParamW(HINSTANCE inst,
                                     LPCWSTR tmpl,
                                     HWND parent,
                                     DLGPROC proc,
                                     LPARAM param) {
    ScopedDialogObserver observer;
    return g_orig_DialogBoxParamW(inst, tmpl, parent, proc, param);
}

INT_PTR WINAPI HookedDialogBoxParamA(HINSTANCE inst,
                                     LPCSTR tmpl,
                                     HWND parent,
                                     DLGPROC proc,
                                     LPARAM param) {
    ScopedDialogObserver observer;
    return g_orig_DialogBoxParamA(inst, tmpl, parent, proc, param);
}

INT_PTR WINAPI HookedDialogBoxIndirectParamW(HINSTANCE inst,
                                             LPCDLGTEMPLATEW tmpl,
                                             HWND parent,
                                             DLGPROC proc,
                                             LPARAM param) {
    ScopedDialogObserver observer;
    return g_orig_DialogBoxIndirectParamW(inst, tmpl, parent, proc, param);
}

INT_PTR WINAPI HookedDialogBoxIndirectParamA(HINSTANCE inst,
                                             LPCDLGTEMPLATEA tmpl,
                                             HWND parent,
                                             DLGPROC proc,
                                             LPARAM param) {
    ScopedDialogObserver observer;
    return g_orig_DialogBoxIndirectParamA(inst, tmpl, parent, proc, param);
}

HWND WINAPI HookedCreateDialogParamW(HINSTANCE inst,
                                     LPCWSTR tmpl,
                                     HWND parent,
                                     DLGPROC proc,
                                     LPARAM param) {
    ScopedDialogObserver observer;
    return g_orig_CreateDialogParamW(inst, tmpl, parent, proc, param);
}

HWND WINAPI HookedCreateDialogParamA(HINSTANCE inst,
                                     LPCSTR tmpl,
                                     HWND parent,
                                     DLGPROC proc,
                                     LPARAM param) {
    ScopedDialogObserver observer;
    return g_orig_CreateDialogParamA(inst, tmpl, parent, proc, param);
}

HWND WINAPI HookedCreateDialogIndirectParamW(HINSTANCE inst,
                                             LPCDLGTEMPLATEW tmpl,
                                             HWND parent,
                                             DLGPROC proc,
                                             LPARAM param) {
    ScopedDialogObserver observer;
    return g_orig_CreateDialogIndirectParamW(inst, tmpl, parent, proc, param);
}

HWND WINAPI HookedCreateDialogIndirectParamA(HINSTANCE inst,
                                             LPCDLGTEMPLATEA tmpl,
                                             HWND parent,
                                             DLGPROC proc,
                                             LPARAM param) {
    ScopedDialogObserver observer;
    return g_orig_CreateDialogIndirectParamA(inst, tmpl, parent, proc, param);
}

BOOL WINAPI HookedSetDlgItemTextW(HWND hdlg, int ctrl_id, LPCWSTR text) {
    if (IsValidStringPtr(text)) {
        if (const auto en = chrome_text_logic::TranslateDialogText(text)) {
            return g_orig_SetDlgItemTextW(hdlg, ctrl_id, en->c_str());
        }
    }
    return g_orig_SetDlgItemTextW(hdlg, ctrl_id, text);
}

// Determines a clean >= 5-byte x86 instruction boundary for standard Win32 API
// prologues without cutting mid-instruction. Returns 0 if unrecognized.
std::size_t MeasureSafeWin32Prologue(std::uint8_t* p,
                                     std::uint8_t*& out_patch_site,
                                     bool& out_is_rel_jmp,
                                     std::uintptr_t& out_jmp_target) noexcept {
    out_patch_site = p;
    out_is_rel_jmp = false;
    out_jmp_target = 0u;

    // 8B FF 55 8B EC: mov edi, edi; push ebp; mov ebp, esp (5 bytes)
    if (p[0] == 0x8Bu && p[1] == 0xFFu && p[2] == 0x55u && p[3] == 0x8Bu &&
        p[4] == 0xECu) {
        return 5u;
    }

    // FF 25 [imm32]: indirect jmp dword ptr [imm32] (6 bytes, position-independent on x86)
    if (p[0] == 0xFFu && p[1] == 0x25u) {
        return 6u;
    }

    // Already detoured with E9 rel32 (e.g. Steam / Discord overlay): chain it.
    if (p[0] == 0xE9u) {
        std::int32_t rel = 0;
        std::memcpy(&rel, p + 1, sizeof(rel));
        out_is_rel_jmp = true;
        out_jmp_target = reinterpret_cast<std::uintptr_t>(p + 5) +
                         static_cast<std::intptr_t>(rel);
        return 5u;
    }

    // Hotpatched EB F9 (-7) jumping to E9 rel32 at p - 5: redirect p - 5 so
    // p + 2 (the previous hook's return landing site) stays untouched.
    if (p[0] == 0xEBu && p[1] == 0xF9u && p[-5] == 0xE9u) {
        std::int32_t rel = 0;
        std::memcpy(&rel, p - 4, sizeof(rel));
        out_patch_site = p - 5;
        out_is_rel_jmp = true;
        out_jmp_target = reinterpret_cast<std::uintptr_t>(p) +
                         static_cast<std::intptr_t>(rel);
        return 5u;
    }

    // 55 8B EC ... (push ebp; mov ebp, esp; ...)
    if (p[0] == 0x55u && p[1] == 0x8Bu && p[2] == 0xECu) {
        // 83 EC ib / 83 E4 ib (sub/and esp, imm8) -> 6 bytes
        if (p[3] == 0x83u && (p[4] == 0xECu || p[4] == 0xE4u)) {
            return 6u;
        }
        // 81 EC id (sub esp, imm32) -> 9 bytes
        if (p[3] == 0x81u && p[4] == 0xECu) {
            return 9u;
        }
        // 6A ib (push imm8) -> 5 bytes
        if (p[3] == 0x6Au) {
            return 5u;
        }
        // 68 id (push imm32) -> 8 bytes
        if (p[3] == 0x68u) {
            return 8u;
        }
        // Two single-byte pushes (50..57) -> 5 bytes
        if ((p[3] >= 0x50u && p[3] <= 0x57u) &&
            (p[4] >= 0x50u && p[4] <= 0x57u)) {
            return 5u;
        }
    }

    // 6A ib 68 id (SEH frame: push imm8; push imm32) -> 7 bytes
    if (p[0] == 0x6Au && p[2] == 0x68u) {
        return 7u;
    }

    return 0u;
}

struct InstalledDetour {
    std::uint8_t* target = nullptr;
    std::uint8_t* trampoline = nullptr;
    std::size_t stolen_len = 0u;
    std::array<std::uint8_t, kMaxStolenBytes> original_bytes{};
    std::array<std::uint8_t, 5u> written_patch{};
    void** orig_fn_slot = nullptr;
};

}  // namespace

struct Win32ChromeController::Impl {
    bool installed = false;
    std::vector<InstalledDetour> detours;

    ~Impl() { Shutdown(); }

    bool InstallDetour(HMODULE user32,
                       const char* export_name,
                       void* hook_fn,
                       void** orig_slot) noexcept {
        auto* proc = reinterpret_cast<std::uint8_t*>(
            ::GetProcAddress(user32, export_name));
        if (proc == nullptr) {
            Log("WIN32-CHROME: user32!%s not found", export_name);
            return false;
        }

        std::uint8_t* patch_site = proc;
        bool is_rel_jmp = false;
        std::uintptr_t jmp_target = 0u;
        const std::size_t stolen_len = MeasureSafeWin32Prologue(
            proc, patch_site, is_rel_jmp, jmp_target);
        if (stolen_len < 5u || stolen_len > kMaxStolenBytes) {
            Log("WIN32-CHROME: unrecognized prologue %02X %02X %02X %02X %02X "
                "at user32!%s — skipping",
                proc[0], proc[1], proc[2], proc[3], proc[4], export_name);
            return false;
        }

        auto* tramp = static_cast<std::uint8_t*>(::VirtualAlloc(
            nullptr, kTrampolineCapacity, MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));
        if (tramp == nullptr) {
            Log("WIN32-CHROME: VirtualAlloc failed for user32!%s", export_name);
            return false;
        }

        InstalledDetour rec{};
        rec.target = patch_site;
        rec.trampoline = tramp;
        rec.stolen_len = stolen_len;
        rec.orig_fn_slot = orig_slot;
        std::memcpy(rec.original_bytes.data(), patch_site, stolen_len);

        if (is_rel_jmp) {
            tramp[0] = 0xE9u;
            const auto rel = static_cast<std::int32_t>(
                jmp_target - reinterpret_cast<std::uintptr_t>(tramp + 5));
            std::memcpy(tramp + 1, &rel, sizeof(rel));
        } else {
            std::memcpy(tramp, patch_site, stolen_len);
            tramp[stolen_len] = 0xE9u;
            const auto rel = static_cast<std::int32_t>(
                reinterpret_cast<std::uintptr_t>(patch_site + stolen_len) -
                reinterpret_cast<std::uintptr_t>(tramp + stolen_len + 5));
            std::memcpy(tramp + stolen_len + 1, &rel, sizeof(rel));
        }
        ::FlushInstructionCache(::GetCurrentProcess(), tramp,
                                kTrampolineCapacity);

        DWORD old_prot = 0u;
        if (!::VirtualProtect(patch_site, 5u, PAGE_EXECUTE_READWRITE,
                              &old_prot)) {
            ::VirtualFree(tramp, 0u, MEM_RELEASE);
            Log("WIN32-CHROME: VirtualProtect failed at user32!%s", export_name);
            return false;
        }

        *orig_slot = tramp;
        rec.written_patch[0] = 0xE9u;
        const auto hook_rel = static_cast<std::int32_t>(
            reinterpret_cast<std::uintptr_t>(hook_fn) -
            reinterpret_cast<std::uintptr_t>(patch_site + 5));
        std::memcpy(rec.written_patch.data() + 1, &hook_rel, sizeof(hook_rel));
        std::memcpy(patch_site, rec.written_patch.data(), 5u);

        DWORD dummy = 0u;
        ::VirtualProtect(patch_site, 5u, old_prot, &dummy);
        ::FlushInstructionCache(::GetCurrentProcess(), patch_site, 5u);

        detours.push_back(rec);
        return true;
    }

    bool Install() noexcept {
        if (installed) {
            return true;
        }

        // user32.dll is statically imported by xinput1_3.dll, so it is already
        // mapped before DllMain(DLL_PROCESS_ATTACH) runs; avoid LoadLibraryW
        // under the OS loader lock.
        HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (user32 == nullptr) {
            Log("WIN32-CHROME: user32.dll unavailable");
            return false;
        }

        struct Binding {
            const char* name;
            void* hook;
            void** orig;
        };

        const Binding kBindings[] = {
            {"MessageBoxW", reinterpret_cast<void*>(&HookedMessageBoxW),
             reinterpret_cast<void**>(&g_orig_MessageBoxW)},
            {"MessageBoxA", reinterpret_cast<void*>(&HookedMessageBoxA),
             reinterpret_cast<void**>(&g_orig_MessageBoxA)},
            {"DialogBoxParamW",
             reinterpret_cast<void*>(&HookedDialogBoxParamW),
             reinterpret_cast<void**>(&g_orig_DialogBoxParamW)},
            {"DialogBoxParamA",
             reinterpret_cast<void*>(&HookedDialogBoxParamA),
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
            {"SetDlgItemTextW",
             reinterpret_cast<void*>(&HookedSetDlgItemTextW),
             reinterpret_cast<void**>(&g_orig_SetDlgItemTextW)},
        };

        std::size_t hooked = 0u;
        for (const auto& b : kBindings) {
            if (InstallDetour(user32, b.name, b.hook, b.orig)) {
                ++hooked;
            }
        }

        installed = (hooked > 0u);
        Log("WIN32-CHROME: installed %zu/%zu user32 hooks", hooked,
            sizeof(kBindings) / sizeof(kBindings[0]));
        return installed;
    }

    void Shutdown() noexcept {
        const bool user32_loaded =
            (::GetModuleHandleW(L"user32.dll") != nullptr);
        for (auto it = detours.rbegin(); it != detours.rend(); ++it) {
            if (user32_loaded && it->target != nullptr &&
                std::memcmp(it->target, it->written_patch.data(), 5u) == 0) {
                DWORD old_prot = 0u;
                if (::VirtualProtect(it->target, it->stolen_len,
                                     PAGE_EXECUTE_READWRITE, &old_prot)) {
                    std::memcpy(it->target, it->original_bytes.data(),
                                it->stolen_len);
                    DWORD dummy = 0u;
                    ::VirtualProtect(it->target, it->stolen_len, old_prot,
                                     &dummy);
                    ::FlushInstructionCache(::GetCurrentProcess(), it->target,
                                            it->stolen_len);
                }
                if (it->orig_fn_slot != nullptr) {
                    *it->orig_fn_slot = nullptr;
                }
                if (it->trampoline != nullptr) {
                    ::VirtualFree(it->trampoline, 0u, MEM_RELEASE);
                }
            }
        }
        detours.clear();

        if (g_dialog_font != nullptr) {
            if (::GetModuleHandleW(L"gdi32.dll") != nullptr) {
                ::DeleteObject(g_dialog_font);
            }
            g_dialog_font = nullptr;
            ::InitOnceInitialize(&g_dialog_font_once);
        }

        installed = false;
    }
};

Win32ChromeController::Win32ChromeController()
    : impl_(std::make_unique<Impl>()) {}

Win32ChromeController::~Win32ChromeController() = default;

Win32ChromeController::Win32ChromeController(
    Win32ChromeController&&) noexcept = default;

Win32ChromeController& Win32ChromeController::operator=(
    Win32ChromeController&&) noexcept = default;

bool Win32ChromeController::Install() noexcept {
    return impl_ != nullptr && impl_->Install();
}

void Win32ChromeController::Shutdown() noexcept {
    if (impl_ != nullptr) {
        impl_->Shutdown();
    }
}

bool Win32ChromeController::IsInstalled() const noexcept {
    return impl_ != nullptr && impl_->installed;
}

namespace {
Win32ChromeController& GlobalController() {
    static Win32ChromeController controller;
    return controller;
}
}  // namespace

bool InstallWin32ChromeHooks() noexcept {
    return GlobalController().Install();
}

void ShutdownWin32ChromeHooks() noexcept {
    GlobalController().Shutdown();
}

}  // namespace win32_chrome
