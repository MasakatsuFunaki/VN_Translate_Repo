// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Command-line handling and wide-process execution for the EXTRA_VA_MIZUNA_CE
// pipeline applications.
#pragma once

#include <boost/program_options.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exm::apps {

namespace po = boost::program_options;

inline constexpr int kExitOk = 0;
inline constexpr int kExitHelp = 0;
inline constexpr int kExitError = 1;

std::string exe_dir();
std::string default_project_dir();

// Resolves the project root by walking ancestor directories from start_dir:
// prefers a developer checkout (pipeline_cpp + build.py) before falling back to
// a staged release layout (bin + game, or legacy bin + script_output).
std::string find_project_dir_from(const std::filesystem::path& start_dir);

po::options_description common_options(std::string& project_dir,
                                       std::string& game_dir);

po::options_description game_only_options(std::string& game_dir);

std::vector<std::string> process_utf8_args();

std::optional<int> parse_command_line(int argc, char** argv,
                                      const po::options_description& desc,
                                      po::variables_map& vm);

std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm);

std::wstring build_process_command_line(std::string_view exe_path,
                                        const std::vector<std::string>& args);

int run_process(std::string_view exe_path,
                const std::vector<std::string>& args);

}  // namespace exm::apps
