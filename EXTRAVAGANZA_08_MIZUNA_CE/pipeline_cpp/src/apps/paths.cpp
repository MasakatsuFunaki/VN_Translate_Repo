// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

#include "apps/paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
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
constexpr std::size_t kBackslashDoublingFactor = 2;
constexpr std::size_t kQuoteEscapeBackslashAddend = 1;

class ScopedLocalArgv {
public:
    explicit ScopedLocalArgv(LPWSTR* argv) noexcept : argv_(argv) {}
    ~ScopedLocalArgv() {
        if (argv_ != nullptr) {
            LocalFree(argv_);
        }
    }

    ScopedLocalArgv(const ScopedLocalArgv&) = delete;
    ScopedLocalArgv& operator=(const ScopedLocalArgv&) = delete;

    LPWSTR* get() const noexcept { return argv_; }
    explicit operator bool() const noexcept { return argv_ != nullptr; }

private:
    LPWSTR* argv_;
};

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

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("Invalid UTF-16 sequence in command-line argument");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        out.data(), needed, nullptr, nullptr);
    if (written <= 0) {
        throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    }
    return out;
}

std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("Invalid UTF-8 sequence in process argument");
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
        out.data(), needed);
    if (written <= 0) {
        throw std::runtime_error("UTF-8 to UTF-16 conversion failed");
    }
    return out;
}

void append_command_line_arg(std::wstring& out, std::wstring_view arg) {
    const bool needs_quotes =
        arg.empty() || arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) {
        out.append(arg);
        return;
    }

    out.push_back(L'"');
    std::size_t i = 0;
    while (i < arg.size()) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * kBackslashDoublingFactor, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * kBackslashDoublingFactor + kQuoteEscapeBackslashAddend,
                       L'\\');
            out.push_back(L'"');
            ++i;
            continue;
        }
        out.append(backslashes, L'\\');
        out.push_back(arg[i]);
        ++i;
    }
    out.push_back(L'"');
}

std::optional<int> finalize_parsed_options(const po::parsed_options& parsed,
                                           const po::options_description& desc,
                                           po::variables_map& vm) {
    const auto stray = po::collect_unrecognized(parsed.options, po::include_positional);
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
    return std::nullopt;
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

po::options_description common_options(std::string& project_dir,
                                       std::string& game_dir) {
    auto* dir = po::value(&project_dir);
    const std::string found = default_project_dir();
    if (found.empty()) {
        dir->required();
    }
    if (!found.empty()) {
        dir->default_value(found);
    }

    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("dir", dir, "EXTRA_VA_MIZUNA_CE project folder")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

po::options_description game_only_options(std::string& game_dir) {
    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

std::vector<std::string> process_utf8_args() {
    const LPCWSTR cmd_line = GetCommandLineW();
    if (cmd_line == nullptr || *cmd_line == L'\0') {
        return {};
    }
    int argc = 0;
    const ScopedLocalArgv argv_w(CommandLineToArgvW(cmd_line, &argc));
    if (!argv_w) {
        throw std::runtime_error("CommandLineToArgvW failed");
    }
    if (argc <= kFirstUserArgIndex) {
        return {};
    }
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc - kFirstUserArgIndex));
    for (int i = kFirstUserArgIndex; i < argc; ++i) {
        const LPCWSTR arg = argv_w.get()[i];
        args.push_back(arg != nullptr ? wide_to_utf8(arg) : std::string{});
    }
    return args;
}

std::optional<int> parse_command_line(int argc, char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm) {
    try {
        const auto parsed = po::command_line_parser(argc, argv).options(desc).run();
        return finalize_parsed_options(parsed, desc, vm);
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
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
        std::cerr << "[ERROR] " << e.what() << std::endl;
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
    std::wstring cmd;
    cmd.push_back(L'"');
    cmd.append(exe_w);
    cmd.push_back(L'"');

    for (const std::string& arg : args) {
        cmd.push_back(L' ');
        append_command_line_arg(cmd, utf8_to_wide(arg));
    }
    return cmd;
}

int run_process(std::string_view exe_path,
                const std::vector<std::string>& args) {
    std::wstring cmd_w = build_process_command_line(exe_path, args);
    const std::wstring exe_w = utf8_to_wide(exe_path);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(exe_w.c_str(), cmd_w.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        throw std::runtime_error(
            "CreateProcessW failed for '" + std::string(exe_path) +
            "' (Win32 error " + std::to_string(GetLastError()) + ")");
    }

    const ScopedWinHandle proc_guard(pi.hProcess);
    const ScopedWinHandle thread_guard(pi.hThread);

    if (WaitForSingleObject(proc_guard.get(), INFINITE) == WAIT_FAILED) {
        throw std::runtime_error("WaitForSingleObject failed for '" +
                                 std::string(exe_path) + "'");
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(proc_guard.get(), &exit_code)) {
        throw std::runtime_error("GetExitCodeProcess failed for '" +
                                 std::string(exe_path) + "'");
    }
    return static_cast<int>(exit_code);
}

}  // namespace exm::apps
