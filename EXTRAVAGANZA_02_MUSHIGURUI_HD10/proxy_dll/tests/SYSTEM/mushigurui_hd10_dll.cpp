// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// End-to-end black-box load test for the built Mushigurui winmm.dll proxy.
//
// Spawns mushigurui_hd10_driver via CreateProcessW to LoadLibraryW the built
// proxy DLL, waits for process exit, verifies that DllMain wrote the startup
// log line, and removes proxy_log.txt via RAII on scope exit.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef PROXY_DLL_PATH
#define PROXY_DLL_PATH ""
#endif
#ifndef DRIVER_EXE_PATH
#define DRIVER_EXE_PATH ""
#endif
#ifndef LOG_FILE_PATH
#define LOG_FILE_PATH ""
#endif
#ifndef EXPECTED_BANNER
#define EXPECTED_BANNER ""
#endif

namespace fs = std::filesystem;

namespace {

constexpr DWORD kDriverProcessTimeoutMs = 30000u;
constexpr UINT kTimeoutTerminateExitCode = 99u;
constexpr DWORD kSentinelExitCode = 0xDEADBEEFu;
constexpr DWORD kExpectedDriverExitCode = 0u;
constexpr std::size_t kBackslashPairFactor = 2u;
constexpr std::size_t kQuoteEscapeBackslashCount = 1u;

class ScopedHandle {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

class ScopedFileRemover {
public:
    explicit ScopedFileRemover(fs::path path) : path_(std::move(path)) {
        RemoveNow();
    }

    ScopedFileRemover(const ScopedFileRemover&) = delete;
    ScopedFileRemover& operator=(const ScopedFileRemover&) = delete;

    ~ScopedFileRemover() noexcept {
        RemoveNow();
    }

private:
    void RemoveNow() const noexcept {
        if (path_.empty()) {
            return;
        }
        std::error_code ec;
        fs::remove(path_, ec);
    }

    fs::path path_;
};

// Quotes a single command-line argument using the Windows CommandLineToArgvW
// 2n / 2n+1 backslash-escaping rule so embedded quotes and trailing backslashes
// survive intact.
std::wstring QuoteCommandLineArgW(std::wstring_view arg) {
    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(
                backslashes * kBackslashPairFactor + kQuoteEscapeBackslashCount,
                L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(ch);
    }
    quoted.append(backslashes * kBackslashPairFactor, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::string ReadAllBytes(const fs::path& path) {
    if (path.empty()) {
        return {};
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool RunDriverExe(const fs::path& exe_path,
                  const fs::path& dll_path,
                  DWORD* out_exit_code) {
    if (out_exit_code == nullptr || exe_path.empty() || dll_path.empty()) {
        return false;
    }

    const std::wstring exe_w = exe_path.wstring();
    const std::wstring dll_w = dll_path.wstring();
    if (exe_w.empty() || exe_w.find(L'"') != std::wstring::npos || dll_w.empty()) {
        return false;
    }

    const std::wstring cmd =
        L"\"" + exe_w + L"\" " + QuoteCommandLineArgW(dll_w);
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_w.c_str(),
                        cmd_buf.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        nullptr,
                        &si,
                        &pi)) {
        return false;
    }

    const ScopedHandle proc_handle(pi.hProcess);
    const ScopedHandle thread_handle(pi.hThread);

    const DWORD wait_rc = WaitForSingleObject(proc_handle.get(), kDriverProcessTimeoutMs);
    if (wait_rc != WAIT_OBJECT_0) {
        TerminateProcess(proc_handle.get(), kTimeoutTerminateExitCode);
        return false;
    }
    return GetExitCodeProcess(proc_handle.get(), out_exit_code) != FALSE;
}

}  // namespace

TEST(ProxyDllSystem, DriverLoadsDllAndDllMainWritesBanner) {
    ASSERT_STRNE(PROXY_DLL_PATH, "") << "PROXY_DLL_PATH must be set at build time";
    ASSERT_STRNE(DRIVER_EXE_PATH, "") << "DRIVER_EXE_PATH must be set at build time";
    ASSERT_STRNE(LOG_FILE_PATH, "") << "LOG_FILE_PATH must be set at build time";
    ASSERT_STRNE(EXPECTED_BANNER, "") << "EXPECTED_BANNER must be set at build time";

    const fs::path configured_log = fs::u8path(LOG_FILE_PATH);
    const fs::path log_path =
        configured_log.is_absolute() ? configured_log : (fs::current_path() / configured_log);
    const ScopedFileRemover log_guard(log_path);

    const fs::path driver_path = fs::u8path(DRIVER_EXE_PATH);
    const fs::path dll_path = fs::u8path(PROXY_DLL_PATH);

    DWORD exit_code = kSentinelExitCode;
    ASSERT_TRUE(RunDriverExe(driver_path, dll_path, &exit_code))
        << "driver EXE failed to spawn or timed out (gle=" << GetLastError() << ")";
    EXPECT_EQ(exit_code, kExpectedDriverExitCode);
    std::cout << "[ NOTE     ] driver exit_code=" << exit_code << "\n";

    const std::string log = ReadAllBytes(log_path);
    ASSERT_FALSE(log.empty()) << "log file empty or missing: " << log_path.u8string();

    EXPECT_NE(log.find(EXPECTED_BANNER), std::string::npos)
        << "log did not contain expected startup message.\n"
        << "  expected substring: " << EXPECTED_BANNER << "\n"
        << "  log path:           " << log_path.u8string() << "\n"
        << "  log contents:\n"
        << log;
}
