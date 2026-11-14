// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "apps/paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace adi::apps {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBufferGrowthFactor = 2;
constexpr int kMaxAncestorLevels = 8;
constexpr int kFirstArgIndex = 1;
constexpr std::size_t kBackslashDoublingFactor = 2;

class ScopedLocalArgv {
public:
    explicit ScopedLocalArgv(wchar_t** argv) noexcept : argv_(argv) {}
    ScopedLocalArgv(const ScopedLocalArgv&) = delete;
    ScopedLocalArgv& operator=(const ScopedLocalArgv&) = delete;

    ~ScopedLocalArgv() {
        if (argv_ != nullptr) {
            ::LocalFree(argv_);
        }
    }

    wchar_t** get() const noexcept { return argv_; }

private:
    wchar_t** argv_ = nullptr;
};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
    }

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

std::string wide_to_utf8_strict(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error(
            "WideCharToMultiByte failed converting UTF-16 command-line argument to UTF-8");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        throw std::runtime_error(
            "WideCharToMultiByte produced an unexpected UTF-8 byte count");
    }
    return out;
}

std::wstring utf8_to_wide_strict(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error(
            "MultiByteToWideChar failed converting UTF-8 string to UTF-16");
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
        out.data(), needed);
    if (written != needed) {
        throw std::runtime_error(
            "MultiByteToWideChar produced an unexpected UTF-16 unit count");
    }
    return out;
}

bool needs_windows_arg_quotes(std::wstring_view arg) {
    if (arg.empty()) {
        return true;
    }
    for (wchar_t ch : arg) {
        if (ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\v' ||
            ch == L'\r' || ch == L'"') {
            return true;
        }
    }
    return false;
}

void append_quoted_windows_arg(std::wstring& out, std::wstring_view arg) {
    if (!needs_windows_arg_quotes(arg)) {
        out.append(arg);
        return;
    }

    out.push_back(L'"');
    std::size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * kBackslashDoublingFactor + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(ch);
    }
    out.append(backslashes * kBackslashDoublingFactor, L'\\');
    out.push_back(L'"');
}

std::optional<int> parse_tokens(const std::vector<std::string>& args,
                                const po::options_description& desc,
                                po::variables_map& vm) {
    try {
        auto parsed = po::command_line_parser(args).options(desc).run();

        // No app takes a positional argument.  An unknown --option is an
        // error, whereas Program_options drops a stray bare token unless
        // collected explicitly.
        auto stray = po::collect_unrecognized(parsed.options, po::include_positional);
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

po::options_description project_only_options(std::string& project_dir) {
    auto* dir = po::value(&project_dir);
    if (const std::string found = default_project_dir(); found.empty()) {
        dir->required();
    } else {
        dir->default_value(found);
    }

    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("dir", dir, "Adieu project folder");
    return desc;
}

po::options_description common_options(std::string& project_dir,
                                       std::string& game_dir) {
    po::options_description desc = project_only_options(project_dir);
    desc.add_options()
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder (GameData)");
    return desc;
}

std::vector<std::string> process_utf8_args() {
    const wchar_t* raw_cmd = ::GetCommandLineW();
    if (raw_cmd == nullptr || *raw_cmd == L'\0') {
        return {};
    }

    int argc = 0;
    const ScopedLocalArgv argv_w(::CommandLineToArgvW(raw_cmd, &argc));
    if (argv_w.get() == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed to parse process command line");
    }

    std::vector<std::string> out;
    if (argc > kFirstArgIndex) {
        out.reserve(static_cast<std::size_t>(argc - kFirstArgIndex));
    }
    for (int i = kFirstArgIndex; i < argc; ++i) {
        out.push_back(wide_to_utf8_strict(argv_w.get()[i]));
    }
    return out;
}

std::optional<int> parse_command_line(int argc, char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm) {
    std::vector<std::string> tokens;
    if (argv != nullptr && argc > kFirstArgIndex) {
        tokens.reserve(static_cast<std::size_t>(argc - kFirstArgIndex));
        for (int i = kFirstArgIndex; i < argc; ++i) {
            if (argv[i] != nullptr) {
                tokens.emplace_back(argv[i]);
            }
        }
    }
    return parse_tokens(tokens, desc, vm);
}

std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm) {
    return parse_tokens(process_utf8_args(), desc, vm);
}

std::wstring build_command_line_utf16(std::string_view exe_path,
                                      const std::vector<std::string>& args) {
    if (exe_path.empty()) {
        throw std::invalid_argument("executable path must not be empty");
    }
    const std::wstring wide_exe = utf8_to_wide_strict(exe_path);
    if (wide_exe.find(L'"') != std::wstring::npos) {
        throw std::invalid_argument("executable path must not contain double quotes");
    }

    // CommandLineToArgvW parses a quoted argv[0] verbatim up to the closing
    // quote with no backslash-escape interpretation, so trailing backslashes in
    // argv[0] must NOT be doubled.
    std::wstring cmd;
    cmd.push_back(L'"');
    cmd.append(wide_exe);
    cmd.push_back(L'"');

    for (const auto& arg : args) {
        const std::wstring wide_arg = utf8_to_wide_strict(arg);
        cmd.push_back(L' ');
        append_quoted_windows_arg(cmd, wide_arg);
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

    const BOOL ok = ::CreateProcessW(
        wide_exe.c_str(), cmd_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
        nullptr, &si, &pi);
    if (!ok) {
        throw std::runtime_error(
            "CreateProcessW failed for '" + std::string(exe_path) +
            "' (error " + std::to_string(::GetLastError()) + ")");
    }

    const ScopedHandle proc_guard(pi.hProcess);
    const ScopedHandle thread_guard(pi.hThread);

    const DWORD wait_rc = ::WaitForSingleObject(proc_guard.get(), INFINITE);
    if (wait_rc != WAIT_OBJECT_0) {
        throw std::runtime_error(
            "WaitForSingleObject failed for '" + std::string(exe_path) +
            "' (error " + std::to_string(::GetLastError()) + ")");
    }

    DWORD exit_code = 0;
    if (!::GetExitCodeProcess(proc_guard.get(), &exit_code)) {
        throw std::runtime_error(
            "GetExitCodeProcess failed for '" + std::string(exe_path) +
            "' (error " + std::to_string(::GetLastError()) + ")");
    }
    return static_cast<int>(exit_code);
}

}  // namespace adi::apps
