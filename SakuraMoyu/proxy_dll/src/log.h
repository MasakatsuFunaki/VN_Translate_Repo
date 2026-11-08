// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// Simple file-based logging for debugging the proxy DLL.
//
// Writes to "proxy_log.txt" in the game directory. The format strings and
// %s arguments passed to Log() are CP932 (Shift-JIS) — that's the game's
// native string encoding and what the hook intercepts. We transcode each
// formatted line to UTF-8 before writing, and the file gets a UTF-8 BOM,
// so the log opens cleanly in a UTF-8 editor with Japanese rendered as
// readable text instead of mojibake.

#include <windows.h>
#include <cstdio>
#include <cstdarg>

inline FILE* g_logFile = nullptr;

inline void LogInit() {
    if (!g_logFile) {
        // Binary mode so we control newlines explicitly and the BOM is
        // written verbatim. Text mode would CRLF-translate the BOM bytes.
        g_logFile = _wfopen(L"proxy_log.txt", L"wb");
        if (g_logFile) {
            // UTF-8 BOM
            fwrite("\xEF\xBB\xBF", 1, 3, g_logFile);
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

    char buf[4096];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;

    // Transcode CP932 -> UTF-16 -> UTF-8. Pure-ASCII lines round-trip
    // unchanged; lines that embed CP932 JP bytes (via %s on engine
    // pointers) come out as readable Japanese in a UTF-8 viewer.
    wchar_t wbuf[4096];
    int wlen = MultiByteToWideChar(932, 0, buf, n, wbuf,
                                   (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (wlen <= 0) {
        // Untranslatable byte sequence — write raw so we at least see
        // something instead of dropping the line.
        fwrite(buf, 1, (size_t)n, g_logFile);
    } else {
        char u8buf[8192];
        int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen,
                                        u8buf, (int)sizeof(u8buf),
                                        nullptr, nullptr);
        if (u8len > 0) fwrite(u8buf, 1, (size_t)u8len, g_logFile);
    }
    fputc('\r', g_logFile);
    fputc('\n', g_logFile);
    fflush(g_logFile);
}
