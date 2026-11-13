// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Where an app is, which project folder it belongs to, and wide Win32 CLI /
// child-process boundaries.
//
// Kept in paths.cpp so that <windows.h> and <shellapi.h> are insulated from
// every app translation unit.
#include "apps/paths.h"

#include "common/util.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fav::apps {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kBufferGrowthFactor = 2;
constexpr int kMaxAncestorLevels = 8;
constexpr std::size_t kFirstOptionArgIndex = 1;
constexpr std::size_t kSurroundingQuoteCount = 2;
constexpr std::size_t kEscapedBackslashMultiplier = 2;
constexpr std::size_t kLiteralQuoteEscapeBackslashOffset = 1;

bool is_valid_handle(HANDLE h) noexcept {
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int size_needed = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size_needed <= 0) {
        throw std::runtime_error("Invalid UTF-16 sequence in command-line argument");
    }
    std::string utf8(static_cast<std::size_t>(size_needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        static_cast<int>(wide.size()),
        utf8.data(),
        size_needed,
        nullptr,
        nullptr);
    if (written != size_needed) {
        throw std::runtime_error("UTF-16 to UTF-8 command-line conversion failed");
    }
    return utf8;
}

std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size_needed = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8.data(),
        static_cast<int>(utf8.size()),
        nullptr,
        0);
    if (size_needed <= 0) {
        throw std::runtime_error("Invalid UTF-8 sequence in command-line string");
    }
    std::wstring wide(static_cast<std::size_t>(size_needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8.data(),
        static_cast<int>(utf8.size()),
        wide.data(),
        size_needed);
    if (written != size_needed) {
        throw std::runtime_error("UTF-8 to UTF-16 command-line conversion failed");
    }
    return wide;
}

class ScopedCommandLineArgv {
public:
    explicit ScopedCommandLineArgv(const wchar_t* cmd_line) {
        if (cmd_line == nullptr || *cmd_line == L'\0') {
            return;
        }
        d_argv = CommandLineToArgvW(cmd_line, &d_argc);
        if (d_argv == nullptr) {
            throw std::runtime_error(
                "CommandLineToArgvW failed on non-empty command line");
        }
    }

    ScopedCommandLineArgv(const ScopedCommandLineArgv&) = delete;
    ScopedCommandLineArgv& operator=(const ScopedCommandLineArgv&) = delete;

    ~ScopedCommandLineArgv() {
        if (d_argv != nullptr) {
            LocalFree(d_argv);
        }
    }

    int count() const noexcept { return d_argc; }
    const wchar_t* at(int index) const noexcept { return d_argv[index]; }

private:
    LPWSTR* d_argv = nullptr;
    int d_argc = 0;
};

class ScopedProcessHandles {
public:
    ScopedProcessHandles() = default;
    ScopedProcessHandles(const ScopedProcessHandles&) = delete;
    ScopedProcessHandles& operator=(const ScopedProcessHandles&) = delete;

    ~ScopedProcessHandles() {
        if (is_valid_handle(d_info.hThread)) {
            CloseHandle(d_info.hThread);
        }
        if (is_valid_handle(d_info.hProcess)) {
            CloseHandle(d_info.hProcess);
        }
    }

    PROCESS_INFORMATION* get() noexcept { return &d_info; }
    HANDLE process() const noexcept { return d_info.hProcess; }

private:
    PROCESS_INFORMATION d_info{};
};

std::vector<std::string> split_wide_command_line(const wchar_t* cmd_line) {
    if (cmd_line == nullptr || *cmd_line == L'\0') {
        return {};
    }
    const ScopedCommandLineArgv wide_args(cmd_line);
    std::vector<std::string> utf8_args;
    utf8_args.reserve(static_cast<std::size_t>(wide_args.count()));
    for (int i = 0; i < wide_args.count(); ++i) {
        utf8_args.push_back(wide_to_utf8(wide_args.at(i)));
    }
    return utf8_args;
}

std::optional<int> finish_parse(const po::parsed_options& parsed,
                                const po::options_description& desc,
                                po::variables_map& vm) {
    // No app takes a positional argument.  An unknown --option is an error,
    // but Program_options drops a stray bare token silently -- so a typo would
    // launch a full pipeline run instead of failing.
    const auto stray =
        po::collect_unrecognized(parsed.options, po::include_positional);
    if (!stray.empty()) {
        std::cerr << "[ERROR] unexpected argument '" << stray.front() << "'\n\n"
                  << desc << std::endl;
        return kExitError;
    }

    po::store(parsed, vm);
    if (vm.count("help") > 0) {
        std::cout << desc << std::endl;
        return kExitHelp;
    }
    po::notify(vm);
    return std::nullopt;
}

bool needs_windows_quotes(const std::string& arg) noexcept {
    if (arg.empty()) {
        return true;
    }
    for (const char ch : arg) {
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '"') {
            return true;
        }
    }
    return false;
}

}  // namespace

// GetModuleFileNameW truncates rather than fails when the buffer is short, and
// reports it only by filling the buffer completely, so the call grows its own
// buffer until the name fits.
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
    return wide_to_utf8(fs::path(buf).parent_path().native());
}

// Two-pass ancestor walk: checks up to kMaxAncestorLevels ancestors for a
// checkout (pipeline_cpp + build.py) before checking staged distributions
// (bin + game or script_output), so binaries under <game>/build/install/bin
// resolve to <game> rather than <game>/build/install.
std::string find_project_dir_from(const std::filesystem::path& start_dir) {
    if (start_dir.empty()) {
        return {};
    }
    std::error_code ec;
    fs::path d = start_dir;
    for (int level = 0; level <= kMaxAncestorLevels && !d.empty(); ++level) {
        if (fs::is_directory(d / "pipeline_cpp", ec) &&
            fs::is_regular_file(d / "build.py", ec)) {
            return wide_to_utf8(d.native());
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
            return wide_to_utf8(d.native());
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
        ("dir", dir, "SakuraMoyu project folder")
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
    return split_wide_command_line(GetCommandLineW());
}

std::optional<int> parse_utf8_args(const std::vector<std::string>& utf8_args,
                                   const po::options_description& desc,
                                   po::variables_map& vm) {
    try {
        const auto parsed =
            po::command_line_parser(utf8_args).options(desc).run();
        return finish_parse(parsed, desc, vm);
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
}

std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm) {
    const std::vector<std::string> args = process_utf8_args();
    if (args.size() <= kFirstOptionArgIndex) {
        return parse_utf8_args({}, desc, vm);
    }
    const std::vector<std::string> option_args(
        args.begin() + static_cast<std::ptrdiff_t>(kFirstOptionArgIndex),
        args.end());
    return parse_utf8_args(option_args, desc, vm);
}

std::optional<int> parse_command_line(int argc, char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm) {
    try {
        const auto parsed =
            po::command_line_parser(argc, argv).options(desc).run();
        return finish_parse(parsed, desc, vm);
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
}

std::string quote_windows_arg(const std::string& arg) {
    if (!needs_windows_quotes(arg)) {
        return arg;
    }

    std::string out;
    out.reserve(arg.size() + kSurroundingQuoteCount);
    out.push_back('"');

    std::size_t backslashes = 0;
    for (const char ch : arg) {
        if (ch == '\\') {
            ++backslashes;
            continue;
        }
        if (ch == '"') {
            out.append(backslashes * kEscapedBackslashMultiplier +
                           kLiteralQuoteEscapeBackslashOffset,
                       '\\');
            out.push_back('"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, '\\');
        out.push_back(ch);
        backslashes = 0;
    }

    out.append(backslashes * kEscapedBackslashMultiplier, '\\');
    out.push_back('"');
    return out;
}

std::string format_windows_command_line(const std::string& exe_path,
                                        const std::vector<std::string>& utf8_args) {
    if (exe_path.empty()) {
        throw std::runtime_error("Executable path must not be empty");
    }
    if (exe_path.find('"') != std::string::npos) {
        throw std::runtime_error("Executable path must not contain double quotes");
    }

    // CommandLineToArgvW scans a quoted argv[0] verbatim up to the closing
    // quote without unescaping \\ or \", so trailing backslashes in exe_path
    // must NOT be doubled.
    std::string cmdline;
    cmdline.reserve(exe_path.size() + kSurroundingQuoteCount);
    cmdline.push_back('"');
    cmdline += exe_path;
    cmdline.push_back('"');

    for (const std::string& arg : utf8_args) {
        cmdline.push_back(' ');
        cmdline += quote_windows_arg(arg);
    }
    return cmdline;
}

std::vector<std::string> split_windows_command_line(const std::string& cmdline) {
    if (cmdline.empty()) {
        return {};
    }
    const std::wstring wide = utf8_to_wide(cmdline);
    return split_wide_command_line(wide.c_str());
}

int run_child_process(const std::filesystem::path& exe_path,
                      const std::vector<std::string>& utf8_args) {
    const std::string exe_utf8 = wide_to_utf8(exe_path.native());
    const std::string cmdline_utf8 = format_windows_command_line(exe_utf8, utf8_args);
    fav::print_line("Running: " + cmdline_utf8);

    const std::wstring app_wide = utf8_to_wide(exe_utf8);
    std::wstring cmd_wide = utf8_to_wide(cmdline_utf8);
    const wchar_t* app_name_ptr =
        exe_path.has_parent_path() ? app_wide.c_str() : nullptr;

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    ScopedProcessHandles proc_handles;

    const BOOL launched = CreateProcessW(
        app_name_ptr,
        cmd_wide.data(),
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        nullptr,
        &startup_info,
        proc_handles.get());
    if (!launched) {
        const DWORD err = GetLastError();
        throw std::runtime_error("CreateProcessW failed for " + exe_utf8 +
                                 " (Win32 error " + std::to_string(err) + ")");
    }

    const DWORD wait_rc = WaitForSingleObject(proc_handles.process(), INFINITE);
    if (wait_rc != WAIT_OBJECT_0) {
        const DWORD err = GetLastError();
        throw std::runtime_error("WaitForSingleObject failed for " + exe_utf8 +
                                 " (Win32 error " + std::to_string(err) + ")");
    }

    DWORD exit_code = static_cast<DWORD>(kExitError);
    if (!GetExitCodeProcess(proc_handles.process(), &exit_code)) {
        const DWORD err = GetLastError();
        throw std::runtime_error("GetExitCodeProcess failed for " + exe_utf8 +
                                 " (Win32 error " + std::to_string(err) + ")");
    }
    return static_cast<int>(exit_code);
}

}  // namespace fav::apps
