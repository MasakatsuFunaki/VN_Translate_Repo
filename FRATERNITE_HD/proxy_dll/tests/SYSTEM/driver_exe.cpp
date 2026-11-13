// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// driver_exe.cpp -- minimal fake host EXE for the SYSTEM gtest.
//
// The gtest spawns this binary with argv[1] = absolute path to the
// built proxy DLL. We LoadLibraryW the DLL -- DllMain ATTACH runs
// synchronously inside LoadLibraryW, writing the banner to the log
// file before LoadLibraryW returns. We then exit the process WITHOUT
// FreeLibrary on purpose:
//
//   Most proxies spawn a polling worker thread in DllMain ATTACH
//   (patcher, scanner, scout). Calling FreeLibrary here would run
//   DllMain DETACH while that thread is still alive in the DLL's
//   code pages -- the OS does NOT auto-terminate worker threads on
//   FreeLibrary, only on process exit. The thread either crashes
//   the test process after the unload (AV reading freed code) or
//   makes PatcherShutdown's WaitForSingleObject time out.
//
//   By letting the process exit normally instead, the OS kills all
//   non-main threads first, THEN calls DllMain DETACH -- which is
//   how a real game exits when the user closes it. Clean shutdown,
//   no race, and we still get exit code 0 (or whatever main returns).
//
// Exit codes the gtest checks:
//   0  -- clean LoadLibrary (DllMain ATTACH ran)
//   2  -- bad argv
//   3  -- CommandLineToArgvW failed
//   4  -- LoadLibraryW failed (proxy didn't load; GetLastError on stderr)

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
    // Deliberately no FreeLibrary -- see header comment.
    return kExitOk;
}
