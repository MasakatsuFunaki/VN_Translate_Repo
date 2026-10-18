// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Simple file-based logging for debugging the proxy DLL.
// Writes to "proxy_log.txt" beside the game executable.  The path is
// absolute on purpose: a relative one follows the launcher's working
// directory, and a session launched from elsewhere logs into the void.

#include <windows.h>
#include <cstdio>
#include <cstdarg>

inline FILE* g_logFile = nullptr;

inline void LogInit() {
    if (!g_logFile) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) wcscpy(slash + 1, L"proxy_log.txt");
        else       wcscpy(path, L"proxy_log.txt");
        g_logFile = _wfopen(path, L"w");
    }
}

inline void LogClose() {
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

// Every line opens with the local wall clock to the millisecond and the
// thread that wrote it.  Without both, a line the render thread wrote cannot
// be ordered against one a background thread wrote, and no line can be
// ordered against a screenshot taken from outside the process.
inline void Log(const char* fmt, ...) {
    if (!g_logFile) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    fprintf(g_logFile, "[%02u:%02u:%02u.%03u %5lu] ",
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
            GetCurrentThreadId());
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
}
