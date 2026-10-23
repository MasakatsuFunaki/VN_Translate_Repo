// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// We write the log via raw Win32 (CreateFileW / WriteFile) instead of
// CRT fopen because some games run under tokens or CRT initialisations
// that trip up fopen silently. CreateFileW also reports a real
// GetLastError when something fails, which we can stash for the next
// log call to surface.
//
// To survive any single path being unwritable (Defender quarantine,
// elevated game launcher, write-protected install dir, etc.), we open
// up to two destinations: the path the caller asked for, plus
// %TEMP%\sakuranotoki_proxy.log. Whichever opens, gets the bytes; if both
// open we write to both so the diagnostic is always recoverable.

namespace {

HANDLE g_h_primary  = INVALID_HANDLE_VALUE;
HANDLE g_h_fallback = INVALID_HANDLE_VALUE;
char   g_primary_path[MAX_PATH]  = {};
char   g_fallback_path[MAX_PATH] = {};
DWORD  g_primary_err  = 0;
DWORD  g_fallback_err = 0;

HANDLE open_log(const char* path) {
    if (!path || !*path) return INVALID_HANDLE_VALUE;
    return CreateFileA(path, GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

void write_all(HANDLE h, const void* data, DWORD len) {
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, data, len, &written, nullptr);
}

}  // namespace

void log_init(const char* path) {
    if (path && *path) {
        std::strncpy(g_primary_path, path, MAX_PATH - 1);
        g_h_primary = open_log(path);
        if (g_h_primary == INVALID_HANDLE_VALUE)
            g_primary_err = GetLastError();
    }

    // Always try a TEMP fallback so we never lose diagnostics. Even if
    // the primary opens, the fallback is harmless duplication.
    char tmp[MAX_PATH] = {};
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    if (n > 0 && n < MAX_PATH) {
        std::snprintf(g_fallback_path, sizeof(g_fallback_path),
                      "%ssakuranotoki_proxy.log", tmp);
        g_h_fallback = open_log(g_fallback_path);
        if (g_h_fallback == INVALID_HANDLE_VALUE)
            g_fallback_err = GetLastError();
    }

    // BOM + opening header on whichever handle(s) opened.
    if (g_h_primary != INVALID_HANDLE_VALUE)
        write_all(g_h_primary, "\xEF\xBB\xBF", 3);
    if (g_h_fallback != INVALID_HANDLE_VALUE)
        write_all(g_h_fallback, "\xEF\xBB\xBF", 3);

    log_msg("---- sakuranotoki version.dll proxy log opened (UTF-8) ----");
    log_msg("primary=%s  open=%s  gle=%lu",
            g_primary_path,
            g_h_primary != INVALID_HANDLE_VALUE ? "yes" : "no",
            g_primary_err);
    log_msg("fallback=%s  open=%s  gle=%lu",
            g_fallback_path,
            g_h_fallback != INVALID_HANDLE_VALUE ? "yes" : "no",
            g_fallback_err);
}

void log_close() {
    if (g_h_primary != INVALID_HANDLE_VALUE) {
        CloseHandle(g_h_primary);
        g_h_primary = INVALID_HANDLE_VALUE;
    }
    if (g_h_fallback != INVALID_HANDLE_VALUE) {
        CloseHandle(g_h_fallback);
        g_h_fallback = INVALID_HANDLE_VALUE;
    }
}

void log_msg(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;

    // Heuristic transcode: pure-ASCII passes through. JP %s args (CP932
    // from a hook, or UTF-8 from the lookup table) are both turned into
    // valid UTF-8 — we try UTF-8 first (no double-conversion in the
    // common case), then fall back to CP932 if the buffer isn't valid
    // UTF-8.
    char u8out[8192];
    int  u8len = 0;

    wchar_t wbuf[4096];
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   buf, n, wbuf,
                                   (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (wlen <= 0) {
        wlen = MultiByteToWideChar(932, 0, buf, n,
                                   wbuf,
                                   (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    }
    if (wlen > 0) {
        u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen,
                                    u8out, (int)sizeof(u8out),
                                    nullptr, nullptr);
    }
    const char* payload = u8out;
    DWORD       paylen  = (DWORD)u8len;
    if (u8len <= 0) {
        // Last-resort fallback: write raw bytes verbatim.
        payload = buf;
        paylen  = (DWORD)n;
    }

    if (g_h_primary != INVALID_HANDLE_VALUE) {
        write_all(g_h_primary, payload, paylen);
        write_all(g_h_primary, "\r\n", 2);
        FlushFileBuffers(g_h_primary);
    }
    if (g_h_fallback != INVALID_HANDLE_VALUE) {
        write_all(g_h_fallback, payload, paylen);
        write_all(g_h_fallback, "\r\n", 2);
        FlushFileBuffers(g_h_fallback);
    }
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}
