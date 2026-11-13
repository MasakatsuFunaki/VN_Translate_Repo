// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Minimal fake host executable for the proxy DLL system test.
//
// Parses the wide Win32 command line via CommandLineToArgvW and loads the built
// winmm.dll via LoadLibraryW so DllMain(DLL_PROCESS_ATTACH) runs synchronously
// and flushes the startup log line, then exits normally without FreeLibrary so
// the OS terminates worker threads cleanly prior to DLL_PROCESS_DETACH.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdio>

namespace {

constexpr int kRequiredArgCount = 2;
constexpr int kExitOk = 0;
constexpr int kExitBadArgs = 2;
constexpr int kExitLoadFailed = 4;

class ScopedWideArgv {
public:
    ScopedWideArgv() noexcept : argv_(CommandLineToArgvW(GetCommandLineW(), &argc_)) {}
    ScopedWideArgv(const ScopedWideArgv&) = delete;
    ScopedWideArgv& operator=(const ScopedWideArgv&) = delete;

    ~ScopedWideArgv() {
        if (argv_ != nullptr) {
            LocalFree(argv_);
        }
    }

    int argc() const noexcept { return argc_; }
    LPWSTR* argv() const noexcept { return argv_; }

private:
    int argc_ = 0;
    LPWSTR* argv_ = nullptr;
};

}  // namespace

int main() {
    const ScopedWideArgv args;
    if (args.argv() == nullptr || args.argc() < kRequiredArgCount) {
        std::fprintf(stderr, "usage: mushigurui_hd10_driver <proxy_dll_path>\n");
        return kExitBadArgs;
    }

    const HMODULE mod = LoadLibraryW(args.argv()[1]);
    if (mod == nullptr) {
        std::fprintf(stderr, "LoadLibraryW failed gle=%lu\n", GetLastError());
        return kExitLoadFailed;
    }
    return kExitOk;
}
