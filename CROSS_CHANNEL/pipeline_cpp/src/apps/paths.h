// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Command-line handling, UTF-8 path discovery, and UTF-16 child process
// spawning for the pipeline executables.
//
// Every app derives its paths from the CROSS_CHANNEL project folder, found at
// run time (see default_project_dir) and overridable with `--dir <path>`, plus
// `--game-dir <path>` for the game installation.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/program_options.hpp>

namespace crc::apps {

namespace po = boost::program_options;

inline constexpr int kExitHelp = 0;
inline constexpr int kExitError = 1;

// The folder holding this executable, encoded as UTF-8.  Empty when Windows
// will not report it.
std::string exe_dir();

// Resolves the project folder from `start_dir` as UTF-8: prefers a checkout
// (`pipeline_cpp` and `build.py`) over a staged release or install layout
// (`bin` with sibling `game` or `script_output`).  Returns empty when neither
// layout is found within the bounded ancestor walk.
std::string find_project_dir_from(const std::filesystem::path& start_dir);

// The project folder at or above this executable, encoded as UTF-8.  Empty
// when neither layout is found, making `--dir` required.
std::string default_project_dir();

// `--dir` and `--game-dir`, bound to the caller's UTF-8 strings.
po::options_description common_options(std::string& project_dir,
                                       std::string& game_dir);

// Returns `argv[1..N]` from the Win32 wide command line converted to UTF-8.
std::vector<std::string> process_utf8_args();

// Parses narrow `argv` into `vm`.  Returns an exit code when `main` should
// stop immediately (`0` for `--help`, `1` on error), or `std::nullopt` to
// proceed.
std::optional<int> parse_command_line(int argc, char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm);

// Parses the process's Win32 wide command line (via `process_utf8_args`) into
// `vm`.  Returns an exit code when `main` should stop immediately, or
// `std::nullopt` to proceed.
std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm);

// Builds a `CreateProcessW`-compatible UTF-16 command line from UTF-8
// `exe_path` (`argv[0]`) and `args` (`argv[1..N]`).  Throws on empty or
// quote-containing `exe_path` or invalid UTF-8.
std::wstring build_command_line_utf16(std::string_view exe_path,
                                      const std::vector<std::string>& args);

// Spawns `exe_path` via `CreateProcessW` with `args`, waits for completion,
// and returns its exit code.  Throws `std::runtime_error` on failure.
int run_process_utf8(std::string_view exe_path,
                     const std::vector<std::string>& args);

inline std::string output_dir(const std::string& project) { return project + "\\script_output"; }
inline std::string sn_bin(const std::string& game) { return game + "\\data\\sn.bin"; }
inline std::string extracted_json(const std::string& project) {
    return output_dir(project) + "\\extracted_text.json";
}
inline std::string cache_json(const std::string& project) {
    return output_dir(project) + "\\translation_cache_anthropic.json";
}
inline std::string translated_json(const std::string& project) {
    return output_dir(project) + "\\translated_text.json";
}
// The staged runtime table lives in build\install\game rather than the project root.
inline std::string out_tsv(const std::string& project) {
    return project + "\\build\\install\\game\\translations.tsv";
}
inline std::string game_tsv(const std::string& game) { return game + "\\translations.tsv"; }

}  // namespace crc::apps
