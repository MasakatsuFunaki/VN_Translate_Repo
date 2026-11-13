// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Command-line handling and wide-process execution for the pipeline apps.
//
// Every pipeline path hangs off the SakuraMoyu project folder, which is found at
// run time (see default_project_dir).  `--dir <path>` overrides it;
// `--game-dir <path>` points at the game installation.  All paths returned or
// parsed by this component are UTF-8 encoded for direct use with fs::u8path.
#pragma once

#include <boost/program_options.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fav::apps {

namespace po = boost::program_options;

inline constexpr int kExitOk = 0;
inline constexpr int kExitHelp = 0;
inline constexpr int kExitError = 1;

// The folder holding this executable (UTF-8).  Empty when Windows will not say.
std::string exe_dir();

// Resolves the project folder (UTF-8) from `start_dir`: prefers an ancestor
// checkout holding `pipeline_cpp` and `build.py`, falling back to a staged
// distribution folder holding `bin` alongside `game` (or `script_output`).
std::string find_project_dir_from(const std::filesystem::path& start_dir);

// The project folder (UTF-8): the nearest folder at or above this executable
// holding either `pipeline_cpp` and `build.py` (a checkout) or `bin` with
// sibling `game` (or `script_output`) (a staged install\<game>).  Empty when
// neither is found, and `--dir` below then becomes required.
//
// Not the working directory, and not a compiled-in path: neither survives the
// tree moving.
std::string default_project_dir();

// --dir / --game-dir, bound to the caller's UTF-8 strings.
//
// Pipeline stages that read both project and game inputs register the pair.
// --game-dir has no default: the game install path differs per machine.
po::options_description common_options(std::string& project_dir,
                                       std::string& game_dir);

// --help / --game-dir only, for standalone tools that operate solely on the
// installed game folder and never read project-tree files.
po::options_description game_only_options(std::string& game_dir);

// Returns the current process's command-line tokens (including argv[0]),
// decoded from UTF-16 (GetCommandLineW + CommandLineToArgvW) into UTF-8.
std::vector<std::string> process_utf8_args();

// Parses post-argv[0] UTF-8 tokens into `vm`.  Returns an exit code when main
// should stop immediately (0 for --help, 1 for a malformed command line), or
// nullopt to carry on.
std::optional<int> parse_utf8_args(const std::vector<std::string>& utf8_args,
                                   const po::options_description& desc,
                                   po::variables_map& vm);

// Parses the live Windows wide command line (stripping argv[0]) into `vm`.
std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm);

// Parses synthetic `(argc, argv)` (including argv[0]) into `vm`.
std::optional<int> parse_command_line(int argc, char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm);

// Quotes a single post-argv[0] UTF-8 command-line token according to Windows
// CommandLineToArgvW 2n/2n+1 backslash-escaping rules.
std::string quote_windows_arg(const std::string& arg);

// Formats a complete Windows command line: quotes `exe_path` (argv[0]) in
// `"..."` verbatim without trailing-backslash doubling (rejecting embedded `"`
// or empty `exe_path` with std::runtime_error) and formats `utf8_args` with
// quote_windows_arg.
std::string format_windows_command_line(const std::string& exe_path,
                                        const std::vector<std::string>& utf8_args);

// Splits a Windows command-line string into UTF-8 argv tokens via
// CommandLineToArgvW.  Throws std::runtime_error if UTF-8/UTF-16 conversion or
// CommandLineToArgvW fails on non-empty input.
std::vector<std::string> split_windows_command_line(const std::string& cmdline);

// Spawns `exe_path` via CreateProcessW with `utf8_args`, waits for completion,
// and returns the child process exit code.  Throws std::runtime_error if
// launching or waiting for the child fails.
int run_child_process(const std::filesystem::path& exe_path,
                      const std::vector<std::string>& utf8_args);

}  // namespace fav::apps
