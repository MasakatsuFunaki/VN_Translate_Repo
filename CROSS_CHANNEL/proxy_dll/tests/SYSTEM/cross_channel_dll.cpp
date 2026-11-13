// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// End-to-end black-box system test for the built CROSS_CHANNEL proxy DLL.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
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

namespace {

namespace fs = std::filesystem;

constexpr DWORD kDriverWaitTimeoutMs = 30000u;
constexpr UINT kTimeoutTerminateCode = 99u;
constexpr DWORD kSentinelExitCode = 0xDEADBEEFu;
constexpr std::size_t kBackslashPairFactor = 2u;
constexpr std::size_t kQuoteEscapeBackslashCount = 1u;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = nullptr) : h_(h) {}
    ~ScopedHandle() {
        if (valid()) {
            CloseHandle(h_);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const { return h_; }
    bool valid() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_;
};

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* prefix) {
        static std::atomic<unsigned int> seq{0};
        const std::string dir_name =
            std::string(prefix) + std::to_string(::GetCurrentProcessId()) + "_" +
            std::to_string(++seq);
        dir_ = fs::temp_directory_path() / fs::u8path(dir_name);
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

class ScopedFileRemover {
public:
    explicit ScopedFileRemover(fs::path path) : path_(std::move(path)) {
        RemoveNow();
    }

    ~ScopedFileRemover() {
        RemoveNow();
    }

    ScopedFileRemover(const ScopedFileRemover&) = delete;
    ScopedFileRemover& operator=(const ScopedFileRemover&) = delete;

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

bool WidenUtf8(const char* utf8, std::wstring& out) {
    if (utf8 == nullptr || *utf8 == '\0') {
        out.clear();
        return false;
    }
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (needed <= 0) {
        return false;
    }
    out.assign(static_cast<std::size_t>(needed), L'\0');
    const int written =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, &out[0], needed);
    if (written != needed) {
        out.clear();
        return false;
    }
    if (!out.empty() && out.back() == L'\0') {
        out.pop_back();
    }
    return !out.empty();
}

std::wstring QuoteCommandLineArgW(const std::wstring& arg) {
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslashes * kBackslashPairFactor + kQuoteEscapeBackslashCount, L'\\');
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
    try {
        std::ifstream f(path, std::ios::binary);
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

bool RunDriverExe(const char* exe_path,
                  const char* dll_arg,
                  const fs::path& work_dir,
                  DWORD* out_exit_code) {
    std::wstring exe_w;
    std::wstring dll_w;
    if (!WidenUtf8(exe_path, exe_w) || !WidenUtf8(dll_arg, dll_w)) {
        return false;
    }

    const std::wstring cmd =
        L"\"" + exe_w + L"\" " + QuoteCommandLineArgW(dll_w);
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    const std::wstring work_dir_w = work_dir.wstring();
    const wchar_t* work_dir_ptr = work_dir_w.empty() ? nullptr : work_dir_w.c_str();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_w.c_str(), cmd_buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, work_dir_ptr, &si, &pi)) {
        return false;
    }
    ScopedHandle proc_guard(pi.hProcess);
    ScopedHandle thread_guard(pi.hThread);

    const DWORD wait_rc = WaitForSingleObject(proc_guard.get(), kDriverWaitTimeoutMs);
    if (wait_rc != WAIT_OBJECT_0) {
        TerminateProcess(proc_guard.get(), kTimeoutTerminateCode);
        return false;
    }
    return GetExitCodeProcess(proc_guard.get(), out_exit_code) != FALSE;
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

    const ScopedTempDir temp_dir("crc_proxy_dll_st_");
    const fs::path configured_log = fs::u8path(LOG_FILE_PATH);
    const ScopedFileRemover cwd_log_guard(configured_log);
    const fs::path active_log_path =
        configured_log.is_absolute() ? configured_log : (temp_dir.path() / configured_log);
    const ScopedFileRemover active_log_guard(active_log_path);

    DWORD exit_code = kSentinelExitCode;
    ASSERT_TRUE(RunDriverExe(DRIVER_EXE_PATH, PROXY_DLL_PATH, temp_dir.path(), &exit_code))
        << "driver EXE failed to spawn or timed out (gle=" << GetLastError() << ")";

    std::cout << "[ NOTE     ] driver exit_code=" << exit_code << "\n";

    const std::string log = ReadAllBytes(active_log_path);
    ASSERT_FALSE(log.empty())
        << "log file empty or missing: " << active_log_path.u8string();

    EXPECT_NE(log.find(EXPECTED_BANNER), std::string::npos)
        << "log did not contain expected banner.\n"
        << "  expected substring: " << EXPECTED_BANNER << "\n"
        << "  log path:           " << active_log_path.u8string() << "\n"
        << "  log contents:\n" << log;
}
