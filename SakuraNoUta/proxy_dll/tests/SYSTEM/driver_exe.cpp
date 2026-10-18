// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Minimal host for the SYSTEM gtest: LoadLibraryW the proxy, exit without
// FreeLibrary to mirror real game shutdown.  Exits 0/2/3/4 per error stage.

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
