// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Command-line and child-process handling for the pipeline apps.
//
// Pipeline tools derive their project inputs and outputs from the shingakkou
// project folder found at run time (see default_project_dir) and overridable
// with `--dir <path>`, while `--game-dir <path>` locates the game
// installation.  Standalone tools that operate solely on the game installation
// register `--game-dir` via game_only_options.
#pragma once

#include <boost/program_options.hpp>

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shin::apps {

namespace po = boost::program_options;

inline constexpr int kExitHelp = 0;
inline constexpr int kExitError = 1;
inline constexpr int kRefusedExitCode = 2;

// The folder holding this executable, encoded as UTF-8.  Empty when Windows
// will not say.
std::string exe_dir();

// Resolves the UTF-8 project folder from `start_dir`: prefers an ancestor
// checkout holding `pipeline_cpp` and `build.py`, falling back to a staged
// distribution folder holding `bin` alongside `game` (or `script_output`).
std::string find_project_dir_from(const std::filesystem::path& start_dir);

// The project folder: the nearest folder at or above this executable holding
// either `pipeline_cpp` and `build.py` (a checkout) or `bin` with sibling
// `game` (or `script_output`) (a staged install\<game>).  Empty when neither is
// found, and `--dir` below then becomes required.
//
// Not the working directory, and not a compiled-in path: neither survives the
// tree moving.
std::string default_project_dir();

// Returns the current process's command-line arguments (`argv[1..N]`) decoded
// directly from `GetCommandLineW()` + `CommandLineToArgvW` into UTF-8.
std::vector<std::string> process_utf8_args();

// Formats a Windows wide command line suitable for `CreateProcessW` /
// `CommandLineToArgvW` from UTF-8 `exe_path` and `args`.
std::wstring build_process_command_line(std::string_view exe_path,
                                        const std::vector<std::string>& args);

// Spawns `exe_path` via `CreateProcessW` with UTF-8 `args`, waits for it to
// finish, and returns its exit code.
int run_process(std::string_view exe_path, const std::vector<std::string>& args);

// `--help` and `--game-dir` only, for standalone tools that operate on the
// game installation and locate bundled patch data automatically.
inline po::options_description game_only_options(std::string& game_dir) {
    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

// `--dir` and `--game-dir`, bound to the caller's strings for pipeline tools
// that read or write both trees.
//
// `--game-dir` has no default: the game install path differs per machine.
inline po::options_description common_options(std::string& project_dir,
                                              std::string& game_dir) {
    // Found: it is the default.  Not found -- this executable is not in the
    // project tree -- and --dir becomes required, so the run stops with a
    // message instead of deriving every path from a folder that merely looked
    // plausible.
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
        ("dir", dir, "shingakkou project folder")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

namespace detail {

inline std::optional<int> finish_parsed_command_line(
    const po::parsed_options& parsed,
    const po::options_description& desc,
    po::variables_map& vm) {
    // No app takes a positional argument.  An unknown --option is an error,
    // but Program_options drops a stray bare token silently -- so a typo would
    // launch a full pipeline run instead of failing.
    const auto stray = po::collect_unrecognized(parsed.options, po::include_positional);
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

}  // namespace detail

// Parses synthetic narrow `argv` into `vm`.  Returns an exit code when the
// caller should stop immediately (0 for --help, 1 for a malformed command
// line), or nullopt to carry on.
inline std::optional<int> parse_command_line(int argc, char** argv,
                                             const po::options_description& desc,
                                             po::variables_map& vm) {
    try {
        const auto parsed = po::command_line_parser(argc, argv).options(desc).run();
        return detail::finish_parsed_command_line(parsed, desc, vm);
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
}

// Parses the current process's UTF-16 Windows command line (decoded to UTF-8)
// into `vm`.
inline std::optional<int> parse_process_command_line(
    const po::options_description& desc,
    po::variables_map& vm) {
    try {
        const std::vector<std::string> args = process_utf8_args();
        const auto parsed = po::command_line_parser(args).options(desc).run();
        return detail::finish_parsed_command_line(parsed, desc, vm);
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return kExitError;
    }
}

}  // namespace shin::apps
