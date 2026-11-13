// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Minimal host EXE for the SYSTEM black-box test.
//
// Loads the built proxy DLL via LoadLibraryW using the wide UTF-16 command line
// from GetCommandLineW / CommandLineToArgvW so non-ASCII directory paths work
// cleanly. Exits without calling FreeLibrary so background worker threads are
// terminated by the OS before DLL_PROCESS_DETACH runs, matching normal game
// process shutdown.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <memory>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitBadArgs = 2;
constexpr int kExitLoadLibraryFailed = 4;
constexpr int kExitException = 5;
constexpr int kMinRequiredArgs = 2;
constexpr int kDllPathArgIndex = 1;

struct LocalFreeDeleter {
    void operator()(wchar_t** ptr) const noexcept {
        if (ptr != nullptr) {
            LocalFree(ptr);
        }
    }
};

}  // namespace

int main() {
    try {
        const LPWSTR cmdline = GetCommandLineW();
        if (cmdline == nullptr) {
            std::fprintf(stderr, "GetCommandLineW returned null\n");
            return kExitBadArgs;
        }

        int wargc = 0;
        wchar_t** const raw_wargv = CommandLineToArgvW(cmdline, &wargc);
        const std::unique_ptr<wchar_t*, LocalFreeDeleter> wargv(raw_wargv);
        if (wargv == nullptr || wargc < kMinRequiredArgs) {
            std::fprintf(stderr, "usage: sakuranotoki_driver <proxy_dll_path>\n");
            return kExitBadArgs;
        }

        const wchar_t* const dll_path = wargv.get()[kDllPathArgIndex];
        const HMODULE module = LoadLibraryW(dll_path);
        if (module == nullptr) {
            std::fprintf(stderr, "LoadLibraryW failed gle=%lu\n", GetLastError());
            return kExitLoadLibraryFailed;
        }

        return kExitOk;
    } catch (...) {
        return kExitException;
    }
}
