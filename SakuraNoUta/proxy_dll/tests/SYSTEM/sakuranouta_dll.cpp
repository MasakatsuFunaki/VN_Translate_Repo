// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// End-to-end black-box test: spawn sakuranouta_driver via CreateProcessW to
// LoadLibraryW the proxy DLL, then assert the expected banner appears in the
// generated log file.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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

#ifndef PROXY_DLL_PATH
#  define PROXY_DLL_PATH ""
#endif
#ifndef DRIVER_EXE_PATH
#  define DRIVER_EXE_PATH ""
#endif
#ifndef LOG_FILE_PATH
#  define LOG_FILE_PATH ""
#endif
#ifndef EXPECTED_BANNER
#  define EXPECTED_BANNER ""
#endif

namespace {

namespace fs = std::filesystem;

constexpr DWORD kDriverWaitTimeoutMs = 30'000;
constexpr UINT kTimeoutTerminateExitCode = 99;
constexpr DWORD kInitialExitCodeSentinel = 0xDEADBEEFu;
constexpr std::size_t kEscapedBackslashMultiplier = 2;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

bool Utf8ToWideStrict(std::string_view utf8, std::wstring& out) {
    out.clear();
    if (utf8.empty()) {
        return true;
    }
    const int len = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) {
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), wide.data(), len);
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
                pending_backslashes * kEscapedBackslashMultiplier + 1, L'\\');
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
    std::ifstream file(fs::u8path(utf8_path), std::ios::binary);
    if (!file) {
        return {};
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool RunDriverExe(std::string_view exe_utf8, std::string_view dll_utf8,
                  DWORD* out_exit_code) {
    if (exe_utf8.find('"') != std::string_view::npos) {
        return false;
    }
    std::wstring exe_w;
    std::wstring dll_w;
    if (!Utf8ToWideStrict(exe_utf8, exe_w) || exe_w.empty()) {
        return false;
    }
    if (!Utf8ToWideStrict(dll_utf8, dll_w) || dll_w.empty()) {
        return false;
    }
    std::wstring cmd_w = L"\"" + exe_w + L"\" " + QuoteWindowsArg(dll_w);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_w.c_str(), cmd_w.data(), nullptr, nullptr, FALSE,
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

    std::wstring log_path_w;
    ASSERT_TRUE(Utf8ToWideStrict(LOG_FILE_PATH, log_path_w))
        << "LOG_FILE_PATH is not valid UTF-8: " << LOG_FILE_PATH;
    ASSERT_FALSE(log_path_w.empty());

    // Remove any leftover log from a previous test run so the bytes read
    // below come exclusively from this driver invocation.
    std::error_code ec;
    fs::remove(fs::u8path(LOG_FILE_PATH), ec);

    DWORD exit_code = kInitialExitCodeSentinel;
    ASSERT_TRUE(RunDriverExe(DRIVER_EXE_PATH, PROXY_DLL_PATH, &exit_code))
        << "driver EXE failed to spawn or timed out -- DllMain may "
           "have hung, or CreateProcessW failed (gle="
        << GetLastError() << ")";

    std::cout << "[ NOTE     ] driver exit_code=" << exit_code
              << " (0 = clean ATTACH; non-zero usually = post-banner DllMain hook crash)\n";

    const std::string log = ReadAllBytes(LOG_FILE_PATH);
    ASSERT_FALSE(log.empty())
        << "log file empty or missing: " << LOG_FILE_PATH
        << " -- did DllMain run and reach LogInit/log_init?";

    EXPECT_NE(log.find(EXPECTED_BANNER), std::string::npos)
        << "log did not contain expected banner.\n"
        << "  expected substring: " << EXPECTED_BANNER << "\n"
        << "  log path:           " << LOG_FILE_PATH << "\n"
        << "  log contents:\n" << log;
}
