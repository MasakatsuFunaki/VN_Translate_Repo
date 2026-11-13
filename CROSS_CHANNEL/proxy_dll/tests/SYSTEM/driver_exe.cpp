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
#include <exception>

namespace {

constexpr int kExitClean = 0;
constexpr int kExitBadArgv = 2;
constexpr int kExitArgParseFailed = 3;
constexpr int kExitLoadLibraryFailed = 4;
constexpr int kExitUnhandledException = 5;
constexpr int kMinArgCount = 2;

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
            std::fprintf(stderr, "usage: cross_channel_driver <proxy_dll_path>\n");
            return kExitBadArgv;
        }

        const HMODULE mod = LoadLibraryW(wargv[1]);
        if (mod == nullptr) {
            std::fprintf(stderr, "LoadLibraryW failed gle=%lu\n", GetLastError());
            return kExitLoadLibraryFailed;
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
