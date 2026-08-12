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
        // Append, not truncate: an engine bounds-check warning (the
        // engine's own "範囲外バッファアクセス" dialog) does not crash the
        // process, but the user restarts after it -- and a truncating
        // open would wipe the very session that misbehaved. Keep prior
        // sessions; the timestamp banner below delimits them.
        g_logFile = _wfopen(L"proxy_log.txt", L"a");
        if (g_logFile) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(g_logFile,
                    "\n========== session start %04d-%02d-%02d "
                    "%02d:%02d:%02d ==========\n",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);
            fflush(g_logFile);
        }
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
