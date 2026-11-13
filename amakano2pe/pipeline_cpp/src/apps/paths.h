// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Command-line, UTF-8 path discovery, and child-process execution for the
// pipeline apps (Boost.ProgramOptions + wide Win32 APIs).
//
// Every string across this boundary is UTF-8.  Every app resolves its project
// inputs and outputs from the amakano2pe project folder, found at run time via
// default_project_dir() and overridable with `--dir <path>`, and requires
// `--game-dir <path>` for the game installation.
#pragma once

#include <boost/program_options.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ama::apps {

namespace po = boost::program_options;

inline constexpr int kExitHelp = 0;
inline constexpr int kExitError = 1;

// The folder holding this executable (UTF-8).  Empty when Windows will not say.
std::string exe_dir();

// Resolves the project folder (UTF-8) from `start_dir`: prefers a checkout
// (`pipeline_cpp` and `build.py`) over a staged release or install layout
// (`bin` with sibling `game` or `script_output`).  Returns empty when neither
// layout is found within the bounded ancestor walk.
std::string find_project_dir_from(const std::filesystem::path& start_dir);

// The project folder (UTF-8): the nearest folder at or above this executable
// holding either `pipeline_cpp` and `build.py` (a checkout) or `bin` with
// sibling `game` or `script_output` (a staged release or install\<game>).
// Empty when neither is found, and `--dir` below then becomes required.
//
// Not the working directory, and not a compiled-in path: neither survives the
// tree moving.
std::string default_project_dir();

// Returns the current process's command-line option tokens (excluding argv[0])
// as lossless UTF-8 decoded from GetCommandLineW() + CommandLineToArgvW().
std::vector<std::string> process_utf8_args();

// --dir / --game-dir, bound to the caller's UTF-8 strings.
//
// EVERY app registers both: 00_run_all passes the pair to all of its
// children, and Program_options treats an unregistered option as an error
// rather than ignoring it.
//
// --game-dir has no default: the game install path differs per machine.
po::options_description common_options(std::string& project_dir,
                                       std::string& game_dir);

// Parses `args` (option tokens excluding argv[0]) into `vm`.  Returns an exit
// code when main should stop immediately (0 for --help, 1 for a malformed
// command line), or nullopt to carry on.
std::optional<int> parse_command_line(const std::vector<std::string>& args,
                                      const po::options_description& desc,
                                      po::variables_map& vm);

// Parses narrow `argv` (including argv[0] at index 0) into `vm`.
std::optional<int> parse_command_line(int argc, char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm);

// Parses the current process's wide Win32 command line as UTF-8 into `vm`.
std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm);

// Formats `exe_path` and `args` (all UTF-8) into a single command-line string
// escaped according to Windows CommandLineToArgvW / CreateProcessW rules.
std::string format_windows_command_line(const std::string& exe_path,
                                        const std::vector<std::string>& args);

// Tokenizes `cmdline_utf8` via CommandLineToArgvW (including token 0) and
// returns each token as UTF-8.
std::vector<std::string> split_windows_command_line(const std::string& cmdline_utf8);

// Launches `exe_path` directly via CreateProcessW with `args` (all UTF-8),
// waits for completion, and returns the child's exit code.
int run_process(const std::string& exe_path,
                const std::vector<std::string>& args);

}  // namespace ama::apps
