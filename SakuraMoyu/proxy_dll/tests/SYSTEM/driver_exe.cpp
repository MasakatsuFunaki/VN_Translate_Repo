// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Minimal fake host executable for the proxy DLL black-box test.
//
// Parses the process wide command line via GetCommandLineW + CommandLineToArgvW
// and calls LoadLibraryW on argv[1] so DllMain ATTACH runs synchronously and
// flushes the startup banner to proxy_log.txt.
//
// Exits without calling FreeLibrary on purpose: DllMain ATTACH spawns a
// background scout thread, and letting the OS terminate worker threads at
// process exit before running DllMain DETACH mirrors real game shutdown without
// unloading code pages under a live thread.
//
// Exit codes:
//   0 -- LoadLibraryW succeeded (DllMain ATTACH ran)
//   2 -- missing or invalid command-line arguments
//   4 -- LoadLibraryW failed
//   5 -- unhandled exception
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <memory>

namespace {

constexpr int kExitSuccess = 0;
constexpr int kExitBadArgs = 2;
constexpr int kExitLoadLibraryFailed = 4;
constexpr int kExitException = 5;

constexpr int kMinRequiredArgs = 2;
constexpr int kDllPathArgIndex = 1;

struct LocalFreeDeleter {
    void operator()(wchar_t** argv) const noexcept {
        if (argv != nullptr) {
            LocalFree(argv);
        }
    }
};

}  // namespace

int main() {
    try {
        const wchar_t* const cmdline = GetCommandLineW();
        if (cmdline == nullptr || *cmdline == L'\0') {
            std::fwprintf(stderr, L"usage: favorite_driver <proxy_dll_path>\n");
            return kExitBadArgs;
        }

        int wargc = 0;
        const std::unique_ptr<wchar_t*, LocalFreeDeleter> wargv(
            CommandLineToArgvW(cmdline, &wargc));
        if (!wargv || wargc < kMinRequiredArgs) {
            std::fwprintf(stderr, L"usage: favorite_driver <proxy_dll_path>\n");
            return kExitBadArgs;
        }

        const wchar_t* const dll_path = wargv.get()[kDllPathArgIndex];
        const HMODULE loaded = LoadLibraryW(dll_path);
        if (loaded == nullptr) {
            std::fwprintf(stderr, L"LoadLibraryW(\"%ls\") failed gle=%lu\n",
                          dll_path, GetLastError());
            return kExitLoadLibraryFailed;
        }
        return kExitSuccess;
    } catch (...) {
        return kExitException;
    }
}
