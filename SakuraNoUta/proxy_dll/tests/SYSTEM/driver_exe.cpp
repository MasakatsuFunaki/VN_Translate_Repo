// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Minimal host for the SYSTEM gtest: decode the wide Win32 command line,
// LoadLibraryW the proxy DLL, and exit without FreeLibrary to mirror real
// game shutdown.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <memory>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsageError = 2;
constexpr int kExitArgvDecodeError = 3;
constexpr int kExitLoadLibraryError = 4;
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
        if (cmdline == nullptr || *cmdline == L'\0') {
            std::fprintf(stderr, "GetCommandLineW returned empty command line\n");
            return kExitArgvDecodeError;
        }
        int wargc = 0;
        const ScopedArgvW wargv(CommandLineToArgvW(cmdline, &wargc));
        if (!wargv) {
            std::fprintf(stderr, "CommandLineToArgvW failed gle=%lu\n",
                         GetLastError());
            return kExitArgvDecodeError;
        }
        if (wargc < kMinRequiredArgs) {
            std::fprintf(stderr, "usage: sakuranouta_driver <proxy_dll_path>\n");
            return kExitUsageError;
        }
        const wchar_t* const dll_path = wargv.get()[kDllPathArgIndex];
        if (dll_path == nullptr || *dll_path == L'\0') {
            std::fprintf(stderr, "empty proxy_dll_path argument\n");
            return kExitUsageError;
        }

        const HMODULE module = LoadLibraryW(dll_path);
        if (module == nullptr) {
            std::fprintf(stderr, "LoadLibraryW failed gle=%lu\n", GetLastError());
            return kExitLoadLibraryError;
        }
        // Deliberately omit FreeLibrary to mirror real game shutdown.
        return kExitOk;
    } catch (...) {
        return kExitException;
    }
}
