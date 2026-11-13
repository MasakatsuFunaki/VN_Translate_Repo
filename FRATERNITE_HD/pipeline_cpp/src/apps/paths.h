// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Command-line handling and UTF-8 process spawning for the pipeline apps.
#pragma once

#include <boost/program_options.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frat::apps {

namespace po = boost::program_options;

inline constexpr int kExitHelp = 0;
inline constexpr int kExitError = 1;

std::string exe_dir();

// Resolves the project folder from `start_dir`: prefers a checkout
// (`pipeline_cpp` and `build.py`) over a staged release or install layout
// (`bin` with sibling `game` or `script_output`).  Returns UTF-8, or empty when
// neither layout is found within the bounded ancestor walk.
std::string find_project_dir_from(const std::filesystem::path& start_dir);

// Walks up from exe_dir() looking for a checkout (`pipeline_cpp` and
// `build.py`) or a staged release/install layout (`bin` with sibling `game` or
// `script_output`).  Empty when nothing matches — --dir then becomes required.
std::string default_project_dir();

// Common options for pipeline apps invoked by 00_run_all (--dir and --game-dir).
po::options_description common_options(std::string& project_dir,
                                       std::string& game_dir);

// Options for standalone tools that operate solely on --game-dir and resolve
// shipped assets relative to exe_dir() / default_project_dir().
po::options_description game_only_options(std::string& game_dir);

// Reads GetCommandLineW(), splits via CommandLineToArgvW, skips argv[0], and
// returns argv[1..N] encoded as strict UTF-8.
std::vector<std::string> process_utf8_args();

// Parses the current process's UTF-16 command line as UTF-8.
// Returns an exit code (0 = --help, 1 = error) or nullopt to proceed.
std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm);

// Parses synthetic narrow argv (retained for unit tests).
// Returns an exit code (0 = --help, 1 = error) or nullopt to proceed.
std::optional<int> parse_command_line(int argc, char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm);

// Builds a CreateProcessW command-line string compatible with CommandLineToArgvW:
// wraps argv[0] (`exe_path`) in quotes without trailing-backslash doubling, and
// applies 2n / 2n+1 backslash escaping on argv[1..N].
std::wstring build_process_command_line(std::string_view exe_path,
                                        const std::vector<std::string>& args);

// Spawns `exe_path` via CreateProcessW and returns its process exit code.
int run_process(std::string_view exe_path, const std::vector<std::string>& args);

}  // namespace frat::apps
