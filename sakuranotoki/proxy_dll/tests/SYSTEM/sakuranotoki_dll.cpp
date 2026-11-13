// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// End-to-end black-box test for the built proxy DLL.
//
// Spawns sakuranotoki_driver via CreateProcessW with wide UTF-16 paths so
// non-ASCII checkout and build paths work without narrow-codepage loss, waits
// for exit, and verifies the proxy log contains EXPECTED_BANNER.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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

constexpr DWORD kDriverWaitTimeoutMs = 30000U;
constexpr UINT kTimeoutTerminateExitCode = 99U;
constexpr DWORD kUnsetExitCodeSentinel = 0xDEADBEEFU;
constexpr std::size_t kBackslashPairMultiplier = 2;
constexpr std::size_t kEscapedQuoteBackslash = 1;

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
    HANDLE h_{nullptr};
};

bool Utf8ToWide(std::string_view utf8, std::wstring& out) {
    if (utf8.empty()) {
        out.clear();
        return true;
    }
    if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int src_len = static_cast<int>(utf8.size());
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), src_len, nullptr, 0);
    if (needed <= 0) {
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), src_len, wide.data(), needed);
    if (written != needed) {
        return false;
    }
    out = std::move(wide);
    return true;
}

void AppendEscapedWideArg(std::wstring& out, std::wstring_view arg) {
    out.push_back(L'"');
    std::size_t pending_backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++pending_backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(
                pending_backslashes * kBackslashPairMultiplier + kEscapedQuoteBackslash,
                L'\\');
            out.push_back(L'"');
            pending_backslashes = 0;
            continue;
        }
        out.append(pending_backslashes, L'\\');
        pending_backslashes = 0;
        out.push_back(ch);
    }
    out.append(pending_backslashes * kBackslashPairMultiplier, L'\\');
    out.push_back(L'"');
}

// CommandLineToArgvW treats quoted argv[0] verbatim without unescaping `\\` or
// `\"`, whereas argv[1..] uses 2n/2n+1 backslash escaping before quotes.
bool FormatDriverCommandLine(std::wstring_view exe_path,
                             std::wstring_view dll_arg,
                             std::wstring& out) {
    if (exe_path.empty() || exe_path.find(L'"') != std::wstring_view::npos) {
        return false;
    }
    out.clear();
    out.push_back(L'"');
    out.append(exe_path);
    out.push_back(L'"');
    out.push_back(L' ');
    AppendEscapedWideArg(out, dll_arg);
    return true;
}

std::string ReadAllBytes(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool RunDriverExe(const std::wstring& exe_path,
                  const std::wstring& dll_arg,
                  DWORD* out_exit_code) {
    if (out_exit_code == nullptr) {
        return false;
    }
    std::wstring cmdline;
    if (!FormatDriverCommandLine(exe_path, dll_arg, cmdline)) {
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_path.c_str(), cmdline.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
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

    std::wstring wide_driver_exe;
    std::wstring wide_proxy_dll;
    std::wstring wide_log_file;
    ASSERT_TRUE(Utf8ToWide(DRIVER_EXE_PATH, wide_driver_exe))
        << "DRIVER_EXE_PATH is not valid UTF-8";
    ASSERT_TRUE(Utf8ToWide(PROXY_DLL_PATH, wide_proxy_dll))
        << "PROXY_DLL_PATH is not valid UTF-8";
    ASSERT_TRUE(Utf8ToWide(LOG_FILE_PATH, wide_log_file))
        << "LOG_FILE_PATH is not valid UTF-8";
    ASSERT_FALSE(wide_log_file.empty());

    std::error_code ec;
    fs::remove(fs::u8path(LOG_FILE_PATH), ec);

    DWORD exit_code = kUnsetExitCodeSentinel;
    ASSERT_TRUE(RunDriverExe(wide_driver_exe, wide_proxy_dll, &exit_code))
        << "driver EXE failed to spawn or timed out (gle="
        << GetLastError() << ")";

    std::cout << "[ NOTE     ] driver exit_code=" << exit_code
              << " (0 = clean ATTACH; non-zero usually = post-banner DllMain hook crash)\n";

    const std::string log = ReadAllBytes(fs::u8path(LOG_FILE_PATH));
    ASSERT_FALSE(log.empty())
        << "log file empty or missing: " << LOG_FILE_PATH
        << " -- did DllMain run and reach log_init?";

    EXPECT_NE(log.find(EXPECTED_BANNER), std::string::npos)
        << "log did not contain expected banner.\n"
        << "  expected substring: " << EXPECTED_BANNER << "\n"
        << "  log path:           " << LOG_FILE_PATH << "\n"
        << "  log contents:\n" << log;
}
