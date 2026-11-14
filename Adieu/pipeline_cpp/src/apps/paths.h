// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Command-line handling, UTF-8 process argument decoding, project-root
// discovery, and UTF-16 child-process spawning for the pipeline apps.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/program_options.hpp>

namespace adi::apps {

namespace po = boost::program_options;

inline constexpr int kExitOk = 0;
inline constexpr int kExitHelp = 0;
inline constexpr int kExitError = 1;
inline constexpr int kExitRefused = 2;

// The folder holding this executable, encoded as UTF-8.
std::string exe_dir();

// Resolves the project folder from `start_dir`: prefers a checkout
// (`pipeline_cpp` and `build.py`) over a staged release or install layout
// (`bin` with sibling `game` or `script_output`).  Returns empty when neither
// layout is found within the bounded ancestor walk.
std::string find_project_dir_from(const std::filesystem::path& start_dir);

// The project folder at or above this executable, encoded as UTF-8, or empty
// when neither layout is found (making `--dir` required).
std::string default_project_dir();

// `--help` and `--dir` only, for tools that operate purely on the project
// folder and never read `--game-dir` (`02_translate`, `03_speaker_gate`).
po::options_description project_only_options(std::string& project_dir);

// `--dir` and `--game-dir`, bound to the caller's strings (`00_run_all`,
// `01_extract`).
po::options_description common_options(std::string& project_dir,
                                       std::string& game_dir);

// Reads the Windows UTF-16 process command line via GetCommandLineW /
// CommandLineToArgvW, skips argv[0], and strictly transcodes argv[1..N] to
// UTF-8.
std::vector<std::string> process_utf8_args();

// Parses narrow `argv` into `vm` (for synthetic narrow-argv tests).  Returns
// an exit code when the caller should stop immediately (`0` for `--help`, `1`
// for a malformed command line), or `std::nullopt` to proceed.
std::optional<int> parse_command_line(int argc, char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm);

// Parses the live Windows UTF-16 process command line into `vm`.
std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm);

// Formats an executable path and UTF-8 argument vector into a Windows UTF-16
// command line suitable for CreateProcessW / CommandLineToArgvW.
std::wstring build_command_line_utf16(std::string_view exe_path,
                                      const std::vector<std::string>& args);

// Spawns `exe_path` directly via CreateProcessW (no cmd.exe intermediary),
// waits for termination, and returns the child's exit code.
int run_process_utf8(std::string_view exe_path,
                     const std::vector<std::string>& args);

}  // namespace adi::apps
