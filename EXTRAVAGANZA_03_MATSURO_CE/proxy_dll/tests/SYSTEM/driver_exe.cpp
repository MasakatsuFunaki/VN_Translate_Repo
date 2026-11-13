// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Minimal fake host EXE for the SYSTEM gtest.
//
// Spawns with argv[1] = absolute path to the built proxy DLL, calls
// LoadLibraryW so DllMain(DLL_PROCESS_ATTACH) flushes the banner synchronously,
// and exits without FreeLibrary so the OS terminates any worker threads cleanly
// prior to DLL_PROCESS_DETACH.

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

constexpr int kExpectedMinArgc = 2;
constexpr int kDllPathArgIndex = 1;
constexpr int kExitOk = 0;
constexpr int kExitBadArgv = 2;
constexpr int kExitArgvParseFailed = 3;
constexpr int kExitLoadFailed = 4;

struct LocalFreeDeleter {
    void operator()(wchar_t** ptr) const noexcept {
        if (ptr != nullptr) {
            LocalFree(ptr);
        }
    }
};

}  // namespace

int main() {
    int argc = 0;
    std::unique_ptr<wchar_t*, LocalFreeDeleter> wargv(
        CommandLineToArgvW(GetCommandLineW(), &argc));
    if (!wargv) {
        std::fprintf(stderr, "CommandLineToArgvW failed gle=%lu\n", GetLastError());
        return kExitArgvParseFailed;
    }
    if (argc < kExpectedMinArgc) {
        std::fwprintf(stderr, L"usage: %ls <proxy_dll_path>\n", wargv.get()[0]);
        return kExitBadArgv;
    }

    const wchar_t* dll_path = wargv.get()[kDllPathArgIndex];
    const HMODULE module = LoadLibraryW(dll_path);
    if (!module) {
        std::fwprintf(stderr, L"LoadLibraryW(\"%ls\") failed gle=%lu\n",
                      dll_path, GetLastError());
        return kExitLoadFailed;
    }
    return kExitOk;
}
