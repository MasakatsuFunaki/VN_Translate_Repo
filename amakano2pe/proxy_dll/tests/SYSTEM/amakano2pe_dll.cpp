// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// End-to-end black-box system test for the built proxy DLL.
//
// Spawns amakano2pe_driver with the built proxy DLL path over CreateProcessW,
// waits for completion, reads the log written during DllMain, and asserts that
// the game-specific startup banner prefix is present.
//
// Compile-time definitions provided by SYSTEM/CMakeLists.txt:
//   PROXY_DLL_PATH:  absolute UTF-8 path to the built proxy DLL
//   DRIVER_EXE_PATH: absolute UTF-8 path to amakano2pe_driver
//   LOG_FILE_PATH:   UTF-8 path to the log file written by the proxy
//   EXPECTED_BANNER: substring that must appear in the log

#include <gtest/gtest.h>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr DWORD kDriverWaitTimeoutMs = 30000;
constexpr UINT kTimeoutTerminateExitCode = 99;
constexpr DWORD kSentinelExitCode = 0xDEADBEEFu;
constexpr std::size_t kEscapedBackslashMultiplier = 2;
constexpr std::size_t kEscapedQuoteSlashCount = 1;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h) noexcept : h_(h) {}

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() {
        if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
        }
    }

    HANDLE get() const noexcept { return h_; }

private:
    HANDLE h_;
};

class ScopedFileRemover {
public:
    explicit ScopedFileRemover(fs::path path) noexcept
        : path_(std::move(path)) {}

    ScopedFileRemover(const ScopedFileRemover&) = delete;
    ScopedFileRemover& operator=(const ScopedFileRemover&) = delete;

    ~ScopedFileRemover() {
        std::error_code ec;
        fs::remove(path_, ec);
    }

private:
    fs::path path_;
};

bool Utf8ToWide(std::string_view s, std::wstring& out) {
    if (s.empty()) {
        return false;
    }
    const int len = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
        nullptr, 0);
    if (len <= 0) {
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
        wide.data(), len);
    if (written != len) {
        return false;
    }
    out = std::move(wide);
    return true;
}

std::wstring QuoteWindowsArg(std::wstring_view arg) {
    if (!arg.empty() &&
        arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }

    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t pending_backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++pending_backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(
                pending_backslashes * kEscapedBackslashMultiplier +
                    kEscapedQuoteSlashCount,
                L'\\');
            quoted.push_back(L'"');
            pending_backslashes = 0;
            continue;
        }
        quoted.append(pending_backslashes, L'\\');
        quoted.push_back(ch);
        pending_backslashes = 0;
    }
    quoted.append(pending_backslashes * kEscapedBackslashMultiplier, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::string ReadAllBytes(std::string_view utf8_path) {
    if (utf8_path.empty()) {
        return {};
    }
    try {
        std::ifstream f(fs::u8path(utf8_path), std::ios::binary);
        if (!f) {
            return {};
        }
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    } catch (const std::exception&) {
        return {};
    }
}

bool RunDriverExe(std::string_view exe_path, std::string_view dll_arg,
                  DWORD* out_exit_code) {
    if (out_exit_code == nullptr ||
        exe_path.find('"') != std::string_view::npos) {
        return false;
    }

    std::wstring exe_wide;
    std::wstring dll_wide;
    if (!Utf8ToWide(exe_path, exe_wide) || !Utf8ToWide(dll_arg, dll_wide)) {
        return false;
    }

    std::wstring cmd = L"\"" + exe_wide + L"\" " + QuoteWindowsArg(dll_wide);
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }

    const ScopedHandle process_handle(pi.hProcess);
    const ScopedHandle thread_handle(pi.hThread);

    const DWORD wait_rc =
        WaitForSingleObject(process_handle.get(), kDriverWaitTimeoutMs);
    if (wait_rc != WAIT_OBJECT_0) {
        TerminateProcess(process_handle.get(), kTimeoutTerminateExitCode);
        return false;
    }
    if (!GetExitCodeProcess(process_handle.get(), out_exit_code)) {
        return false;
    }
    return true;
}

}  // namespace

TEST(ProxyDllSystem, DriverLoadsDllAndDllMainWritesBanner) {
    ASSERT_STRNE(PROXY_DLL_PATH, "")
        << "PROXY_DLL_PATH must be set at build time";
    ASSERT_STRNE(DRIVER_EXE_PATH, "")
        << "DRIVER_EXE_PATH must be set at build time";
    ASSERT_STRNE(LOG_FILE_PATH, "")
        << "LOG_FILE_PATH must be set at build time";
    ASSERT_STRNE(EXPECTED_BANNER, "")
        << "EXPECTED_BANNER must be set at build time";

    std::wstring log_wide;
    ASSERT_TRUE(Utf8ToWide(LOG_FILE_PATH, log_wide))
        << "LOG_FILE_PATH must be valid UTF-8";

    const fs::path log_path = fs::u8path(LOG_FILE_PATH);
    std::error_code ec;
    fs::remove(log_path, ec);
    const ScopedFileRemover cleanup_log(log_path);

    DWORD exit_code = kSentinelExitCode;
    ASSERT_TRUE(RunDriverExe(DRIVER_EXE_PATH, PROXY_DLL_PATH, &exit_code))
        << "driver EXE failed to spawn or timed out (gle="
        << GetLastError() << ")";

    // Informational: in a non-game host, post-banner hook installation may fail
    // or fault after Log() has already flushed the startup banner to disk.
    std::cout << "[ NOTE     ] driver exit_code=" << exit_code
              << " (0 = clean ATTACH; non-zero = post-banner hook failure)\n";

    const std::string log = ReadAllBytes(LOG_FILE_PATH);
    ASSERT_FALSE(log.empty())
        << "log file empty or missing: " << LOG_FILE_PATH;

    EXPECT_NE(log.find(EXPECTED_BANNER), std::string::npos)
        << "log did not contain expected banner.\n"
        << "  expected substring: " << EXPECTED_BANNER << "\n"
        << "  log path:           " << LOG_FILE_PATH << "\n"
        << "  log contents:\n" << log;
}
