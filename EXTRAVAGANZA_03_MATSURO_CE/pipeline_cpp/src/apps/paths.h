// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Command-line and child-process handling for the pipeline apps.
//
// Pipeline tools resolve their project inputs and outputs from the
// EXTRAVAGANZA_MATSURO_CE project folder found at run time (see
// default_project_dir) and overridable with `--dir <path>`, while
// `--game-dir <path>` points at the game installation.  Arguments are read
// from the Windows wide command line in UTF-16 and converted strictly to UTF-8.
#pragma once

#include <boost/program_options.hpp>

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exm::apps {

namespace po = boost::program_options;

inline constexpr int kExitOk = 0;
inline constexpr int kExitError = 1;
inline constexpr int kExitRefused = 2;

// The folder holding this executable as UTF-8.  Empty when Windows will not say.
std::string exe_dir();

// Walks up from `start_dir` to locate the project folder as UTF-8: prefers a
// repository checkout (`pipeline_cpp` + `build.py`), falling back to a staged
// layout (`bin` alongside `game` or `script_output`).  Returns empty when
// neither layout is found within the ancestor search bound.
std::string find_project_dir_from(const std::filesystem::path& start_dir);

// Resolves the project folder from exe_dir() via find_project_dir_from().
// Empty when no checkout or staged layout is found, making `--dir` required.
std::string default_project_dir();

// Reads a wide Windows environment variable and returns its UTF-8 value when
// set and non-empty.
std::optional<std::string> wide_env_utf8(const wchar_t* name);

// Splits a wide Windows command line via CommandLineToArgvW and converts every
// token (including argv[0]) to UTF-8.
std::vector<std::string> split_windows_command_line_utf8(std::wstring_view cmd_line);

// Returns the current process's UTF-8 arguments (argv[1..N], skipping argv[0])
// decoded directly from GetCommandLineW() + CommandLineToArgvW().
std::vector<std::string> process_utf8_args();

// Formats an executable path and UTF-8 argument vector into a mutable-ready
// Windows wide command line following CommandLineToArgvW quoting rules:
// argv[0] is wrapped in quotes without doubling trailing backslashes (rejecting
// embedded quotes), while argv[1..N] apply 2n / 2n+1 backslash escaping.
std::wstring build_windows_command_line(std::string_view exe_path,
                                        const std::vector<std::string>& args);

// Spawns a child executable via CreateProcessW with UTF-16 arguments, waits for
// completion, and returns the child's exit code.
int run_child_process(std::string_view exe_path, const std::vector<std::string>& args);

// `--dir` and `--game-dir`, bound to the caller's strings.
inline po::options_description common_options(std::string& project_dir,
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
        ("dir", dir, "EXTRAVAGANZA_MATSURO_CE project folder")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

// `--game-dir` only, for standalone tools that operate solely on the game
// installation.
inline po::options_description game_only_options(std::string& game_dir) {
    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

// Parses an argv[0]-stripped UTF-8 argument vector into `vm`.
inline std::optional<int> parse_command_line_args(
    const std::vector<std::string>& args,
    const po::options_description& desc,
    po::variables_map& vm) {
    try {
        auto parsed = po::command_line_parser(args).options(desc).run();
        auto stray = po::collect_unrecognized(parsed.options, po::include_positional);
        if (!stray.empty()) {
            std::cerr << "[ERROR] unexpected argument '" << stray.front() << "'\n\n"
                      << desc << std::endl;
            return kExitError;
        }

        po::store(parsed, vm);
        if (vm.count("help") != 0) {
            std::cout << desc << std::endl;
            return kExitOk;
        }
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
    return std::nullopt;
}

// Parses the current process's wide Windows command line as UTF-8 into `vm`.
inline std::optional<int> parse_process_command_line(
    const po::options_description& desc,
    po::variables_map& vm) {
    try {
        return parse_command_line_args(process_utf8_args(), desc, vm);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
}

// Parses synthetic narrow `argv` into `vm` (retained for narrow unit tests).
inline std::optional<int> parse_command_line(int argc, char** argv,
                                             const po::options_description& desc,
                                             po::variables_map& vm) {
    try {
        auto parsed = po::command_line_parser(argc, argv).options(desc).run();
        auto stray = po::collect_unrecognized(parsed.options, po::include_positional);
        if (!stray.empty()) {
            std::cerr << "[ERROR] unexpected argument '" << stray.front() << "'\n\n"
                      << desc << std::endl;
            return kExitError;
        }

        po::store(parsed, vm);
        if (vm.count("help") != 0) {
            std::cout << desc << std::endl;
            return kExitOk;
        }
        po::notify(vm);
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
    return std::nullopt;
}

}  // namespace exm::apps
