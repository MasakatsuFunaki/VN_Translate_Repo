// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace {

constexpr wchar_t kDefaultLogFileName[] = L"proxy_log.txt";

INIT_ONCE g_log_init_once = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_log_mutex{};
std::FILE* g_log_file = nullptr;

BOOL CALLBACK InitLogMutexCallback(PINIT_ONCE /*init_once*/,
                                   PVOID /*parameter*/,
                                   PVOID* /*context*/) noexcept {
    ::InitializeCriticalSection(&g_log_mutex);
    return TRUE;
}

CRITICAL_SECTION* GetLogMutex() noexcept {
    ::InitOnceExecuteOnce(&g_log_init_once, InitLogMutexCallback, nullptr,
                          nullptr);
    return &g_log_mutex;
}

class ScopedCriticalSection {
public:
    explicit ScopedCriticalSection(CRITICAL_SECTION* cs) noexcept : cs_(cs) {
        ::EnterCriticalSection(cs_);
    }
    ~ScopedCriticalSection() { ::LeaveCriticalSection(cs_); }

    ScopedCriticalSection(const ScopedCriticalSection&) = delete;
    ScopedCriticalSection& operator=(const ScopedCriticalSection&) = delete;

private:
    CRITICAL_SECTION* cs_;
};

}  // namespace

void LogInit() {
    LogInit(kDefaultLogFileName);
}

void LogInit(const wchar_t* path) {
    ScopedCriticalSection lock(GetLogMutex());
    if (g_log_file != nullptr) {
        std::fclose(g_log_file);
        g_log_file = nullptr;
    }
    if (path != nullptr && path[0] != L'\0') {
        g_log_file = _wfopen(path, L"w");
    }
}

void LogClose() {
    ScopedCriticalSection lock(GetLogMutex());
    if (g_log_file != nullptr) {
        std::fclose(g_log_file);
        g_log_file = nullptr;
    }
}

void Log(const char* fmt, ...) {
    if (fmt == nullptr) {
        return;
    }
    ScopedCriticalSection lock(GetLogMutex());
    if (g_log_file == nullptr) {
        return;
    }

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    std::fprintf(g_log_file, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log_file, fmt, args);
    va_end(args);

    std::fputc('\n', g_log_file);
    std::fflush(g_log_file);
}
