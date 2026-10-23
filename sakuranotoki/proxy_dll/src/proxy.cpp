// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// proxy.cpp — version.dll proxy. Forwards every export of System32's
// version.dll through naked-jmp stubs so the engine resolves all
// version.dll calls correctly even though our DLL sits in the EXE
// folder. No sibling version_orig.dll file is needed (FAVORITE's
// winmm.dll uses the same pattern).
//
// We resolve System32\version.dll once at DllMain ATTACH, cache the
// addresses in g_procs, and the .def file maps each exported `Name`
// to the corresponding `proxy_Name` naked stub.

#include "log.h"
#include <windows.h>


static HMODULE g_realVersion = nullptr;

#define VERSION_FUNC_COUNT 17

static FARPROC g_procs[VERSION_FUNC_COUNT] = {0};

// All exported version.dll function names, in alphabetical order.
// Verified via dumpbin /exports C:\Windows\SysWOW64\version.dll —
// 17 names, ordinals 1..17, two are kernel32-forwarders (we don't
// need to special-case them; GetProcAddress walks forwarder chains).
static const char* g_names[VERSION_FUNC_COUNT] = {
    "GetFileVersionInfoA",          //  0
    "GetFileVersionInfoByHandle",   //  1
    "GetFileVersionInfoExA",        //  2
    "GetFileVersionInfoExW",        //  3
    "GetFileVersionInfoSizeA",      //  4
    "GetFileVersionInfoSizeExA",    //  5
    "GetFileVersionInfoSizeExW",    //  6
    "GetFileVersionInfoSizeW",      //  7
    "GetFileVersionInfoW",          //  8
    "VerFindFileA",                 //  9
    "VerFindFileW",                 // 10
    "VerInstallFileA",              // 11
    "VerInstallFileW",              // 12
    "VerLanguageNameA",             // 13
    "VerLanguageNameW",             // 14
    "VerQueryValueA",               // 15
    "VerQueryValueW",               // 16
};


extern "C" void ProxyInit() {
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    wcscat_s(sysDir, L"\\version.dll");

    g_realVersion = LoadLibraryW(sysDir);
    if (!g_realVersion) {
        LOG("FATAL: Could not load real version.dll from %ls (gle=%lu)",
            sysDir, GetLastError());
        return;
    }

    int resolved = 0;
    for (int i = 0; i < VERSION_FUNC_COUNT; i++) {
        g_procs[i] = GetProcAddress(g_realVersion, g_names[i]);
        if (g_procs[i]) resolved++;
    }
    LOG("Proxy: loaded real version.dll, resolved %d/%d exports",
        resolved, VERSION_FUNC_COUNT);
}

extern "C" void ProxyShutdown() {
    if (g_realVersion) {
        FreeLibrary(g_realVersion);
        g_realVersion = nullptr;
    }
}


// ── Naked forwarding stubs ───────────────────────────────────────────────────
// Each stub jumps through the corresponding cached function pointer.
// __declspec(naked) avoids any prologue/epilogue so the call frame is
// preserved verbatim. The .def file maps `Name = proxy_Name` for each.

#define PROXY_STUB(name, idx) \
    extern "C" __declspec(naked) void __stdcall proxy_##name() { \
        __asm { jmp dword ptr [g_procs + idx * 4] } \
    }

PROXY_STUB(GetFileVersionInfoA,        0)
PROXY_STUB(GetFileVersionInfoByHandle, 1)
PROXY_STUB(GetFileVersionInfoExA,      2)
PROXY_STUB(GetFileVersionInfoExW,      3)
PROXY_STUB(GetFileVersionInfoSizeA,    4)
PROXY_STUB(GetFileVersionInfoSizeExA,  5)
PROXY_STUB(GetFileVersionInfoSizeExW,  6)
PROXY_STUB(GetFileVersionInfoSizeW,    7)
PROXY_STUB(GetFileVersionInfoW,        8)
PROXY_STUB(VerFindFileA,               9)
PROXY_STUB(VerFindFileW,              10)
PROXY_STUB(VerInstallFileA,           11)
PROXY_STUB(VerInstallFileW,           12)
PROXY_STUB(VerLanguageNameA,          13)
PROXY_STUB(VerLanguageNameW,          14)
PROXY_STUB(VerQueryValueA,            15)
PROXY_STUB(VerQueryValueW,            16)


// Pre-DllMain static-init sentinel: writes to %TEMP%\sakuranotoki_proxy_loaded.txt
// the moment the DLL is mapped, BEFORE DllMain even runs. Pure Win32, no
// CRT dependency. Lets us tell from outside whether the DLL was actually
// loaded into the game process at all.
namespace {
struct SentinelWriter {
    SentinelWriter() {
        char tmp[MAX_PATH] = {};
        DWORD n = GetTempPathA(MAX_PATH, tmp);
        if (n == 0 || n >= MAX_PATH) return;
        char path[MAX_PATH] = {};
        wsprintfA(path, "%ssakuranotoki_proxy_loaded.txt", tmp);
        HANDLE h = CreateFileA(path, GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        SYSTEMTIME st;
        GetLocalTime(&st);
        char msg[256];
        int len = wsprintfA(msg,
            "version.dll proxy mapped at %04d-%02d-%02d %02d:%02d:%02d.%03d\r\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        DWORD written = 0;
        WriteFile(h, msg, (DWORD)len, &written, nullptr);
        CloseHandle(h);
    }
};
static SentinelWriter g_sentinel;
}  // namespace
