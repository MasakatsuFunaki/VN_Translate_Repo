// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// File-based logging for the Cross-Channel proxy DLL.
// Resolves <exe_dir>\proxy_log.txt first, falling back to CWD "proxy_log.txt"
// only when the executable directory is unwritable.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace log_detail {

constexpr DWORD kInitialLogPathChars = MAX_PATH;
constexpr DWORD kMaxLogPathChars = 32768u;

inline bool ResolveExeSiblingLogPath(std::wstring& out_path) {
    std::vector<wchar_t> buf(kInitialLogPathChars, L'\0');
    DWORD len = 0;
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return false;
        }
        if (len < buf.size() - 1 && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            break;
        }
        if (buf.size() >= kMaxLogPathChars) {
            return false;
        }
        const std::size_t next_size =
            std::min<std::size_t>(buf.size() * 2u, kMaxLogPathChars);
        buf.assign(next_size, L'\0');
    }

    std::wstring full_path(buf.data(), len);
    const std::size_t sep = full_path.find_last_of(L"\\/");
    if (sep == std::wstring::npos) {
        return false;
    }
    full_path.resize(sep + 1u);
    full_path += L"proxy_log.txt";
    out_path = std::move(full_path);
    return true;
}

}  // namespace log_detail

inline FILE* g_logFile = nullptr;

inline void LogInit() {
    if (g_logFile) return;
    std::wstring exe_log_path;
    if (log_detail::ResolveExeSiblingLogPath(exe_log_path)) {
        g_logFile = _wfopen(exe_log_path.c_str(), L"w");
    }
    if (!g_logFile) {
        g_logFile = _wfopen(L"proxy_log.txt", L"w");
    }
}

inline void LogClose() {
    if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
}

inline void Log(const char* fmt, ...) {
    if (!g_logFile) return;
    va_list args; va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
    fputc('\n', g_logFile);
    fflush(g_logFile);
}
