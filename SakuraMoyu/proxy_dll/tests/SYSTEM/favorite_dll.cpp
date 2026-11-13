// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// End-to-end black-box test for the built FAVORITE proxy DLL.
//
// Spawns favorite_driver via CreateProcessW with the built proxy DLL path,
// waits for completion, and verifies that DllMain ATTACH wrote the expected
// game banner to proxy_log.txt.
//
// Compile-time definitions supplied by CMake:
//   PROXY_DLL_PATH  -- UTF-8 path to the built proxy DLL
//   DRIVER_EXE_PATH -- UTF-8 path to favorite_driver.exe
//   LOG_FILE_PATH   -- UTF-8 path where the proxy writes its log
//   EXPECTED_BANNER -- substring expected in the log output
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

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

namespace {

namespace fs = std::filesystem;

constexpr DWORD kDriverTimeoutMs = 30000u;
constexpr UINT kTerminateTimeoutExitCode = 99u;
constexpr DWORD kInitialSentinelExitCode = 0xDEADBEEFu;
constexpr std::size_t kSurroundingQuoteCount = 2u;
constexpr std::size_t kEscapedBackslashMultiplier = 2u;
constexpr std::size_t kLiteralQuoteEscapeBackslashOffset = 1u;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = nullptr) noexcept : handle_(h) {}

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size_needed = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8.data(),
        static_cast<int>(utf8.size()),
        nullptr,
        0);
    if (size_needed <= 0) {
        throw std::runtime_error("Invalid UTF-8 sequence in path");
    }
    std::wstring wide(static_cast<std::size_t>(size_needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8.data(),
        static_cast<int>(utf8.size()),
        wide.data(),
        size_needed);
    if (written != size_needed) {
        throw std::runtime_error("UTF-8 to UTF-16 path conversion failed");
    }
    return wide;
}

bool NeedsWindowsQuotesWide(std::wstring_view arg) noexcept {
    if (arg.empty()) {
        return true;
    }
    for (const wchar_t ch : arg) {
        if (ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\v' || ch == L'"') {
            return true;
        }
    }
    return false;
}

std::wstring QuoteWindowsArgWide(std::wstring_view arg) {
    if (!NeedsWindowsQuotesWide(arg)) {
        return std::wstring(arg);
    }

    std::wstring out;
    out.reserve(arg.size() + kSurroundingQuoteCount);
    out.push_back(L'"');

    std::size_t backslashes = 0u;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * kEscapedBackslashMultiplier +
                           kLiteralQuoteEscapeBackslashOffset,
                       L'\\');
            out.push_back(L'"');
            backslashes = 0u;
            continue;
        }
        out.append(backslashes, L'\\');
        out.push_back(ch);
        backslashes = 0u;
    }

    out.append(backslashes * kEscapedBackslashMultiplier, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring FormatDriverCommandLineWide(std::wstring_view exe_path,
                                         std::wstring_view dll_arg) {
    if (exe_path.empty()) {
        throw std::runtime_error("Driver executable path must not be empty");
    }
    if (exe_path.find(L'"') != std::wstring_view::npos) {
        throw std::runtime_error("Driver executable path must not contain double quotes");
    }

    // CommandLineToArgvW scans a quoted argv[0] verbatim up to the closing
    // quote without unescaping \\ or \", so trailing backslashes in exe_path
    // must NOT be doubled.
    std::wstring cmdline;
    cmdline.reserve(
        exe_path.size() + dll_arg.size() + kSurroundingQuoteCount * kSurroundingQuoteCount);
    cmdline.push_back(L'"');
    cmdline.append(exe_path);
    cmdline.push_back(L'"');
    cmdline.push_back(L' ');
    cmdline.append(QuoteWindowsArgWide(dll_arg));
    return cmdline;
}

std::string ReadAllBytes(const fs::path& log_path) {
    std::ifstream file(log_path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

bool RunDriverExe(std::wstring_view exe_wide,
                  std::wstring_view dll_wide,
                  DWORD* out_exit_code) {
    std::wstring cmd_wide = FormatDriverCommandLineWide(exe_wide, dll_wide);
    const std::wstring app_wide(exe_wide);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(app_wide.c_str(),
                        cmd_wide.data(),
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

    const DWORD wait_rc = WaitForSingleObject(proc_handle.get(), kDriverTimeoutMs);
    if (wait_rc != WAIT_OBJECT_0) {
        TerminateProcess(proc_handle.get(), kTerminateTimeoutExitCode);
        return false;
    }
    if (!GetExitCodeProcess(proc_handle.get(), out_exit_code)) {
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

    const std::wstring driver_exe_wide = Utf8ToWide(DRIVER_EXE_PATH);
    const std::wstring proxy_dll_wide = Utf8ToWide(PROXY_DLL_PATH);
    const std::wstring log_file_wide = Utf8ToWide(LOG_FILE_PATH);
    ASSERT_FALSE(driver_exe_wide.empty());
    ASSERT_FALSE(proxy_dll_wide.empty());
    ASSERT_FALSE(log_file_wide.empty());

    const fs::path log_path = fs::u8path(LOG_FILE_PATH);
    std::error_code ec;
    fs::remove(log_path, ec);

    DWORD exit_code = kInitialSentinelExitCode;
    ASSERT_TRUE(RunDriverExe(driver_exe_wide, proxy_dll_wide, &exit_code))
        << "driver EXE failed to spawn or timed out; gle=" << GetLastError();

    std::cout << "[ NOTE     ] driver exit_code=" << exit_code
              << " (0 = clean ATTACH; non-zero = post-banner DllMain hook fault)\n";

    const std::string log = ReadAllBytes(log_path);
    ASSERT_FALSE(log.empty())
        << "log file empty or missing: " << LOG_FILE_PATH;

    EXPECT_NE(log.find(EXPECTED_BANNER), std::string::npos)
        << "log did not contain expected banner.\n"
        << "  expected substring: " << EXPECTED_BANNER << "\n"
        << "  log path:           " << LOG_FILE_PATH << "\n"
        << "  log contents:\n" << log;
}
