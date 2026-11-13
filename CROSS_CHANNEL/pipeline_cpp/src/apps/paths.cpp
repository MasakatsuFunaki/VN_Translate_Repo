// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UTF-8 project-root discovery, wide CLI parsing, and CreateProcessW spawning.
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

namespace crc::apps {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBufferGrowthFactor = 2;
constexpr int kMaxAncestorLevels = 8;
constexpr int kFirstUserArgIndex = 1;
constexpr std::size_t kBackslashPairFactor = 2;
constexpr std::size_t kQuoteEscapeBackslashCount = 1;

class LocalFreeGuard {
public:
    explicit LocalFreeGuard(void* ptr) noexcept : ptr_(ptr) {}
    LocalFreeGuard(const LocalFreeGuard&) = delete;
    LocalFreeGuard& operator=(const LocalFreeGuard&) = delete;

    ~LocalFreeGuard() {
        if (ptr_ != nullptr) {
            ::LocalFree(ptr_);
        }
    }

private:
    void* ptr_;
};

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    ~UniqueHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
    }

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

std::string wide_to_utf8_strict(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    if (wide.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("UTF-16 argument exceeds maximum Win32 length");
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("Invalid UTF-16 sequence in command-line argument");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        throw std::runtime_error("Failed converting UTF-16 argument to UTF-8");
    }
    return out;
}

std::wstring utf8_to_wide_strict(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("UTF-8 string exceeds maximum Win32 length");
    }
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("Invalid UTF-8 sequence in process argument");
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
        out.data(), needed);
    if (written != needed) {
        throw std::runtime_error("Failed converting UTF-8 argument to UTF-16");
    }
    return out;
}

bool arg_needs_quotes(std::wstring_view arg) noexcept {
    if (arg.empty()) {
        return true;
    }
    for (const wchar_t ch : arg) {
        if (ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\v' ||
            ch == L'\r' || ch == L'"') {
            return true;
        }
    }
    return false;
}

void append_escaped_windows_arg(std::wstring& cmd, std::wstring_view arg) {
    if (!arg_needs_quotes(arg)) {
        cmd.append(arg);
        return;
    }

    cmd.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            cmd.append(backslashes * kBackslashPairFactor + kQuoteEscapeBackslashCount, L'\\');
            cmd.push_back(L'"');
            backslashes = 0;
            continue;
        }
        cmd.append(backslashes, L'\\');
        cmd.push_back(ch);
        backslashes = 0;
    }
    cmd.append(backslashes * kBackslashPairFactor, L'\\');
    cmd.push_back(L'"');
}

std::optional<int> parse_utf8_args(const std::vector<std::string>& args,
                                   const po::options_description& desc,
                                   po::variables_map& vm) {
    try {
        const auto parsed = po::command_line_parser(args).options(desc).run();

        // No app takes a positional argument.  An unknown --option is an
        // error, while Boost.ProgramOptions drops a stray bare token unless
        // positional arguments are collected explicitly.
        const auto stray = po::collect_unrecognized(parsed.options, po::include_positional);
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
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
    return std::nullopt;
}

}  // namespace

std::string exe_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n =
            ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
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
        ("dir", dir, "CROSS_CHANNEL project folder")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

std::vector<std::string> process_utf8_args() {
    const wchar_t* raw_cmd = ::GetCommandLineW();
    if (raw_cmd == nullptr || *raw_cmd == L'\0') {
        return {};
    }

    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(raw_cmd, &argc);
    const LocalFreeGuard argv_guard(argv);
    if (argv == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed");
    }
    if (argc <= kFirstUserArgIndex) {
        return {};
    }

    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(argc - kFirstUserArgIndex));
    for (int i = kFirstUserArgIndex; i < argc; ++i) {
        out.push_back(wide_to_utf8_strict(argv[i] != nullptr ? argv[i] : L""));
    }
    return out;
}

std::optional<int> parse_command_line(int argc, char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm) {
    std::vector<std::string> args;
    if (argc > kFirstUserArgIndex && argv != nullptr) {
        args.reserve(static_cast<std::size_t>(argc - kFirstUserArgIndex));
        for (int i = kFirstUserArgIndex; i < argc; ++i) {
            args.emplace_back(argv[i] != nullptr ? argv[i] : "");
        }
    }
    return parse_utf8_args(args, desc, vm);
}

std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm) {
    try {
        return parse_utf8_args(process_utf8_args(), desc, vm);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
}

std::wstring build_command_line_utf16(std::string_view exe_path,
                                      const std::vector<std::string>& args) {
    if (exe_path.empty()) {
        throw std::invalid_argument("Executable path must not be empty");
    }
    if (exe_path.find('"') != std::string_view::npos) {
        throw std::invalid_argument("Executable path must not contain quotes");
    }

    const std::wstring wide_exe = utf8_to_wide_strict(exe_path);
    std::wstring cmd;
    cmd.push_back(L'"');
    cmd.append(wide_exe);
    cmd.push_back(L'"');

    for (const std::string& arg : args) {
        const std::wstring wide_arg = utf8_to_wide_strict(arg);
        cmd.push_back(L' ');
        append_escaped_windows_arg(cmd, wide_arg);
    }
    return cmd;
}

int run_process_utf8(std::string_view exe_path,
                     const std::vector<std::string>& args) {
    std::wstring cmd_line = build_command_line_utf16(exe_path, args);
    const std::wstring wide_exe = utf8_to_wide_strict(exe_path);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!::CreateProcessW(wide_exe.c_str(), cmd_line.data(), nullptr, nullptr,
                          FALSE, 0, nullptr, nullptr, &si, &pi)) {
        const DWORD err = ::GetLastError();
        throw std::runtime_error("CreateProcessW failed with Win32 error " +
                                 std::to_string(err));
    }

    const UniqueHandle proc_guard(pi.hProcess);
    const UniqueHandle thread_guard(pi.hThread);

    if (::WaitForSingleObject(proc_guard.get(), INFINITE) != WAIT_OBJECT_0) {
        const DWORD err = ::GetLastError();
        throw std::runtime_error("WaitForSingleObject failed with Win32 error " +
                                 std::to_string(err));
    }

    DWORD exit_code = 0;
    if (!::GetExitCodeProcess(proc_guard.get(), &exit_code)) {
        const DWORD err = ::GetLastError();
        throw std::runtime_error("GetExitCodeProcess failed with Win32 error " +
                                 std::to_string(err));
    }
    return static_cast<int>(exit_code);
}

}  // namespace crc::apps
