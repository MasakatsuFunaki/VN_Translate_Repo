// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "common/utf8_text.h"
#include "common/win32_cli_env.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace spot_checker::test_support {

inline constexpr DWORD kDefaultProcessTimeoutMs = 60000;
inline constexpr DWORD kTerminateWaitMs = 5000;
inline constexpr std::size_t kPipeReadChunkBytes = 4096;

class UniqueWinHandle {
public:
    UniqueWinHandle() noexcept = default;
    explicit UniqueWinHandle(HANDLE h) noexcept : handle_(h) {}

    ~UniqueWinHandle() { reset(); }

    UniqueWinHandle(const UniqueWinHandle&) = delete;
    UniqueWinHandle& operator=(const UniqueWinHandle&) = delete;

    UniqueWinHandle(UniqueWinHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    UniqueWinHandle& operator=(UniqueWinHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    HANDLE get() const noexcept { return handle_; }

    void reset(HANDLE replacement = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_ = nullptr;
};

// Self-cleaning RAII sandbox directory rooted under the OS temporary folder.
class TempSandbox {
public:
    explicit TempSandbox(std::string_view prefix = "spot_checker_st") {
        static std::atomic<std::uint64_t> counter{1};
        const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto pid = static_cast<std::uint64_t>(GetCurrentProcessId());

        std::ostringstream folder_name;
        folder_name << prefix << "_" << pid << "_" << tick << "_" << seq;
        root_ = std::filesystem::temp_directory_path() / folder_name.str();
        std::filesystem::create_directories(root_);
    }

    ~TempSandbox() noexcept {
        if (root_.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    TempSandbox(const TempSandbox&) = delete;
    TempSandbox& operator=(const TempSandbox&) = delete;

    TempSandbox(TempSandbox&& other) noexcept : root_(std::move(other.root_)) {
        other.root_.clear();
    }

    TempSandbox& operator=(TempSandbox&& other) noexcept {
        if (this != &other) {
            std::error_code ec;
            if (!root_.empty()) {
                std::filesystem::remove_all(root_, ec);
            }
            root_ = std::move(other.root_);
            other.root_.clear();
        }
        return *this;
    }

    const std::filesystem::path& path() const noexcept { return root_; }

    std::filesystem::path write_bytes(const std::filesystem::path& relative_path,
                                      std::string_view content) const {
        const auto full_path = root_ / relative_path;
        if (full_path.has_parent_path()) {
            std::filesystem::create_directories(full_path.parent_path());
        }
        std::ofstream out(full_path, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        return full_path;
    }

    static std::string read_bytes(const std::filesystem::path& file_path) {
        std::ifstream in(file_path, std::ios::binary);
        if (!in) {
            return {};
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

private:
    std::filesystem::path root_;
};

struct ProcessRunResult {
    int exit_code = -1;
    bool timed_out = false;
    bool launch_failed = false;
    std::string stdout_text;
    std::string stderr_text;
    std::string launch_error;
};

inline std::wstring quote_win32_arg(std::wstring_view arg) {
    if (!arg.empty() &&
        arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }

    std::wstring quoted;
    quoted.push_back(L'"');
    for (std::size_t i = 0; i < arg.size();) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            quoted.append(backslashes * 2U, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            quoted.append(backslashes * 2U + 1U, L'\\');
            quoted.push_back(L'"');
            ++i;
            continue;
        }
        quoted.append(backslashes, L'\\');
        quoted.push_back(arg[i]);
        ++i;
    }
    quoted.push_back(L'"');
    return quoted;
}

inline void drain_pipe_to_string(HANDLE read_pipe, std::string& out) {
    char buffer[kPipeReadChunkBytes];
    DWORD bytes_read = 0;
    while (ReadFile(read_pipe, buffer, static_cast<DWORD>(sizeof(buffer)),
                    &bytes_read, nullptr) != FALSE &&
           bytes_read > 0) {
        out.append(buffer, static_cast<std::size_t>(bytes_read));
    }
}

inline std::vector<wchar_t> build_child_environment_block(
    const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::wstring> entries;
    LPWCH raw_env = GetEnvironmentStringsW();
    if (raw_env != nullptr) {
        const wchar_t* cur = raw_env;
        while (*cur != L'\0') {
            const std::wstring item(cur);
            cur += item.size() + 1U;
            entries.push_back(item);
        }
        FreeEnvironmentStringsW(raw_env);
    }

    for (const auto& kv : overrides) {
        const std::wstring key_w = utf8_to_wide(kv.first);
        std::wstring prefix_upper = key_w;
        for (wchar_t& ch : prefix_upper) {
            if (ch >= L'a' && ch <= L'z') {
                ch = static_cast<wchar_t>(ch - L'a' + L'A');
            }
        }
        prefix_upper.push_back(L'=');

        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                           [&](const std::wstring& existing) {
                               if (existing.size() < prefix_upper.size()) {
                                   return false;
                               }
                               for (std::size_t i = 0; i < prefix_upper.size(); ++i) {
                                   wchar_t c = existing[i];
                                   if (c >= L'a' && c <= L'z') {
                                       c = static_cast<wchar_t>(c - L'a' + L'A');
                                   }
                                   if (c != prefix_upper[i]) {
                                       return false;
                                   }
                               }
                               return true;
                           }),
            entries.end());
        entries.push_back(key_w + L"=" + utf8_to_wide(kv.second));
    }

    std::vector<wchar_t> block;
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// Spawns `exe_path` via Win32 `CreateProcessW` with captured stdout/stderr pipes
// and explicit `WaitForSingleObject` timeout (zero arbitrary sleeps).
inline ProcessRunResult run_process_utf8(
    const std::filesystem::path& exe_path,
    const std::vector<std::string>& utf8_args,
    const std::filesystem::path& working_dir = {},
    DWORD timeout_ms = kDefaultProcessTimeoutMs,
    const std::vector<std::pair<std::string, std::string>>& env_overrides = {}) {
    ProcessRunResult result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE out_read_raw = nullptr;
    HANDLE out_write_raw = nullptr;
    if (CreatePipe(&out_read_raw, &out_write_raw, &sa, 0) == FALSE) {
        result.launch_failed = true;
        result.launch_error = "CreatePipe(stdout) failed";
        return result;
    }
    UniqueWinHandle out_read(out_read_raw);
    UniqueWinHandle out_write(out_write_raw);
    SetHandleInformation(out_read.get(), HANDLE_FLAG_INHERIT, 0);

    HANDLE err_read_raw = nullptr;
    HANDLE err_write_raw = nullptr;
    if (CreatePipe(&err_read_raw, &err_write_raw, &sa, 0) == FALSE) {
        result.launch_failed = true;
        result.launch_error = "CreatePipe(stderr) failed";
        return result;
    }
    UniqueWinHandle err_read(err_read_raw);
    UniqueWinHandle err_write(err_write_raw);
    SetHandleInformation(err_read.get(), HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd_line = quote_win32_arg(exe_path.wstring());
    for (const auto& arg : utf8_args) {
        cmd_line.push_back(L' ');
        cmd_line.append(quote_win32_arg(utf8_to_wide(arg)));
    }
    std::vector<wchar_t> mutable_cmd(cmd_line.begin(), cmd_line.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_write.get();
    si.hStdError = err_write.get();

    PROCESS_INFORMATION pi{};
    const std::wstring cwd_wide =
        working_dir.empty() ? std::wstring{} : working_dir.wstring();
    const wchar_t* cwd_ptr = working_dir.empty() ? nullptr : cwd_wide.c_str();

    std::vector<wchar_t> env_block;
    DWORD creation_flags = CREATE_NO_WINDOW;
    LPVOID env_ptr = nullptr;
    if (!env_overrides.empty()) {
        env_block = build_child_environment_block(env_overrides);
        env_ptr = env_block.data();
        creation_flags |= CREATE_UNICODE_ENVIRONMENT;
    }

    const BOOL created = CreateProcessW(
        exe_path.c_str(),
        mutable_cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        creation_flags,
        env_ptr,
        cwd_ptr,
        &si,
        &pi);

    out_write.reset();
    err_write.reset();

    if (created == FALSE) {
        result.launch_failed = true;
        result.launch_error =
            "CreateProcessW failed with error " + std::to_string(GetLastError());
        return result;
    }

    UniqueWinHandle proc_handle(pi.hProcess);
    UniqueWinHandle thread_handle(pi.hThread);

    std::thread stdout_reader(
        [&]() { drain_pipe_to_string(out_read.get(), result.stdout_text); });
    std::thread stderr_reader(
        [&]() { drain_pipe_to_string(err_read.get(), result.stderr_text); });

    const DWORD wait_rc = WaitForSingleObject(proc_handle.get(), timeout_ms);
    if (wait_rc == WAIT_TIMEOUT) {
        result.timed_out = true;
        TerminateProcess(proc_handle.get(), 1);
        WaitForSingleObject(proc_handle.get(), kTerminateWaitMs);
    }

    stdout_reader.join();
    stderr_reader.join();

    DWORD raw_exit_code = 1;
    if (GetExitCodeProcess(proc_handle.get(), &raw_exit_code) != FALSE) {
        result.exit_code = static_cast<int>(raw_exit_code);
    }
    return result;
}

// Locates the staged/installed `install/bin/01_spot_check.exe` artifact relative
// to the running test binary or repository root (never falling back to raw
// uninstalled build intermediates).
inline std::filesystem::path locate_installed_spot_check_exe() {
    const auto self_exe = executable_path();
    std::error_code ec;
    for (auto dir = self_exe.parent_path(); !dir.empty();) {
        const auto staged = dir / "install" / "bin" / "01_spot_check.exe";
        if (std::filesystem::is_regular_file(staged, ec)) {
            return staged;
        }
        const auto parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }

    const auto repo_root = find_repo_root_from(self_exe);
    if (!repo_root.empty()) {
        return repo_root / "tools" / "spot_checker" / "build" / "install" /
               "bin" / "01_spot_check.exe";
    }
    return std::filesystem::path("install") / "bin" / "01_spot_check.exe";
}

}  // namespace spot_checker::test_support
