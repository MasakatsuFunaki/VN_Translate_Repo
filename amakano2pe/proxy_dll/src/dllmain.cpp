// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "log.h"
#include "proxy.h"
#include "translator.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace {

constexpr wchar_t kDefaultLogFilePath[] = L"proxy_log.txt";

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        LogInit(kDefaultLogFilePath);
        Log("=== amakano2pe (CatSystem2) Proxy DLL loaded ===");
        ProxyInit();
        TranslatorInit();
        break;

    case DLL_PROCESS_DETACH:
        // When lpReserved != nullptr, the process is terminating via
        // ExitProcess: background threads may already have been terminated
        // while holding locks or CRT stdio state, so skip lock acquisition and
        // file teardown and let the OS reclaim all process resources.
        if (lpReserved != nullptr) {
            return TRUE;
        }
        TranslatorShutdown();
        ProxyShutdown();
        Log("=== amakano2pe Proxy DLL unloaded ===");
        LogClose();
        break;
    }
    return TRUE;
}
