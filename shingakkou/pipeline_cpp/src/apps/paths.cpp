// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Executable discovery, UTF-16 -> UTF-8 CLI decoding, and UTF-16 child-process
// spawning.  Isolated here so `<windows.h>` and `<shellapi.h>` do not leak into
// app headers.
#include "apps/paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace shin::apps {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kBufferGrowthFactor = 2;
constexpr int kMaxAncestorLevels = 8;
constexpr int kFirstProgramArgIndex = 1;
constexpr std::size_t kBackslashEscapeFactor = 2;
constexpr std::size_t kEscapedQuoteBackslashCount = 1;

class ScopedLocalArgv {
public:
    explicit ScopedLocalArgv(LPWSTR* argv) noexcept : argv_(argv) {}
    ScopedLocalArgv(const ScopedLocalArgv&) = delete;
    ScopedLocalArgv& operator=(const ScopedLocalArgv&) = delete;

    ~ScopedLocalArgv() {
        if (argv_ != nullptr) {
            LocalFree(argv_);
        }
    }

    LPWSTR* get() const noexcept { return argv_; }

private:
    LPWSTR* argv_;
};

class ScopedWinHandle {
public:
    explicit ScopedWinHandle(HANDLE handle) noexcept : handle_(handle) {}
    ScopedWinHandle(const ScopedWinHandle&) = delete;
    ScopedWinHandle& operator=(const ScopedWinHandle&) = delete;

    ~ScopedWinHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed for command-line argument");
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        utf8.data(), needed, nullptr, nullptr);
    if (written <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed for command-line argument");
    }
    return utf8;
}

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed for UTF-8 argument");
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
        wide.data(), needed);
    if (written <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed for UTF-8 argument");
    }
    return wide;
}

bool NeedsWindowsArgQuoting(std::wstring_view arg) {
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

void AppendQuotedWindowsArg(std::wstring& out, std::wstring_view arg) {
    if (!NeedsWindowsArgQuoting(arg)) {
        out.append(arg);
        return;
    }

    out.push_back(L'"');
    std::size_t pending_backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++pending_backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(
                pending_backslashes * kBackslashEscapeFactor + kEscapedQuoteBackslashCount,
                L'\\');
            out.push_back(L'"');
            pending_backslashes = 0;
            continue;
        }
        out.append(pending_backslashes, L'\\');
        pending_backslashes = 0;
        out.push_back(ch);
    }
    out.append(pending_backslashes * kBackslashEscapeFactor, L'\\');
    out.push_back(L'"');
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

std::string find_project_dir_from(const std::filesystem::path& start_dir) {
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

std::vector<std::string> process_utf8_args() {
    const LPCWSTR cmd_line = GetCommandLineW();
    if (cmd_line == nullptr || cmd_line[0] == L'\0') {
        return {};
    }

    int argc = 0;
    const ScopedLocalArgv argv(CommandLineToArgvW(cmd_line, &argc));
    if (argv.get() == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed");
    }

    std::vector<std::string> out;
    if (argc > kFirstProgramArgIndex) {
        out.reserve(static_cast<std::size_t>(argc - kFirstProgramArgIndex));
    }
    for (int i = kFirstProgramArgIndex; i < argc; ++i) {
        out.push_back(WideToUtf8(argv.get()[i]));
    }
    return out;
}

std::wstring build_process_command_line(std::string_view exe_path,
                                        const std::vector<std::string>& args) {
    if (exe_path.empty()) {
        throw std::runtime_error("executable path must not be empty");
    }
    if (exe_path.find('"') != std::string_view::npos) {
        throw std::runtime_error("executable path must not contain double quotes");
    }

    const std::wstring exe_w = Utf8ToWide(exe_path);
    std::wstring cmd;
    cmd.push_back(L'"');
    cmd.append(exe_w);
    cmd.push_back(L'"');

    for (const std::string& arg : args) {
        const std::wstring arg_w = Utf8ToWide(arg);
        cmd.push_back(L' ');
        AppendQuotedWindowsArg(cmd, arg_w);
    }
    return cmd;
}

int run_process(std::string_view exe_path, const std::vector<std::string>& args) {
    std::wstring cmd_w = build_process_command_line(exe_path, args);
    const std::wstring exe_w = Utf8ToWide(exe_path);

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION proc_info{};

    const BOOL created = CreateProcessW(
        exe_w.c_str(),
        cmd_w.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &startup_info,
        &proc_info);
    if (!created) {
        const DWORD err = GetLastError();
        throw std::runtime_error(
            "CreateProcessW failed for '" + std::string(exe_path) +
            "' (error " + std::to_string(err) + ")");
    }

    const ScopedWinHandle process_handle(proc_info.hProcess);
    const ScopedWinHandle thread_handle(proc_info.hThread);

    const DWORD wait_rc = WaitForSingleObject(process_handle.get(), INFINITE);
    if (wait_rc == WAIT_FAILED) {
        const DWORD err = GetLastError();
        throw std::runtime_error(
            "WaitForSingleObject failed (error " + std::to_string(err) + ")");
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process_handle.get(), &exit_code)) {
        const DWORD err = GetLastError();
        throw std::runtime_error(
            "GetExitCodeProcess failed (error " + std::to_string(err) + ")");
    }
    return static_cast<int>(exit_code);
}

}  // namespace shin::apps
