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
constexpr std::size_t kDoubleBackslashFactor = 2;
constexpr std::size_t kEscapedQuoteExtraBackslash = 1;

struct LocalFreeDeleter {
    void operator()(wchar_t** ptr) const noexcept {
        if (ptr != nullptr) {
            LocalFree(ptr);
        }
    }
};
using ScopedArgvW = std::unique_ptr<wchar_t*, LocalFreeDeleter>;

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

std::string wide_to_utf8_strict(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int byte_len = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (byte_len <= 0) {
        throw std::runtime_error("WideCharToMultiByte(UTF-8) failed on CLI argument");
    }
    std::string out(static_cast<std::size_t>(byte_len), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), out.data(), byte_len, nullptr, nullptr);
    if (written != byte_len) {
        throw std::runtime_error("WideCharToMultiByte(UTF-8) length mismatch");
    }
    return out;
}

std::wstring utf8_to_wide_strict(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int wide_len = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (wide_len <= 0) {
        throw std::runtime_error("MultiByteToWideChar(UTF-8) failed on process argument");
    }
    std::wstring out(static_cast<std::size_t>(wide_len), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), out.data(), wide_len);
    if (written != wide_len) {
        throw std::runtime_error("MultiByteToWideChar(UTF-8) length mismatch");
    }
    return out;
}

void append_escaped_argument(std::wstring& cmd_line, std::wstring_view arg) {
    const bool needs_quotes =
        arg.empty() || arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) {
        cmd_line.append(arg);
        return;
    }

    cmd_line.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            cmd_line.append(
                backslashes * kDoubleBackslashFactor + kEscapedQuoteExtraBackslash,
                L'\\');
            cmd_line.push_back(L'"');
            backslashes = 0;
            continue;
        }
        cmd_line.append(backslashes, L'\\');
        cmd_line.push_back(ch);
        backslashes = 0;
    }
    cmd_line.append(backslashes * kDoubleBackslashFactor, L'\\');
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

// Checks all ancestor levels for a developer checkout (pipeline_cpp + build.py)
// before checking staged distributions (bin + game or script_output), so
// binaries under <game>/build/install/bin resolve to <game> rather than
// <game>/build/install.
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
    wchar_t** argv_w = CommandLineToArgvW(cmd_line, &argc);
    if (argv_w == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed");
    }
    const ScopedArgvW guard(argv_w);
    if (argc <= kFirstUserArgIndex) {
        return {};
    }

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc - kFirstUserArgIndex));
    for (int i = kFirstUserArgIndex; i < argc; ++i) {
        args.push_back(wide_to_utf8_strict(argv_w[i]));
    }
    return args;
}

std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm) {
    try {
        const std::vector<std::string> args = process_utf8_args();
        const auto parsed = po::command_line_parser(args).options(desc).run();
        const auto stray =
            po::collect_unrecognized(parsed.options, po::include_positional);
        if (!stray.empty()) {
            std::cerr << "[ERROR] unexpected argument '" << stray.front() << "'\n\n"
                      << desc << std::endl;
            return kExitError;
        }

        po::store(parsed, vm);
        if (vm.count("help")) {
            std::cout << desc << std::endl;
            return kExitHelp;
        }
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
    return std::nullopt;
}

std::wstring build_process_command_line(std::string_view exe_path,
                                        const std::vector<std::string>& args) {
    if (exe_path.empty()) {
        throw std::runtime_error("Executable path must not be empty");
    }
    if (exe_path.find('"') != std::string_view::npos) {
        throw std::runtime_error("Executable path must not contain '\"'");
    }

    const std::wstring exe_w = utf8_to_wide_strict(exe_path);
    std::wstring cmd_line;
    cmd_line.push_back(L'"');
    cmd_line.append(exe_w);
    cmd_line.push_back(L'"');

    for (const std::string& arg : args) {
        const std::wstring arg_w = utf8_to_wide_strict(arg);
        cmd_line.push_back(L' ');
        append_escaped_argument(cmd_line, arg_w);
    }
    return cmd_line;
}

int run_process(std::string_view exe_path, const std::vector<std::string>& args) {
    std::wstring cmd_w = build_process_command_line(exe_path, args);
    const std::wstring exe_w = utf8_to_wide_strict(exe_path);

    STARTUPINFOW si{};
    si.cb = static_cast<DWORD>(sizeof(si));
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(exe_w.c_str(), cmd_w.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) {
        throw std::runtime_error("CreateProcessW failed for: " + std::string(exe_path));
    }
    const ScopedWinHandle proc_guard(pi.hProcess);
    const ScopedWinHandle thread_guard(pi.hThread);

    if (WaitForSingleObject(pi.hProcess, INFINITE) == WAIT_FAILED) {
        throw std::runtime_error("WaitForSingleObject failed for: " + std::string(exe_path));
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        throw std::runtime_error("GetExitCodeProcess failed for: " + std::string(exe_path));
    }
    return static_cast<int>(exit_code);
}

}  // namespace exm::apps
