// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Wide UTF-16 -> UTF-8 CLI argument decoding, project directory discovery,
// CommandLineToArgvW-compatible argument quoting, and CreateProcessW execution.
#include "apps/paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace snt::apps {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kBufferGrowthFactor = 2;
constexpr int kMaxAncestorLevels = 8;
constexpr int kFirstArgumentIndex = 1;
constexpr std::size_t kBackslashPairMultiplier = 2;
constexpr std::size_t kEscapedQuoteBackslash = 1;
constexpr unsigned char kMaxControlOrSpaceByte = 0x20U;

class ScopedLocalFree {
public:
    explicit ScopedLocalFree(LPWSTR* ptr) noexcept : ptr_(ptr) {}

    ScopedLocalFree(const ScopedLocalFree&) = delete;
    ScopedLocalFree& operator=(const ScopedLocalFree&) = delete;

    ~ScopedLocalFree() {
        if (ptr_ != nullptr) {
            LocalFree(ptr_);
        }
    }

private:
    LPWSTR* ptr_{nullptr};
};

class ScopedProcessHandles {
public:
    explicit ScopedProcessHandles(const PROCESS_INFORMATION& info) noexcept
        : process_(info.hProcess), thread_(info.hThread) {}

    ScopedProcessHandles(const ScopedProcessHandles&) = delete;
    ScopedProcessHandles& operator=(const ScopedProcessHandles&) = delete;

    ~ScopedProcessHandles() {
        close_handle(thread_);
        close_handle(process_);
    }

    HANDLE process() const noexcept { return process_; }

private:
    static void close_handle(HANDLE h) noexcept {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }

    HANDLE process_{nullptr};
    HANDLE thread_{nullptr};
};

bool arg_needs_quotes(std::string_view arg) noexcept {
    if (arg.empty()) {
        return true;
    }
    for (const char ch : arg) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= kMaxControlOrSpaceByte || ch == '"') {
            return true;
        }
    }
    return false;
}

void append_escaped_arg(std::string& out, std::string_view arg) {
    if (!arg_needs_quotes(arg)) {
        out.append(arg);
        return;
    }

    out.push_back('"');
    std::size_t pending_backslashes = 0;
    for (const char ch : arg) {
        if (ch == '\\') {
            ++pending_backslashes;
            continue;
        }
        if (ch == '"') {
            out.append(
                pending_backslashes * kBackslashPairMultiplier + kEscapedQuoteBackslash,
                '\\');
            out.push_back('"');
            pending_backslashes = 0;
            continue;
        }
        out.append(pending_backslashes, '\\');
        pending_backslashes = 0;
        out.push_back(ch);
    }
    out.append(pending_backslashes * kBackslashPairMultiplier, '\\');
    out.push_back('"');
}

bool is_whitespace_only(std::string_view s) noexcept {
    for (const char ch : s) {
        if (static_cast<unsigned char>(ch) > kMaxControlOrSpaceByte) {
            return false;
        }
    }
    return true;
}

std::optional<int> finish_parsed_options(const po::parsed_options& parsed,
                                         const po::options_description& desc,
                                         po::variables_map& vm) {
    const auto stray =
        po::collect_unrecognized(parsed.options, po::include_positional);
    if (!stray.empty()) {
        std::cerr << "[ERROR] unexpected argument '" << stray.front() << "'\n\n"
                  << desc << std::endl;
        return kExitFailure;
    }

    po::store(parsed, vm);
    if (vm.count("help") > 0) {
        std::cout << desc << std::endl;
        return kExitOk;
    }
    po::notify(vm);
    return std::nullopt;
}

}  // namespace

std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("UTF-8 input exceeds MultiByteToWideChar limit");
    }
    const int src_len = static_cast<int>(utf8.size());
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), src_len, nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("Invalid UTF-8 sequence in utf8_to_wide");
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), src_len, wide.data(), needed);
    if (written != needed) {
        throw std::runtime_error("MultiByteToWideChar conversion failed");
    }
    return wide;
}

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    if (wide.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("UTF-16 input exceeds WideCharToMultiByte limit");
    }
    const int src_len = static_cast<int>(wide.size());
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), src_len,
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("Invalid UTF-16 sequence in wide_to_utf8");
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), src_len,
        utf8.data(), needed, nullptr, nullptr);
    if (written != needed) {
        throw std::runtime_error("WideCharToMultiByte conversion failed");
    }
    return utf8;
}

std::vector<std::string> process_utf8_args() {
    const LPWSTR cmdline = GetCommandLineW();
    if (cmdline == nullptr) {
        throw std::runtime_error("GetCommandLineW returned nullptr");
    }
    int wargc = 0;
    LPWSTR* const wargv = CommandLineToArgvW(cmdline, &wargc);
    if (wargv == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed");
    }
    const ScopedLocalFree guard(wargv);

    std::vector<std::string> args;
    if (wargc <= kFirstArgumentIndex) {
        return args;
    }
    args.reserve(static_cast<std::size_t>(wargc - kFirstArgumentIndex));
    for (int i = kFirstArgumentIndex; i < wargc; ++i) {
        args.push_back(wide_to_utf8(wargv[i]));
    }
    return args;
}

// In CommandLineToArgvW, quoted argv[0] does NOT unescape `\\` or `\"`: every
// character up to the closing `"` is taken verbatim.  Wrap `exe_path` in `"..."`
// without doubling trailing backslashes, and reject `"` in `exe_path`.
std::string format_windows_command_line(std::string_view exe_path,
                                        const std::vector<std::string>& args) {
    if (exe_path.empty()) {
        throw std::runtime_error("Executable path must not be empty");
    }
    if (exe_path.find('"') != std::string::npos) {
        throw std::runtime_error(
            "Executable path must not contain double quotes");
    }

    std::string cmdline;
    cmdline.push_back('"');
    cmdline.append(exe_path);
    cmdline.push_back('"');

    for (const std::string& arg : args) {
        cmdline.push_back(' ');
        append_escaped_arg(cmdline, arg);
    }
    return cmdline;
}

std::vector<std::string> split_windows_command_line(std::string_view cmdline) {
    if (is_whitespace_only(cmdline)) {
        throw std::runtime_error("Command line must not be empty");
    }
    const std::wstring wide = utf8_to_wide(cmdline);
    int wargc = 0;
    LPWSTR* const wargv = CommandLineToArgvW(wide.c_str(), &wargc);
    if (wargv == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed");
    }
    const ScopedLocalFree guard(wargv);

    std::vector<std::string> tokens;
    tokens.reserve(static_cast<std::size_t>(wargc));
    for (int i = 0; i < wargc; ++i) {
        tokens.push_back(wide_to_utf8(wargv[i]));
    }
    return tokens;
}

int run_process(const std::filesystem::path& exe,
                const std::vector<std::string>& args,
                const std::filesystem::path& working_dir) {
    if (exe.empty()) {
        throw std::runtime_error("Executable path must not be empty");
    }
    const std::wstring app_wide = exe.wstring();
    const std::string cmdline_utf8 =
        format_windows_command_line(wide_to_utf8(app_wide), args);
    std::wstring cmdline_wide = utf8_to_wide(cmdline_utf8);

    const std::wstring cwd_wide =
        working_dir.empty() ? std::wstring{} : working_dir.wstring();
    const wchar_t* const app_ptr =
        exe.has_parent_path() ? app_wide.c_str() : nullptr;
    const wchar_t* const cwd_ptr =
        working_dir.empty() ? nullptr : cwd_wide.c_str();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    const BOOL created = CreateProcessW(
        app_ptr, cmdline_wide.data(), nullptr, nullptr, FALSE, 0, nullptr,
        cwd_ptr, &si, &pi);
    if (!created) {
        const DWORD err = GetLastError();
        throw std::runtime_error(
            "CreateProcessW failed for '" + wide_to_utf8(app_wide) +
            "' (Win32 error " + std::to_string(err) + ")");
    }
    const ScopedProcessHandles handles(pi);

    const DWORD wait_rc = WaitForSingleObject(handles.process(), INFINITE);
    if (wait_rc != WAIT_OBJECT_0) {
        const DWORD err = GetLastError();
        throw std::runtime_error(
            "WaitForSingleObject failed (Win32 error " + std::to_string(err) + ")");
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(handles.process(), &exit_code)) {
        const DWORD err = GetLastError();
        throw std::runtime_error(
            "GetExitCodeProcess failed (Win32 error " + std::to_string(err) + ")");
    }
    return static_cast<int>(exit_code);
}

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
    return wide_to_utf8(fs::path(buf).parent_path().wstring());
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

po::options_description project_only_options(std::string& project_dir) {
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
        ("dir", dir, "sakuranotoki project folder");
    return desc;
}

po::options_description common_options(std::string& project_dir,
                                       std::string& game_dir) {
    po::options_description desc = project_only_options(project_dir);
    desc.add_options()
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

std::optional<int> parse_command_line(const std::vector<std::string>& args,
                                      const po::options_description& desc,
                                      po::variables_map& vm) {
    try {
        const auto parsed = po::command_line_parser(args).options(desc).run();
        return finish_parsed_options(parsed, desc, vm);
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitFailure;
    }
}

std::optional<int> parse_command_line(int argc, const char* const* argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm) {
    try {
        const auto parsed =
            po::command_line_parser(argc, argv).options(desc).run();
        return finish_parsed_options(parsed, desc, vm);
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitFailure;
    }
}

std::optional<int> parse_process_command_line(
    const po::options_description& desc,
    po::variables_map& vm) {
    try {
        return parse_command_line(process_utf8_args(), desc, vm);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitFailure;
    }
}

std::optional<int> parse_project_only_command_line(
    const std::vector<std::string>& args,
    const po::options_description& desc,
    po::variables_map& vm) {
    if (desc.find_nothrow("game-dir", false) != nullptr) {
        std::cerr << "[ERROR] project-only CLI must not register --game-dir\n\n"
                  << desc << std::endl;
        return kExitFailure;
    }
    return parse_command_line(args, desc, vm);
}

std::optional<int> parse_project_only_command_line(
    int argc, const char* const* argv,
    const po::options_description& desc,
    po::variables_map& vm) {
    if (desc.find_nothrow("game-dir", false) != nullptr) {
        std::cerr << "[ERROR] project-only CLI must not register --game-dir\n\n"
                  << desc << std::endl;
        return kExitFailure;
    }
    return parse_command_line(argc, argv, desc, vm);
}

std::optional<int> parse_process_project_only_command_line(
    const po::options_description& desc,
    po::variables_map& vm) {
    try {
        return parse_project_only_command_line(process_utf8_args(), desc, vm);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitFailure;
    }
}

}  // namespace snt::apps
