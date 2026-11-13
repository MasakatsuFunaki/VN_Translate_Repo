// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Minimal host executable for the proxy DLL system test.
//
// Spawns with argv[1] set to the built proxy DLL path via the wide Win32
// command line. LoadLibraryW runs DllMain(DLL_PROCESS_ATTACH) synchronously,
// flushing the startup banner to proxy_log.txt before returning.
//
// Deliberately exits without calling FreeLibrary: letting the OS terminate
// any worker threads before running DLL_PROCESS_DETACH matches normal game
// process shutdown and avoids races against unloaded code pages.
//
// Exit codes:
//   0: clean LoadLibraryW (DllMain ATTACH ran)
//   2: missing or invalid command-line arguments
//   4: LoadLibraryW failed (GetLastError printed to stderr)
//   5: unhandled C++ exception

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <memory>

namespace {

constexpr int kExitSuccess = 0;
constexpr int kExitBadArgv = 2;
constexpr int kExitLoadFailed = 4;
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

using ScopedArgvW = std::unique_ptr<wchar_t*, LocalFreeDeleter>;

}  // namespace

int main() {
    try {
        const wchar_t* const cmdline = GetCommandLineW();
        int wargc = 0;
        const ScopedArgvW wargv(
            (cmdline != nullptr && *cmdline != L'\0')
                ? CommandLineToArgvW(cmdline, &wargc)
                : nullptr);
        if (wargv == nullptr || wargc < kMinRequiredArgs) {
            std::fprintf(stderr, "usage: amakano2pe_driver <proxy_dll_path>\n");
            return kExitBadArgv;
        }

        const HMODULE mod = LoadLibraryW(wargv.get()[kDllPathArgIndex]);
        if (mod == nullptr) {
            const DWORD err = GetLastError();
            std::fprintf(stderr, "LoadLibraryW failed gle=%lu\n", err);
            return kExitLoadFailed;
        }
        return kExitSuccess;
    } catch (...) {
        return kExitException;
    }
}
