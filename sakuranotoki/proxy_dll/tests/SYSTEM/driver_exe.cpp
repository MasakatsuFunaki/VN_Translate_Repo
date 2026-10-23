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
//   3  -- path widen failure
//   4  -- LoadLibraryW failed (proxy didn't load; GetLastError on stderr)

#include <windows.h>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <proxy_dll_path>\n", argv[0]);
        return 2;
    }
    int wn = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
    if (wn <= 0) {
        std::fprintf(stderr, "MultiByteToWideChar failed gle=%lu\n",
                     GetLastError());
        return 3;
    }
    std::wstring wpath(static_cast<size_t>(wn), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, &wpath[0], wn);
    if (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();

    HMODULE h = LoadLibraryW(wpath.c_str());
    if (!h) {
        std::fprintf(stderr, "LoadLibraryW(\"%s\") failed gle=%lu\n",
                     argv[1], GetLastError());
        return 4;
    }
    // Deliberately no FreeLibrary -- see header comment.
    return 0;
}
