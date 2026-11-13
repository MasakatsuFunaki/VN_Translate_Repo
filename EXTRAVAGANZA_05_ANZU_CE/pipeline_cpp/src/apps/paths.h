// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Command-line and child-process handling for the WelcomeToAnzU_CE pipeline apps.
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
inline constexpr int kExitHelp = 0;
inline constexpr int kExitError = 1;
inline constexpr int kExitRefused = 2;

std::string exe_dir();
std::string default_project_dir();

// Resolves the project root as UTF-8 by walking ancestor directories from
// start_dir: prefers a developer checkout (pipeline_cpp + build.py) before
// falling back to a staged release layout (bin + game, or legacy bin +
// script_output).
std::string find_project_dir_from(const std::filesystem::path& start_dir);

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
        ("dir", dir, "EXTRAVAGANZA_05_ANZU_CE project folder")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

inline po::options_description game_only_options(std::string& game_dir) {
    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

inline std::optional<int> parse_command_line_args(
    const std::vector<std::string>& args,
    const po::options_description& desc,
    po::variables_map& vm) {
    try {
        const auto parsed = po::command_line_parser(args).options(desc).run();
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
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
    return std::nullopt;
}

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

inline std::optional<int> parse_command_line(int argc, char** argv,
                                             const po::options_description& desc,
                                             po::variables_map& vm) {
    try {
        const auto parsed = po::command_line_parser(argc, argv).options(desc).run();
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

}  // namespace exm::apps
