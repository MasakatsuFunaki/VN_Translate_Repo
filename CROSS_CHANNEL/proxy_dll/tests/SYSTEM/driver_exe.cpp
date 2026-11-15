// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Minimal host executable for the proxy DLL system test.
//
// Loads the target proxy DLL via LoadLibraryW(wargv[1]) so DllMain ATTACH runs
// synchronously and flushes the startup banner to proxy_log.txt, then exits the
// process cleanly without FreeLibrary so OS process teardown terminates any
// background worker thread before DllMain DETACH runs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <string>
#include <vector>

namespace {

constexpr int kExitClean = 0;
constexpr int kExitBadArgv = 2;
constexpr int kExitArgParseFailed = 3;
constexpr int kExitLoadLibraryFailed = 4;
constexpr int kExitUnhandledException = 5;
constexpr int kExitChromeProbeFailed = 6;
constexpr int kMinArgCount = 2;
constexpr int kAboutVersionCtrlId = 1001;

class LocalFreeGuard {
public:
    explicit LocalFreeGuard(LPWSTR* ptr) : ptr_(ptr) {}
    ~LocalFreeGuard() {
        if (ptr_ != nullptr) {
            LocalFree(ptr_);
        }
    }
    LocalFreeGuard(const LocalFreeGuard&) = delete;
    LocalFreeGuard& operator=(const LocalFreeGuard&) = delete;

private:
    LPWSTR* ptr_;
};

void AppendWideZ(std::vector<WORD>& words, const wchar_t* s) {
    while (*s != L'\0') {
        words.push_back(static_cast<WORD>(*s++));
    }
    words.push_back(0u);
}

void AlignDword(std::vector<WORD>& words) {
    if ((words.size() & 1u) != 0u) {
        words.push_back(0u);
    }
}

void AppendDialogItem(std::vector<WORD>& words,
                      DWORD style,
                      WORD id,
                      WORD atom_class,
                      const wchar_t* text) {
    AlignDword(words);
    DLGITEMTEMPLATE item{};
    item.style = style;
    item.dwExtendedStyle = 0u;
    item.x = 4;
    item.y = 4;
    item.cx = 120;
    item.cy = 14;
    item.id = id;
    const auto* raw = reinterpret_cast<const WORD*>(&item);
    words.insert(words.end(), raw, raw + (sizeof(item) / sizeof(WORD)));
    words.push_back(0xFFFFu);
    words.push_back(atom_class);
    AppendWideZ(words, text);
    words.push_back(0u);  // no creation data
}

INT_PTR CALLBACK ProbeDialogProc(HWND, UINT msg, WPARAM, LPARAM) {
    return msg == WM_INITDIALOG ? TRUE : FALSE;
}

std::string WideToUtf8(const wchar_t* w) {
    if (w == nullptr || *w == L'\0') {
        return {};
    }
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

bool RunWin32ChromeDialogProbe(const wchar_t* out_path) {
    std::vector<WORD> tmpl;
    DLGTEMPLATE hdr{};
    hdr.style = WS_POPUP | WS_CAPTION;  // hidden: no WS_VISIBLE
    hdr.dwExtendedStyle = 0u;
    hdr.cdit = 2u;
    hdr.x = 0;
    hdr.y = 0;
    hdr.cx = 180;
    hdr.cy = 80;
    const auto* hdr_words = reinterpret_cast<const WORD*>(&hdr);
    tmpl.insert(tmpl.end(), hdr_words, hdr_words + (sizeof(hdr) / sizeof(WORD)));
    tmpl.push_back(0u);  // no menu
    tmpl.push_back(0u);  // default dialog class
    AppendWideZ(tmpl, L"バージョン情報 ph");

    AppendDialogItem(tmpl, WS_CHILD, kAboutVersionCtrlId, 0x0082u,
                     L"cc, Version 1.0");
    AppendDialogItem(tmpl, WS_CHILD | BS_DEFPUSHBUTTON, IDOK, 0x0080u, L"OK");

    HWND dlg = CreateDialogIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(tmpl.data()),
        nullptr,
        ProbeDialogProc,
        0);
    if (dlg == nullptr) {
        return false;
    }

    wchar_t caption[256] = {};
    wchar_t ver_text[256] = {};
    wchar_t ok_text[256] = {};
    GetWindowTextW(dlg, caption, 256);
    GetDlgItemTextW(dlg, kAboutVersionCtrlId, ver_text, 256);
    GetDlgItemTextW(dlg, IDOK, ok_text, 256);

    wchar_t face_name[LF_FACESIZE] = {};
    int font_height = 0;
    HWND static_ctrl = GetDlgItem(dlg, kAboutVersionCtrlId);
    if (static_ctrl != nullptr) {
        const auto hfont = reinterpret_cast<HFONT>(
            SendMessageW(static_ctrl, WM_GETFONT, 0, 0));
        LOGFONTW lf{};
        if (hfont != nullptr && GetObjectW(hfont, sizeof(lf), &lf) > 0) {
            wcsncpy_s(face_name, lf.lfFaceName, _TRUNCATE);
            font_height = lf.lfHeight;
        }
    }

    DestroyWindow(dlg);

    FILE* fp = _wfopen(out_path, L"wb");
    if (fp == nullptr) {
        return false;
    }
    const std::string report =
        "caption=" + WideToUtf8(caption) + "\n" +
        "version=" + WideToUtf8(ver_text) + "\n" +
        "ok=" + WideToUtf8(ok_text) + "\n" +
        "font_face=" + WideToUtf8(face_name) + "\n" +
        "font_height=" + std::to_string(font_height) + "\n";
    const std::size_t written = std::fwrite(report.data(), 1u, report.size(), fp);
    std::fclose(fp);
    return written == report.size();
}

}  // namespace

int main() {
    try {
        int wargc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv == nullptr) {
            std::fprintf(stderr, "CommandLineToArgvW failed gle=%lu\n", GetLastError());
            return kExitArgParseFailed;
        }
        LocalFreeGuard argv_guard(wargv);

        if (wargc < kMinArgCount || wargv[1] == nullptr || wargv[1][0] == L'\0') {
            std::fprintf(stderr, "usage: cross_channel_driver <proxy_dll_path> [--probe-chrome <out>]\n");
            return kExitBadArgv;
        }

        const HMODULE mod = LoadLibraryW(wargv[1]);
        if (mod == nullptr) {
            std::fprintf(stderr, "LoadLibraryW failed gle=%lu\n", GetLastError());
            return kExitLoadLibraryFailed;
        }

        if (wargc >= 4 && std::wcscmp(wargv[2], L"--probe-chrome") == 0) {
            if (!RunWin32ChromeDialogProbe(wargv[3])) {
                std::fprintf(stderr, "RunWin32ChromeDialogProbe failed gle=%lu\n",
                             GetLastError());
                return kExitChromeProbeFailed;
            }
        }
        return kExitClean;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Unhandled exception: %s\n", e.what());
        return kExitUnhandledException;
    } catch (...) {
        std::fprintf(stderr, "Unhandled non-standard exception\n");
        return kExitUnhandledException;
    }
}
