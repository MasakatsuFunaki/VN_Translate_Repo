// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Where an app is, which project folder it belongs to, lossless UTF-8 CLI
// decoding, and wide Win32 child-process execution.
//
// Split out of paths.h so that <windows.h> and <shellapi.h> are not dragged
// into every app main.
#include "apps/paths.h"

#include "common/util.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ama::apps {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBufferGrowthFactor = 2;
constexpr int kMaxAncestorLevels = 8;
constexpr int kAllTokensStartIndex = 0;
constexpr int kFirstOptionArgIndex = 1;
constexpr std::size_t kEscapedBackslashMultiplier = 2;

struct LocalFreeDeleter {
    void operator()(wchar_t** ptr) const noexcept {
        if (ptr != nullptr) {
            LocalFree(ptr);
        }
    }
};

using ScopedArgvW = std::unique_ptr<wchar_t*, LocalFreeDeleter>;

class ScopedProcessHandles {
public:
    explicit ScopedProcessHandles(const PROCESS_INFORMATION& pi) noexcept
        : process_(pi.hProcess), thread_(pi.hThread) {}

    ScopedProcessHandles(const ScopedProcessHandles&) = delete;
    ScopedProcessHandles& operator=(const ScopedProcessHandles&) = delete;

    ~ScopedProcessHandles() {
        if (thread_ != nullptr && thread_ != INVALID_HANDLE_VALUE) {
            CloseHandle(thread_);
        }
        if (process_ != nullptr && process_ != INVALID_HANDLE_VALUE) {
            CloseHandle(process_);
        }
    }

    HANDLE process() const noexcept { return process_; }

private:
    HANDLE process_;
    HANDLE thread_;
};

std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()),
        nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        throw std::runtime_error("invalid UTF-16 string");
    }
    std::string out(static_cast<std::size_t>(len), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()),
        out.data(), len, nullptr, nullptr);
    if (written != len) {
        throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    }
    return out;
}

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
        nullptr, 0);
    if (len <= 0) {
        throw std::runtime_error("invalid UTF-8 string");
    }
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
        out.data(), len);
    if (written != len) {
        throw std::runtime_error("UTF-8 to UTF-16 conversion failed");
    }
    return out;
}

std::vector<std::string> tokenize_wide_command_line(const wchar_t* cmdline,
                                                    int start_index) {
    if (cmdline == nullptr || *cmdline == L'\0') {
        return {};
    }
    int argc = 0;
    const ScopedArgvW argv(CommandLineToArgvW(cmdline, &argc));
    if (!argv || argc <= 0) {
        throw std::runtime_error("CommandLineToArgvW failed");
    }
    if (argc <= start_index) {
        return {};
    }
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(argc - start_index));
    for (int i = start_index; i < argc; ++i) {
        out.push_back(wide_to_utf8(argv.get()[i]));
    }
    return out;
}

std::string quote_windows_exe(const std::string& exe_path) {
    if (exe_path.find('"') != std::string::npos) {
        throw std::runtime_error("executable path cannot contain double quotes");
    }
    (void)utf8_to_wide(exe_path);
    return "\"" + exe_path + "\"";
}

std::wstring quote_windows_arg(std::wstring_view arg) {
    if (!arg.empty() &&
        arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }

    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t pending_backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++pending_backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(pending_backslashes * kEscapedBackslashMultiplier + 1, L'\\');
            quoted.push_back(L'"');
            pending_backslashes = 0;
            continue;
        }
        quoted.append(pending_backslashes, L'\\');
        quoted.push_back(ch);
        pending_backslashes = 0;
    }
    quoted.append(pending_backslashes * kEscapedBackslashMultiplier, L'\\');
    quoted.push_back(L'"');
    return quoted;
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
    return fs::path(buf).parent_path().u8string();
}

// std::filesystem hands back native separators, so the derived paths and
// the child command lines the apps echo come out Windows-facing with no
// fixup.
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

std::vector<std::string> process_utf8_args() {
    return tokenize_wide_command_line(GetCommandLineW(), kFirstOptionArgIndex);
}

po::options_description common_options(std::string& project_dir,
                                       std::string& game_dir) {
    // Found: it is the default.  Not found -- this executable is not in the
    // project tree -- and --dir becomes required, so the run stops with a
    // message instead of deriving every path from a folder that merely looked
    // plausible.
    auto* dir = po::value(&project_dir);
    if (const std::string found = default_project_dir(); found.empty()) {
        dir->required();
    } else {
        dir->default_value(found);
    }

    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("dir", dir, "amakano2pe project folder")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

std::optional<int> parse_command_line(const std::vector<std::string>& args,
                                      const po::options_description& desc,
                                      po::variables_map& vm) {
    try {
        auto parsed = po::command_line_parser(args).options(desc).run();

        // No app takes a positional argument.  An unknown --option is an
        // error, but Program_options drops a stray bare token silently -- so a
        // typo would launch a full pipeline run instead of failing.
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
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
    return std::nullopt;
}

std::optional<int> parse_command_line(int argc, char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm) {
    std::vector<std::string> args;
    if (argc > kFirstOptionArgIndex && argv != nullptr) {
        args.reserve(static_cast<std::size_t>(argc - kFirstOptionArgIndex));
        for (int i = kFirstOptionArgIndex; i < argc; ++i) {
            args.emplace_back(argv[i] != nullptr ? argv[i] : "");
        }
    }
    return parse_command_line(args, desc, vm);
}

std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm) {
    return parse_command_line(process_utf8_args(), desc, vm);
}

std::string format_windows_command_line(const std::string& exe_path,
                                        const std::vector<std::string>& args) {
    std::string cmd = quote_windows_exe(exe_path);
    for (const auto& arg : args) {
        cmd.push_back(' ');
        cmd += wide_to_utf8(quote_windows_arg(utf8_to_wide(arg)));
    }
    return cmd;
}

std::vector<std::string> split_windows_command_line(const std::string& cmdline_utf8) {
    const std::wstring wide = utf8_to_wide(cmdline_utf8);
    return tokenize_wide_command_line(wide.c_str(), kAllTokensStartIndex);
}

int run_process(const std::string& exe_path,
                const std::vector<std::string>& args) {
    const std::wstring exe_w = utf8_to_wide(exe_path);
    const std::string cmd_utf8 = format_windows_command_line(exe_path, args);
    std::wstring cmd_w = utf8_to_wide(cmd_utf8);
    print_line("Running: " + cmd_utf8);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_w.c_str(), cmd_w.data(), nullptr, nullptr, TRUE, 0,
                        nullptr, nullptr, &si, &pi)) {
        const DWORD err = GetLastError();
        throw std::runtime_error("cannot launch " + exe_path +
                                 " (Win32 error " + std::to_string(err) + ")");
    }

    const ScopedProcessHandles handles(pi);

    if (WaitForSingleObject(handles.process(), INFINITE) == WAIT_FAILED) {
        const DWORD err = GetLastError();
        throw std::runtime_error("waiting for " + exe_path +
                                 " failed (Win32 error " + std::to_string(err) + ")");
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(handles.process(), &exit_code)) {
        const DWORD err = GetLastError();
        throw std::runtime_error("reading exit code of " + exe_path +
                                 " failed (Win32 error " + std::to_string(err) + ")");
    }
    return static_cast<int>(exit_code);
}

}  // namespace ama::apps
