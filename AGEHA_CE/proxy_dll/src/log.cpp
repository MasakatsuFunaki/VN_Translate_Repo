// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

#include "log.h"

#include <cstdarg>
#include <cstdio>

FILE* g_logFile = nullptr;

void LogInit() {
    if (!g_logFile) {
        g_logFile = _wfopen(L"proxy_log.txt", L"w");
    }
}

void LogClose() {
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
}

void Log(const char* fmt, ...) {
    if (!g_logFile) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_logFile, fmt, args);
    va_end(args);
    std::fprintf(g_logFile, "\n");
    std::fflush(g_logFile);
}
