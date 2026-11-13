// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Separated from paths.h to keep <windows.h> out of every app main.
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

namespace frat::apps {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBufferGrowthFactor = 2;
constexpr std::size_t kBackslashEscapeFactor = 2;
constexpr int kMaxAncestorLevels = 8;
constexpr int kFirstUserArgIndex = 1;

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

private:
    LPWSTR* argv_;
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
    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("failed to convert UTF-16 string to UTF-8");
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
    if (written != needed) {
        throw std::runtime_error("UTF-16 to UTF-8 conversion length mismatch");
    }
    return utf8;
}

std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("failed to convert UTF-8 string to UTF-16");
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), wide.data(), needed);
    if (written != needed) {
        throw std::runtime_error("UTF-8 to UTF-16 conversion length mismatch");
    }
    return wide;
}

bool arg_needs_quotes(std::wstring_view arg) {
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

void append_escaped_arg(std::wstring& out, std::wstring_view arg) {
    if (!arg_needs_quotes(arg)) {
        out.append(arg);
        return;
    }
    out.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * kBackslashEscapeFactor + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(ch);
    }
    out.append(backslashes * kBackslashEscapeFactor, L'\\');
    out.push_back(L'"');
}

std::optional<int> finalize_parsed_options(const po::parsed_options& parsed,
                                           const po::options_description& desc,
                                           po::variables_map& vm) {
    const auto stray =
        po::collect_unrecognized(parsed.options, po::include_positional);
    if (!stray.empty()) {
        std::cerr << "[ERROR] unexpected argument '" << stray.front() << "'\n\n"
                  << desc << std::endl;
        return kExitError;
    }

    po::store(parsed, vm);
    if (vm.count("help") != 0) {
        std::cout << desc << std::endl;
        return kExitHelp;
    }
    po::notify(vm);
    return std::nullopt;
}

}  // namespace

// GetModuleFileNameW signals truncation by filling the buffer completely.
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

    // Staged release or install\<game> layout (`bin` with sibling `game` or
    // `script_output`).  Checked in a second pass so a binary inside
    // `<checkout>\build\install\bin` resolves to the outer checkout rather
    // than `<checkout>\build\install`.
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
    if (const std::string found = default_project_dir(); found.empty()) {
        dir->required();
    } else {
        dir->default_value(found);
    }

    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("dir", dir, "FRATERNITE_HD project folder")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

po::options_description game_only_options(std::string& game_dir) {
    po::options_description desc("Options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

std::vector<std::string> process_utf8_args() {
    const LPCWSTR cmd_line = GetCommandLineW();
    if (cmd_line == nullptr || cmd_line[0] == L'\0') {
        return {};
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmd_line, &argc);
    const ScopedLocalArgv guard(argv);
    if (argv == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed");
    }

    std::vector<std::string> args;
    if (argc > kFirstUserArgIndex) {
        args.reserve(static_cast<std::size_t>(argc - kFirstUserArgIndex));
    }
    for (int i = kFirstUserArgIndex; i < argc; ++i) {
        args.push_back(wide_to_utf8(argv[i] != nullptr ? argv[i] : L""));
    }
    return args;
}

std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm) {
    try {
        const std::vector<std::string> args = process_utf8_args();
        const auto parsed = po::command_line_parser(args).options(desc).run();
        return finalize_parsed_options(parsed, desc, vm);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
}

std::optional<int> parse_command_line(int argc, char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm) {
    try {
        const auto parsed =
            po::command_line_parser(argc, argv).options(desc).run();
        return finalize_parsed_options(parsed, desc, vm);
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
}

std::wstring build_process_command_line(std::string_view exe_path,
                                        const std::vector<std::string>& args) {
    if (exe_path.empty()) {
        throw std::runtime_error("executable path cannot be empty");
    }
    if (exe_path.find('"') != std::string_view::npos) {
        throw std::runtime_error("executable path cannot contain double quotes");
    }

    std::wstring cmd;
    cmd.push_back(L'"');
    cmd.append(utf8_to_wide(exe_path));
    cmd.push_back(L'"');

    for (const std::string& arg : args) {
        cmd.push_back(L' ');
        append_escaped_arg(cmd, utf8_to_wide(arg));
    }
    return cmd;
}

int run_process(std::string_view exe_path, const std::vector<std::string>& args) {
    std::wstring cmd_w = build_process_command_line(exe_path, args);
    const std::wstring exe_w = utf8_to_wide(exe_path);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    const BOOL created = CreateProcessW(
        exe_w.c_str(), cmd_w.data(), nullptr, nullptr, FALSE, 0,
        nullptr, nullptr, &si, &pi);
    if (!created) {
        throw std::runtime_error(
            "CreateProcessW failed for '" + std::string(exe_path) +
            "' (Win32 error " + std::to_string(GetLastError()) + ")");
    }

    const ScopedHandle proc_guard(pi.hProcess);
    const ScopedHandle thread_guard(pi.hThread);

    const DWORD wait_rc = WaitForSingleObject(proc_guard.get(), INFINITE);
    if (wait_rc == WAIT_FAILED) {
        throw std::runtime_error("WaitForSingleObject failed for child process");
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(proc_guard.get(), &exit_code)) {
        throw std::runtime_error("GetExitCodeProcess failed for child process");
    }
    return static_cast<int>(exit_code);
}

}  // namespace frat::apps
