// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Command-line handling and child-process execution for the YUMEMIRYOJOKU_CE
// pipeline apps (Boost.ProgramOptions + wide Win32 process APIs).
#pragma once

#include <boost/program_options.hpp>

#include <filesystem>
#include <iostream>
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

// Returns UTF-8 encoded argv[1..N] decoded directly from GetCommandLineW().
std::vector<std::string> process_utf8_args();

// Builds a Windows CreateProcessW command line from UTF-8 executable path and
// UTF-8 arguments, preserving literal backslashes in argv[0] and applying
// CommandLineToArgvW backslash/quote escaping to argv[1..N].
std::wstring build_process_command_line(std::string_view exe_path,
                                        const std::vector<std::string>& args);

// Spawns exe_path with args via CreateProcessW and returns its exit code.
int run_process(std::string_view exe_path, const std::vector<std::string>& args);

inline po::options_description common_options(std::string& project_dir,
                                              std::string& game_dir) {
    auto* dir = po::value(&project_dir);
    if (const std::string found = default_project_dir(); found.empty()) {
        dir->required();
    } else {
        dir->default_value(found);
    }

    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("dir", dir, "YUMEMIRYOJOKU_CE project folder")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

inline po::options_description game_only_options(std::string& game_dir) {
    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

inline std::optional<int> finalize_parsed_options(
    const po::parsed_options& parsed,
    const po::options_description& desc,
    po::variables_map& vm) {
    const auto stray =
        po::collect_unrecognized(parsed.options, po::include_positional);
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

inline std::optional<int> parse_command_line(int argc, char** argv,
                                             const po::options_description& desc,
                                             po::variables_map& vm) {
    try {
        const auto parsed = po::command_line_parser(argc, argv).options(desc).run();
        return finalize_parsed_options(parsed, desc, vm);
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
}

std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm);

}  // namespace exm::apps
