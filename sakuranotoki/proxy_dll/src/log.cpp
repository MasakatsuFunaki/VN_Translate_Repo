// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr unsigned char kUtf8Bom[] = {0xEFU, 0xBBU, 0xBFU};
constexpr std::size_t kFormatBufferSize = 4096;
constexpr std::size_t kUtf8OutputBufferSize = 8192;
constexpr std::size_t kWideBufferChars = 4096;
constexpr DWORD kMaxTempPathChars = 32768U;
constexpr UINT kCp932CodePage = 932U;
constexpr DWORD kCrlfLength = 2U;

class ScopedHandle {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.release()) {}

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ~ScopedHandle() {
        reset();
    }

    HANDLE get() const noexcept { return handle_; }

    bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE release() noexcept {
        const HANDLE current = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return current;
    }

    void reset(HANDLE next = INVALID_HANDLE_VALUE) noexcept {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = next;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

ScopedHandle g_h_primary;
ScopedHandle g_h_fallback;
DWORD g_primary_err = 0;
DWORD g_fallback_err = 0;

ScopedHandle open_log(const wchar_t* path) {
    if (path == nullptr || *path == L'\0') {
        return ScopedHandle{};
    }
    const HANDLE h = CreateFileW(
        path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return ScopedHandle{h};
}

std::wstring query_temp_dir() {
    const DWORD needed = GetTempPathW(0, nullptr);
    if (needed == 0 || needed > kMaxTempPathChars) {
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

std::string wide_to_utf8(const wchar_t* wpath) {
    if (wpath == nullptr || *wpath == L'\0') {
        return {};
    }
    const int wlen = static_cast<int>(std::wcslen(wpath));
    DWORD flags = WC_ERR_INVALID_CHARS;
    int needed = WideCharToMultiByte(
        CP_UTF8, flags, wpath, wlen, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        flags = 0;
        needed = WideCharToMultiByte(
            CP_UTF8, flags, wpath, wlen, nullptr, 0, nullptr, nullptr);
    }
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8, flags, wpath, wlen, out.data(), needed, nullptr, nullptr);
    if (converted <= 0) {
        return {};
    }
    return out;
}

void write_all(HANDLE h, const void* data, DWORD len) {
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(h, data, len, &written, nullptr);
}

}  // namespace

void log_init(const wchar_t* path) {
    g_h_primary.reset();
    g_h_fallback.reset();
    g_primary_err = 0;
    g_fallback_err = 0;

    std::string primary_utf8;
    if (path != nullptr && *path != L'\0') {
        g_h_primary = open_log(path);
        if (!g_h_primary.valid()) {
            g_primary_err = GetLastError();
        }
        primary_utf8 = wide_to_utf8(path);
    }

    std::string fallback_utf8;
    const std::wstring temp_dir = query_temp_dir();
    if (!temp_dir.empty()) {
        const std::wstring fallback_path = temp_dir + L"sakuranotoki_proxy.log";
        g_h_fallback = open_log(fallback_path.c_str());
        if (!g_h_fallback.valid()) {
            g_fallback_err = GetLastError();
        }
        fallback_utf8 = wide_to_utf8(fallback_path.c_str());
    }

    if (g_h_primary.valid()) {
        write_all(g_h_primary.get(), kUtf8Bom, static_cast<DWORD>(sizeof(kUtf8Bom)));
    }
    if (g_h_fallback.valid()) {
        write_all(g_h_fallback.get(), kUtf8Bom, static_cast<DWORD>(sizeof(kUtf8Bom)));
    }

    log_msg("sakuranotoki version.dll proxy log opened (UTF-8)");
    log_msg("primary=%s  open=%s  gle=%lu",
            primary_utf8.c_str(),
            g_h_primary.valid() ? "yes" : "no",
            g_primary_err);
    log_msg("fallback=%s  open=%s  gle=%lu",
            fallback_utf8.c_str(),
            g_h_fallback.valid() ? "yes" : "no",
            g_fallback_err);
}

void log_close() {
    g_h_primary.reset();
    g_h_fallback.reset();
}

void log_msg(const char* fmt, ...) {
    char buf[kFormatBufferSize] = {};
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    const int max_len = static_cast<int>(sizeof(buf)) - 1;
    if (n > max_len) {
        n = max_len;
    }

    char u8out[kUtf8OutputBufferSize] = {};
    int u8len = 0;

    wchar_t wbuf[kWideBufferChars] = {};
    int wlen = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, buf, n,
        wbuf, static_cast<int>(kWideBufferChars));
    if (wlen <= 0) {
        wlen = MultiByteToWideChar(
            kCp932CodePage, 0, buf, n,
            wbuf, static_cast<int>(kWideBufferChars));
    }
    if (wlen > 0) {
        u8len = WideCharToMultiByte(
            CP_UTF8, 0, wbuf, wlen,
            u8out, static_cast<int>(sizeof(u8out)), nullptr, nullptr);
    }
    const char* payload = u8out;
    DWORD paylen = static_cast<DWORD>(u8len);
    if (u8len <= 0) {
        payload = buf;
        paylen = static_cast<DWORD>(n);
    }

    if (g_h_primary.valid()) {
        write_all(g_h_primary.get(), payload, paylen);
        write_all(g_h_primary.get(), "\r\n", kCrlfLength);
        FlushFileBuffers(g_h_primary.get());
    }
    if (g_h_fallback.valid()) {
        write_all(g_h_fallback.get(), payload, paylen);
        write_all(g_h_fallback.get(), "\r\n", kCrlfLength);
        FlushFileBuffers(g_h_fallback.get());
    }
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}
