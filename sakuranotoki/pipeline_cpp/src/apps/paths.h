// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Wide UTF-16 -> UTF-8 command-line handling, project root discovery, and
// child process execution for the pipeline apps.
//
// Pipeline executables derive their project inputs and outputs from the
// sakuranotoki project folder found at run time (see default_project_dir) and
// overridable with `--dir <path>`.  Executables that also read or write the
// game installation register `--game-dir <path>` via common_options;
// project-only executables register project_only_options.
#pragma once

#include <boost/program_options.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snt::apps {

namespace po = boost::program_options;

inline constexpr int kExitOk = 0;
inline constexpr int kExitFailure = 1;
inline constexpr int kExitHelp = kExitOk;
inline constexpr int kExitError = kExitFailure;
inline constexpr int kRefusedExitCode = 2;

// The folder holding this executable, encoded as UTF-8.  Empty on failure.
std::string exe_dir();

// Resolves the project folder from `start_dir`: prefers an ancestor checkout
// holding `pipeline_cpp` and `build.py`, falling back to a staged distribution
// folder holding `bin` alongside `game` (or `script_output`).
std::string find_project_dir_from(const std::filesystem::path& start_dir);

// The project folder: the nearest folder at or above this executable holding
// either `pipeline_cpp` and `build.py` (a checkout) or `bin` with sibling
// `game` (or `script_output`) (a staged install\<game>).  Empty when neither is
// found, in which case `--dir` becomes required.
std::string default_project_dir();

// Strict UTF-8 <-> UTF-16 conversions using MB_ERR_INVALID_CHARS /
// WC_ERR_INVALID_CHARS; throw std::runtime_error on invalid sequences.
std::wstring utf8_to_wide(std::string_view utf8);
std::string wide_to_utf8(std::wstring_view wide);

// Reads the current process command line via GetCommandLineW +
// CommandLineToArgvW and returns argv[1..argc-1] encoded as UTF-8.
std::vector<std::string> process_utf8_args();

// Formats a Windows command line matching CommandLineToArgvW parsing rules:
// wraps argv[0] (`exe_path`) in quotes without backslash doubling (rejecting
// embedded `"` with std::runtime_error), and applies 2n / 2n+1 backslash
// escaping to argv[1..N].
std::string format_windows_command_line(std::string_view exe_path,
                                        const std::vector<std::string>& args);

// Splits a Windows command line via CommandLineToArgvW into UTF-8 tokens
// (argv[0..argc-1]).
std::vector<std::string> split_windows_command_line(std::string_view cmdline);

// Runs `exe` with `args` via CreateProcessW and returns the child exit code.
// Throws std::runtime_error if process creation or synchronization fails.
int run_process(const std::filesystem::path& exe,
                const std::vector<std::string>& args,
                const std::filesystem::path& working_dir = {});

// `--help` and `--dir` only, for executables that operate solely on files
// inside the project folder and never accept `--game-dir`.
po::options_description project_only_options(std::string& project_dir);

// `--dir` and `--game-dir`, bound to the caller's strings for executables that
// read or write both the project folder and the game installation.
po::options_description common_options(std::string& project_dir,
                                       std::string& game_dir);

// Parses `args` (argv[1..N], without argv[0]) into `vm`.  Returns an exit code
// when main should stop immediately (kExitOk for --help, kExitFailure for a
// malformed command line), or std::nullopt to continue.
std::optional<int> parse_command_line(const std::vector<std::string>& args,
                                      const po::options_description& desc,
                                      po::variables_map& vm);

// Parses `argc`/`argv` (including argv[0]) into `vm`.
std::optional<int> parse_command_line(int argc, const char* const* argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm);

// Parses the live UTF-16 process command line into `vm`.
std::optional<int> parse_process_command_line(
    const po::options_description& desc,
    po::variables_map& vm);

// Project-only parser overloads enforcing that `--game-dir` is neither
// registered in `desc` nor accepted on the command line.
std::optional<int> parse_project_only_command_line(
    const std::vector<std::string>& args,
    const po::options_description& desc,
    po::variables_map& vm);

std::optional<int> parse_project_only_command_line(
    int argc, const char* const* argv,
    const po::options_description& desc,
    po::variables_map& vm);

std::optional<int> parse_process_project_only_command_line(
    const po::options_description& desc,
    po::variables_map& vm);

}  // namespace snt::apps
