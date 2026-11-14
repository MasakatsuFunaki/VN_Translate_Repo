// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "key_dumper.h"
#include "log.h"
#include "proxy.h"
#include "translator.h"
#include "win32_chrome.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        ::DisableThreadLibraryCalls(hModule);
        LogInit();
        Log("=== Adieu (SiglusEngine) Proxy DLL loaded ===");
        ProxyInit();
        KeyDumperInit();
        win32_chrome::Win32ChromeInit();
        TranslatorInit(hModule);
        break;

    case DLL_PROCESS_DETACH:
        if (lpReserved != nullptr) {
            return TRUE;
        }
        TranslatorShutdown();
        win32_chrome::Win32ChromeShutdown();
        KeyDumperShutdown();
        ProxyShutdown();
        Log("=== Adieu Proxy DLL unloaded ===");
        LogClose();
        break;
    }
    return TRUE;
}
