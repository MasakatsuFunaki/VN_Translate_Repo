// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Where an app is, which project folder it belongs to, and wide UTF-16 -> UTF-8
// command-line parsing.
#include "apps/paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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

namespace ksh::apps {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBufferGrowthFactor = 2;
constexpr int kMaxAncestorLevels = 8;
constexpr int kFirstProgramArgIndex = 1;

struct LocalFreeDeleter {
    void operator()(wchar_t** ptr) const noexcept {
        if (ptr != nullptr) {
            ::LocalFree(ptr);
        }
    }
};

std::string wide_arg_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int byte_len = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
        nullptr, nullptr);
    if (byte_len <= 0) {
        throw std::runtime_error("invalid UTF-16 command-line argument");
    }
    std::string utf8(static_cast<std::size_t>(byte_len), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), utf8.data(),
        byte_len, nullptr, nullptr);
    if (written != byte_len) {
        throw std::runtime_error("UTF-16 to UTF-8 command-line conversion failed");
    }
    return utf8;
}

std::optional<int> parse_arg_tokens(const std::vector<std::string>& args,
                                    const po::options_description& desc,
                                    po::variables_map& vm) {
    try {
        auto parsed = po::command_line_parser(args).options(desc).run();

        // No tool takes a positional argument.  An unknown --option is an
        // error, but Program_options drops a stray bare token silently -- so a
        // typo would run the tool against the wrong tree instead of failing.
        auto stray = po::collect_unrecognized(parsed.options, po::include_positional);
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

// GetModuleFileNameW truncates rather than fails when the buffer is short, and
// reports it only by filling the buffer completely, so the call grows its own
// buffer until the name fits.
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

std::vector<std::string> command_line_to_utf8_args(const wchar_t* wide_cmd_line) {
    if (wide_cmd_line == nullptr || *wide_cmd_line == L'\0') {
        return {};
    }

    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(wide_cmd_line, &argc);
    if (argv == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed");
    }
    const std::unique_ptr<wchar_t*, LocalFreeDeleter> argv_guard(argv);

    if (argc <= kFirstProgramArgIndex) {
        return {};
    }

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc - kFirstProgramArgIndex));
    for (int i = kFirstProgramArgIndex; i < argc; ++i) {
        const std::wstring_view wide_arg =
            (argv[i] != nullptr) ? std::wstring_view(argv[i]) : std::wstring_view{};
        args.push_back(wide_arg_to_utf8(wide_arg));
    }
    return args;
}

std::vector<std::string> process_utf8_args() {
    return command_line_to_utf8_args(::GetCommandLineW());
}

po::options_description common_options(std::string& project_dir) {
    auto* dir = po::value(&project_dir);
    if (const std::string found = default_project_dir(); found.empty()) {
        dir->required();
    } else {
        dir->default_value(found);
    }

    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("dir", dir, "kishin_hisho project folder");
    return desc;
}

std::optional<int> parse_command_line(int argc,
                                      char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm) {
    std::vector<std::string> args;
    if (argc > kFirstProgramArgIndex && argv != nullptr) {
        args.reserve(static_cast<std::size_t>(argc - kFirstProgramArgIndex));
        for (int i = kFirstProgramArgIndex; i < argc; ++i) {
            args.emplace_back(argv[i] != nullptr ? argv[i] : "");
        }
    }
    return parse_arg_tokens(args, desc, vm);
}

std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm) {
    return parse_arg_tokens(process_utf8_args(), desc, vm);
}

}  // namespace ksh::apps
