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

#include "proxy.h"

#include "log.h"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kVersionFuncCount = 17;
constexpr DWORD kMaxSystemPathChars = 32768U;
constexpr std::size_t kSentinelMsgBufSize = 256;

class ScopedHandle {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() {
        if (valid()) {
            CloseHandle(handle_);
        }
    }

    HANDLE get() const noexcept { return handle_; }

    bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class ScopedModule {
public:
    ScopedModule() noexcept = default;

    ScopedModule(const ScopedModule&) = delete;
    ScopedModule& operator=(const ScopedModule&) = delete;

    ~ScopedModule() {
        reset();
    }

    HMODULE get() const noexcept { return module_; }

    void reset(HMODULE next = nullptr) noexcept {
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
        module_ = next;
    }

private:
    HMODULE module_ = nullptr;
};

ScopedModule g_real_version;

// All exported version.dll function names, in alphabetical order.
// Verified via dumpbin /exports C:\Windows\SysWOW64\version.dll —
// 17 names, ordinals 1..17, two are kernel32-forwarders (we don't
// need to special-case them; GetProcAddress walks forwarder chains).
constexpr const char* kExportNames[kVersionFuncCount] = {
    "GetFileVersionInfoA",
    "GetFileVersionInfoByHandle",
    "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA",
    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW",
    "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW",
    "VerFindFileA",
    "VerFindFileW",
    "VerInstallFileA",
    "VerInstallFileW",
    "VerLanguageNameA",
    "VerLanguageNameW",
    "VerQueryValueA",
    "VerQueryValueW",
};

std::wstring query_system_version_dll_path() {
    const UINT needed = GetSystemDirectoryW(nullptr, 0);
    if (needed == 0 || needed > kMaxSystemPathChars) {
        return {};
    }
    std::vector<wchar_t> buf(needed, L'\0');
    const UINT written = GetSystemDirectoryW(buf.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    std::wstring path(buf.data(), written);
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    path += L"version.dll";
    return path;
}

std::wstring query_temp_dir() {
    const DWORD needed = GetTempPathW(0, nullptr);
    if (needed == 0 || needed > kMaxSystemPathChars) {
        return {};
    }
    std::vector<wchar_t> buf(needed, L'\0');
    const DWORD written = GetTempPathW(needed, buf.data());
    if (written == 0 || written >= needed) {
        return {};
    }
    std::wstring dir(buf.data(), written);
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
        dir.push_back(L'\\');
    }
    return dir;
}

std::string wide_to_utf8(const std::wstring& wpath) {
    if (wpath.empty()) {
        return {};
    }
    const int wlen = static_cast<int>(wpath.size());
    DWORD flags = WC_ERR_INVALID_CHARS;
    int needed = WideCharToMultiByte(
        CP_UTF8, flags, wpath.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        flags = 0;
        needed = WideCharToMultiByte(
            CP_UTF8, flags, wpath.data(), wlen, nullptr, 0, nullptr, nullptr);
    }
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8, flags, wpath.data(), wlen, out.data(), needed, nullptr, nullptr);
    if (converted <= 0) {
        return {};
    }
    return out;
}

}  // namespace

static FARPROC g_procs[kVersionFuncCount] = {};

void ProxyInit() {
    const std::wstring sys_dll_path = query_system_version_dll_path();
    if (sys_dll_path.empty()) {
        LOG("FATAL: Could not query system directory for version.dll (gle=%lu)",
            GetLastError());
        return;
    }

    const HMODULE loaded = LoadLibraryW(sys_dll_path.c_str());
    if (loaded == nullptr) {
        const DWORD last_err = GetLastError();
        const std::string path_utf8 = wide_to_utf8(sys_dll_path);
        LOG("FATAL: Could not load real version.dll from %s (gle=%lu)",
            path_utf8.c_str(), last_err);
        return;
    }
    g_real_version.reset(loaded);

    std::size_t resolved = 0;
    for (std::size_t i = 0; i < kVersionFuncCount; ++i) {
        g_procs[i] = GetProcAddress(g_real_version.get(), kExportNames[i]);
        if (g_procs[i] != nullptr) {
            ++resolved;
        }
    }
    LOG("Proxy: loaded real version.dll, resolved %zu/%zu exports",
        resolved, kVersionFuncCount);
}

void ProxyShutdown() {
    g_real_version.reset();
}

// Naked forwarding stubs: each stub jumps through the cached pointer.
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

#undef PROXY_STUB

// Pre-DllMain static-init sentinel: writes to %TEMP%\sakuranotoki_proxy_loaded.txt
// the moment the DLL is mapped, BEFORE DllMain even runs. Lets us tell from
// outside whether the DLL was actually loaded into the game process at all.
namespace {

struct SentinelWriter {
    SentinelWriter() {
        const std::wstring temp_dir = query_temp_dir();
        if (temp_dir.empty()) {
            return;
        }
        const std::wstring path = temp_dir + L"sakuranotoki_proxy_loaded.txt";
        const ScopedHandle file(CreateFileW(
            path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.valid()) {
            return;
        }
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char msg[kSentinelMsgBufSize] = {};
        const int len = std::snprintf(
            msg, sizeof(msg),
            "version.dll proxy mapped at %04u-%02u-%02u %02u:%02u:%02u.%03u\r\n",
            static_cast<unsigned>(st.wYear),
            static_cast<unsigned>(st.wMonth),
            static_cast<unsigned>(st.wDay),
            static_cast<unsigned>(st.wHour),
            static_cast<unsigned>(st.wMinute),
            static_cast<unsigned>(st.wSecond),
            static_cast<unsigned>(st.wMilliseconds));
        if (len <= 0) {
            return;
        }
        const std::size_t max_len = sizeof(msg) - 1;
        const DWORD to_write = static_cast<DWORD>(
            static_cast<std::size_t>(len) > max_len
                ? max_len
                : static_cast<std::size_t>(len));
        DWORD written = 0;
        WriteFile(file.get(), msg, to_write, &written, nullptr);
    }
};

const SentinelWriter g_sentinel{};

}  // namespace
