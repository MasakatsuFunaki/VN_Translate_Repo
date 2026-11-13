// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

#include "apps/paths.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
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
constexpr int kFirstUserArgIndex = 1;
constexpr std::size_t kBackslashDoubleFactor = 2;

struct LocalArgvDeleter {
    void operator()(wchar_t** ptr) const noexcept {
        if (ptr != nullptr) {
            LocalFree(ptr);
        }
    }
};
using ScopedLocalArgv = std::unique_ptr<wchar_t*, LocalArgvDeleter>;

class ScopedWinHandle {
public:
    explicit ScopedWinHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~ScopedWinHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    ScopedWinHandle(const ScopedWinHandle&) = delete;
    ScopedWinHandle& operator=(const ScopedWinHandle&) = delete;

private:
    HANDLE handle_;
};

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int byte_len = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (byte_len <= 0) {
        throw std::runtime_error("WideCharToMultiByte(CP_UTF8) size query failed");
    }
    std::string out(static_cast<std::size_t>(byte_len), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), out.data(), byte_len, nullptr, nullptr);
    if (written != byte_len) {
        throw std::runtime_error("WideCharToMultiByte(CP_UTF8) conversion failed");
    }
    return out;
}

std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int wide_len = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (wide_len <= 0) {
        throw std::runtime_error("MultiByteToWideChar(CP_UTF8) size query failed");
    }
    std::wstring out(static_cast<std::size_t>(wide_len), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), out.data(), wide_len);
    if (written != wide_len) {
        throw std::runtime_error("MultiByteToWideChar(CP_UTF8) conversion failed");
    }
    return out;
}

bool arg_needs_quotes(std::wstring_view arg) noexcept {
    if (arg.empty()) {
        return true;
    }
    for (wchar_t ch : arg) {
        if (ch == L' ' || ch == L'\t' || ch == L'\n' ||
            ch == L'\v' || ch == L'\r' || ch == L'"') {
            return true;
        }
    }
    return false;
}

void append_escaped_arg(std::wstring& cmd_line, std::wstring_view arg) {
    if (!arg_needs_quotes(arg)) {
        cmd_line.append(arg);
        return;
    }

    cmd_line.push_back(L'"');
    std::size_t pending_backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++pending_backslashes;
            continue;
        }
        if (ch == L'"') {
            cmd_line.append(pending_backslashes * kBackslashDoubleFactor + 1, L'\\');
            cmd_line.push_back(L'"');
            pending_backslashes = 0;
            continue;
        }
        cmd_line.append(pending_backslashes, L'\\');
        pending_backslashes = 0;
        cmd_line.push_back(ch);
    }
    cmd_line.append(pending_backslashes * kBackslashDoubleFactor, L'\\');
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

std::vector<std::string> process_utf8_args() {
    const wchar_t* cmd_line = GetCommandLineW();
    if (cmd_line == nullptr || *cmd_line == L'\0') {
        return {};
    }

    int argc = 0;
    const ScopedLocalArgv argv(CommandLineToArgvW(cmd_line, &argc));
    if (!argv || argc <= 0) {
        throw std::runtime_error("CommandLineToArgvW failed");
    }

    std::vector<std::string> args;
    if (argc > kFirstUserArgIndex) {
        args.reserve(static_cast<std::size_t>(argc - kFirstUserArgIndex));
    }
    for (int i = kFirstUserArgIndex; i < argc; ++i) {
        const std::wstring_view wide_arg =
            argv.get()[i] != nullptr ? std::wstring_view(argv.get()[i]) : std::wstring_view{};
        args.push_back(wide_to_utf8(wide_arg));
    }
    return args;
}

std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm) {
    try {
        const std::vector<std::string> args = process_utf8_args();
        const auto parsed = po::command_line_parser(args).options(desc).run();
        return finalize_parsed_options(parsed, desc, vm);
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
}

std::wstring build_process_command_line(std::string_view exe_path,
                                        const std::vector<std::string>& args) {
    if (exe_path.empty()) {
        throw std::runtime_error("Executable path must not be empty");
    }
    if (exe_path.find('"') != std::string_view::npos) {
        throw std::runtime_error("Executable path must not contain double quotes");
    }

    const std::wstring exe_w = utf8_to_wide(exe_path);
    std::wstring cmd_line;
    cmd_line.push_back(L'"');
    cmd_line.append(exe_w);
    cmd_line.push_back(L'"');

    for (const std::string& arg : args) {
        cmd_line.push_back(L' ');
        append_escaped_arg(cmd_line, utf8_to_wide(arg));
    }
    return cmd_line;
}

int run_process(std::string_view exe_path, const std::vector<std::string>& args) {
    std::wstring cmd_w = build_process_command_line(exe_path, args);
    const std::wstring exe_w = utf8_to_wide(exe_path);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    const BOOL started = CreateProcessW(
        exe_w.c_str(), cmd_w.data(), nullptr, nullptr, FALSE, 0,
        nullptr, nullptr, &si, &pi);
    if (started == FALSE) {
        throw std::runtime_error(
            "CreateProcessW failed for '" + std::string(exe_path) +
            "' (error " + std::to_string(GetLastError()) + ")");
    }

    const ScopedWinHandle proc_guard(pi.hProcess);
    const ScopedWinHandle thread_guard(pi.hThread);

    if (WaitForSingleObject(pi.hProcess, INFINITE) == WAIT_FAILED) {
        throw std::runtime_error(
            "WaitForSingleObject failed for '" + std::string(exe_path) +
            "' (error " + std::to_string(GetLastError()) + ")");
    }

    DWORD exit_code = 0;
    if (GetExitCodeProcess(pi.hProcess, &exit_code) == FALSE) {
        throw std::runtime_error(
            "GetExitCodeProcess failed for '" + std::string(exe_path) +
            "' (error " + std::to_string(GetLastError()) + ")");
    }
    return static_cast<int>(exit_code);
}

}  // namespace exm::apps
