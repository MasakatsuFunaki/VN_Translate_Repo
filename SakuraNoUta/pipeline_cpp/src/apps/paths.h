// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Command-line handling, UTF-8 path discovery, and wide Win32 child-process
// execution for the pipeline executables (Boost.ProgramOptions + Win32 APIs).
//
// Every string across this boundary is UTF-8.  Executables derive their project
// inputs and outputs from the SakuraNoUta project folder found at run time via
// default_project_dir() and overridable with `--dir <path>`.  Executables that
// also read or write the game installation register `--game-dir <path>` via
// common_options(); project-only executables register project_only_options().
#pragma once

#include <boost/program_options.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snu::apps {

namespace po = boost::program_options;

inline constexpr int kExitOk = 0;
inline constexpr int kExitFailure = 1;
inline constexpr int kExitHelp = kExitOk;
inline constexpr int kExitError = kExitFailure;
inline constexpr int kRefusedExitCode = 2;

// Converts a UTF-8 string to UTF-16, rejecting invalid UTF-8 sequences.
std::wstring utf8_to_wide(std::string_view utf8);

// Converts a UTF-16 string to UTF-8, rejecting invalid UTF-16 sequences.
std::string wide_to_utf8(std::wstring_view wide);

// The folder holding this executable (UTF-8).  Empty when Windows will not say.
std::string exe_dir();

// Resolves the project folder (UTF-8) from `start_dir`: prefers an ancestor
// checkout holding `pipeline_cpp` and `build.py`, falling back to a staged
// distribution folder holding `bin` alongside `game` (or `script_output`).
std::string find_project_dir_from(const std::filesystem::path& start_dir);

// The project folder (UTF-8): the nearest folder at or above this executable
// holding either `pipeline_cpp` and `build.py` (a checkout) or `bin` with
// sibling `game` or `script_output` (a staged distribution).  Empty when
// neither is found, and `--dir` then becomes required.
std::string default_project_dir();

// Returns the current process's command-line option tokens (excluding argv[0])
// as lossless UTF-8 decoded from GetCommandLineW() + CommandLineToArgvW().
std::vector<std::string> process_utf8_args();

// `--help` and `--dir` only, for executables that operate solely on files
// inside the project folder and never accept `--game-dir`.
po::options_description project_only_options(std::string& project_dir);

// `--help`, `--dir`, and `--game-dir`, bound to the caller's UTF-8 strings.
po::options_description common_options(std::string& project_dir,
                                       std::string& game_dir);

// Parses `args` (option tokens excluding argv[0]) into `vm`.  Returns kExitOk
// for `--help`, kExitFailure on error, or nullopt to carry on.
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

// Project-only CLI parsers enforcing that `--game-dir` is not registered or accepted.
std::optional<int> parse_project_only_command_line(
    const std::vector<std::string>& args,
    const po::options_description& desc,
    po::variables_map& vm);

std::optional<int> parse_project_only_command_line(
    int argc, char** argv,
    const po::options_description& desc,
    po::variables_map& vm);

std::optional<int> parse_process_project_only_command_line(
    const po::options_description& desc,
    po::variables_map& vm);

// Formats `exe_path` and `args` (all UTF-8) into a single command-line string
// escaped according to Windows CommandLineToArgvW / CreateProcessW rules.
std::string format_windows_command_line(const std::string& exe_path,
                                        const std::vector<std::string>& args);

// Tokenizes `cmdline_utf8` via CommandLineToArgvW (including token 0) and
// returns each token as UTF-8.
std::vector<std::string> split_windows_command_line(const std::string& cmdline_utf8);

// Launches `exe_path` directly via CreateProcessW with `args` and optional
// `working_dir` (all UTF-8), waits for completion, and returns the exit code.
int run_process(const std::string& exe_path,
                const std::vector<std::string>& args,
                const std::string& working_dir = {});

}  // namespace snu::apps
