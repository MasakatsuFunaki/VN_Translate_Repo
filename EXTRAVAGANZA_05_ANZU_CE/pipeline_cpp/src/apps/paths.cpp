// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Executable location, UTF-8 project-root discovery, wide Windows CLI decoding,
// and wide CreateProcessW child-process execution.
#include "apps/paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <shellapi.h>

#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace exm::apps {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBufferGrowthFactor = 2;
constexpr int kMaxAncestorLevels = 8;
constexpr std::size_t kEscapedBackslashMultiplier = 2;

class ScopedLocalFree {
public:
    explicit ScopedLocalFree(wchar_t** ptr) noexcept : ptr_(ptr) {}
    ScopedLocalFree(const ScopedLocalFree&) = delete;
    ScopedLocalFree& operator=(const ScopedLocalFree&) = delete;

    ~ScopedLocalFree() {
        if (ptr_ != nullptr) {
            LocalFree(ptr_);
        }
    }

private:
    wchar_t** ptr_;
};

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

private:
    HANDLE handle_;
};

std::string utf16_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("Invalid UTF-16 sequence in wide string");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    }
    return out;
}

std::wstring utf8_to_utf16(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("Invalid UTF-8 sequence in argument string");
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), out.data(), needed);
    if (written != needed) {
        throw std::runtime_error("UTF-8 to UTF-16 conversion failed");
    }
    return out;
}

void append_quoted_windows_arg(std::wstring& cmd_line, std::wstring_view arg) {
    cmd_line.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            cmd_line.append(backslashes * kEscapedBackslashMultiplier + 1, L'\\');
            cmd_line.push_back(L'"');
            backslashes = 0;
            continue;
        }
        cmd_line.append(backslashes, L'\\');
        backslashes = 0;
        cmd_line.push_back(ch);
    }
    cmd_line.append(backslashes * kEscapedBackslashMultiplier, L'\\');
    cmd_line.push_back(L'"');
}

}  // namespace

std::string exe_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n =
            GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * kBufferGrowthFactor);
    }
    return fs::path(buf).parent_path().u8string();
}

std::string find_project_dir_from(const fs::path& start_dir) {
    if (start_dir.empty()) {
        return {};
    }
    std::error_code ec;
    fs::path d = start_dir;
    for (int level = 0; level <= kMaxAncestorLevels && !d.empty(); ++level) {
        if (fs::is_directory(d / "pipeline_cpp", ec) &&
            fs::is_regular_file(d / "build.py", ec)) {
            return d.u8string();
        }
        if (d == d.root_path()) {
            break;
        }
        d = d.parent_path();
    }

    d = start_dir;
    for (int level = 0; level <= kMaxAncestorLevels && !d.empty(); ++level) {
        if (fs::is_directory(d / "bin", ec) &&
            (fs::is_directory(d / "game", ec) ||
             fs::is_directory(d / "script_output", ec))) {
            return d.u8string();
        }
        if (d == d.root_path()) {
            break;
        }
        d = d.parent_path();
    }
    return {};
}

std::string default_project_dir() {
    const std::string exe = exe_dir();
    if (exe.empty()) {
        return {};
    }
    return find_project_dir_from(fs::u8path(exe));
}

std::optional<std::string> wide_env_utf8(const wchar_t* name) {
    if (name == nullptr) {
        return std::nullopt;
    }
    const wchar_t* val = _wgetenv(name);
    if (val == nullptr || *val == L'\0') {
        return std::nullopt;
    }
    return utf16_to_utf8(val);
}

std::vector<std::string> split_windows_command_line_utf8(std::wstring_view cmd_line) {
    if (cmd_line.empty()) {
        return {};
    }
    const std::wstring owned(cmd_line);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(owned.c_str(), &argc);
    const ScopedLocalFree guard(argv);
    if (argv == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed on command line");
    }

    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        out.push_back(utf16_to_utf8(argv[i]));
    }
    return out;
}

std::vector<std::string> process_utf8_args() {
    const wchar_t* raw_cmd = GetCommandLineW();
    if (raw_cmd == nullptr || *raw_cmd == L'\0') {
        return {};
    }

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(raw_cmd, &argc);
    const ScopedLocalFree guard(argv);
    if (argv == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed on process command line");
    }

    std::vector<std::string> out;
    if (argc > 1) {
        out.reserve(static_cast<std::size_t>(argc - 1));
    }
    for (int i = 1; i < argc; ++i) {
        out.push_back(utf16_to_utf8(argv[i]));
    }
    return out;
}

std::wstring build_windows_command_line(std::string_view exe_path,
                                        const std::vector<std::string>& args) {
    if (exe_path.empty()) {
        throw std::invalid_argument("Executable path must not be empty");
    }
    if (exe_path.find('"') != std::string_view::npos) {
        throw std::invalid_argument("Executable path must not contain double quotes");
    }

    const std::wstring wide_exe = utf8_to_utf16(exe_path);
    std::wstring cmd_line;
    cmd_line.push_back(L'"');
    cmd_line.append(wide_exe);
    cmd_line.push_back(L'"');

    for (const std::string& arg : args) {
        cmd_line.push_back(L' ');
        append_quoted_windows_arg(cmd_line, utf8_to_utf16(arg));
    }
    return cmd_line;
}

int run_child_process(std::string_view exe_path, const std::vector<std::string>& args) {
    std::wstring cmd_line = build_windows_command_line(exe_path, args);
    const std::wstring wide_exe = utf8_to_utf16(exe_path);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(wide_exe.c_str(), cmd_line.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) {
        const DWORD err = GetLastError();
        throw std::runtime_error("CreateProcessW failed for '" + std::string(exe_path) +
                                 "' (Win32 error " + std::to_string(err) + ")");
    }

    const ScopedHandle proc_guard(pi.hProcess);
    const ScopedHandle thread_guard(pi.hThread);

    const DWORD wait_rc = WaitForSingleObject(pi.hProcess, INFINITE);
    if (wait_rc != WAIT_OBJECT_0) {
        const DWORD err = GetLastError();
        throw std::runtime_error("WaitForSingleObject failed for '" + std::string(exe_path) +
                                 "' (Win32 error " + std::to_string(err) + ")");
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        const DWORD err = GetLastError();
        throw std::runtime_error("GetExitCodeProcess failed for '" + std::string(exe_path) +
                                 "' (Win32 error " + std::to_string(err) + ")");
    }
    return static_cast<int>(exit_code);
}

}  // namespace exm::apps
