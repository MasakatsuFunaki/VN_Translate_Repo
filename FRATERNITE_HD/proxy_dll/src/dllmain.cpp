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
#include "corpus.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        LogInit();
        Log("=== FRATERNITE_HD (YU-RIS) Proxy DLL loaded ===");
        CorpusInit();
        ProxyInit();
        TranslatorInit();
        break;

    case DLL_PROCESS_DETACH:
        TranslatorShutdown();
        ProxyShutdown();
        Log("=== FRATERNITE_HD Proxy DLL unloaded ===");
        CorpusClose();
        LogClose();
        break;
    }
    return TRUE;
}
