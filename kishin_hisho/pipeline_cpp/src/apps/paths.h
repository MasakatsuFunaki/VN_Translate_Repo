// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Command-line handling and UTF-8 project-root discovery for the kishin_hisho
// tools (Boost.ProgramOptions).
//
// Every tool derives its inputs and outputs from the kishin_hisho folder,
// found at run time (see default_project_dir) and overridable with
// `--dir <path>`.
#pragma once

#include <boost/program_options.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ksh::apps {

namespace po = boost::program_options;

inline constexpr int kExitOk = 0;
inline constexpr int kExitHelp = 0;
inline constexpr int kExitError = 1;

// The folder holding this executable as UTF-8.  Empty when Windows will not say.
std::string exe_dir();

// Resolves the project folder as UTF-8 from `start_dir`: prefers a checkout
// (`pipeline_cpp` and `build.py`) over a staged release or install layout
// (`bin` with sibling `game` or `script_output`).  Returns empty when neither
// layout is found within the bounded ancestor walk.
std::string find_project_dir_from(const std::filesystem::path& start_dir);

// The project folder as UTF-8: the nearest folder at or above this executable
// holding either `pipeline_cpp` and `build.py` (a checkout) or `bin` with
// sibling `game` or `script_output` (a staged release or install\<game>).
// Empty when neither is found, and `--dir` below then becomes required.
std::string default_project_dir();

// Splits a Win32 wide command line (`CommandLineToArgvW`), skips `argv[0]`, and
// encodes `argv[1..N]` as strict UTF-8.  Returns empty when `wide_cmd_line` is
// null or empty.
std::vector<std::string> command_line_to_utf8_args(const wchar_t* wide_cmd_line);

// Returns the current process's `argv[1..N]` encoded as strict UTF-8 via
// `GetCommandLineW()` and `CommandLineToArgvW()`.
std::vector<std::string> process_utf8_args();

// `--help` and `--dir`, bound to the caller's string.
po::options_description common_options(std::string& project_dir);

// Parses synthetic narrow `argv` into `vm`.  Returns an exit code when the
// caller should stop immediately (0 for --help, 1 for a malformed command
// line), or nullopt to carry on.
std::optional<int> parse_command_line(int argc,
                                      char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm);

// Parses the current process's wide Win32 command line as UTF-8 into `vm`.
std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm);

}  // namespace ksh::apps
