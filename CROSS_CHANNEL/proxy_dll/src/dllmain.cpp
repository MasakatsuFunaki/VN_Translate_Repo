// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include <windows.h>
#include "proxy.h"
#include "translator.h"
#include "log.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        LogInit();
        Log("=== CROSS-CHANNEL (cc.exe / WillPlus) Proxy DLL loaded ===");
        ProxyInit();
        TranslatorInit();
        break;

    case DLL_PROCESS_DETACH:
        TranslatorShutdown();
        ProxyShutdown();
        Log("=== CROSS-CHANNEL Proxy DLL unloaded ===");
        LogClose();
        break;
    }
    return TRUE;
}
