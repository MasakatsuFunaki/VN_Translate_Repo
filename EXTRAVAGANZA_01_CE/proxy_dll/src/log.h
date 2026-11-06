// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Simple file-based logging for debugging the proxy DLL.
// Writes to "proxy_log.txt" in the game directory.

#include <windows.h>
#include <cstdio>
#include <cstdarg>

inline FILE* g_logFile = nullptr;

inline void LogInit() {
    if (!g_logFile) {
        g_logFile = _wfopen(L"proxy_log.txt", L"w");
    }
}

inline void LogClose() {
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

inline void Log(const char* fmt, ...) {
    if (!g_logFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
}
