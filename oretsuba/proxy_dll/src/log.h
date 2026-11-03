// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#pragma once

#include <cstdarg>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace oretsuba::proxy {

inline FILE*& LogStream() {
    static FILE* stream = nullptr;
    return stream;
}

inline void LogInit(const wchar_t* path) {
    _wfopen_s(&LogStream(), path, L"w");
}

inline void Log(const char* fmt, ...) {
    FILE* stream = LogStream();
    if (!stream) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stream, fmt, ap);
    va_end(ap);
    std::fprintf(stream, "\n");
    std::fflush(stream);
}

inline void LogClose() {
    FILE*& stream = LogStream();
    if (!stream) {
        return;
    }
    std::fclose(stream);
    stream = nullptr;
}

}  // namespace oretsuba::proxy
