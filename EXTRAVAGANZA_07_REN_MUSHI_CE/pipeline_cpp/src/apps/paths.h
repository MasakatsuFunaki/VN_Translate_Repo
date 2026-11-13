// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Command-line and child-process handling for the Ren_mushi_CE pipeline apps.
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

std::vector<std::string> process_utf8_args();

std::optional<int> parse_process_command_line(const po::options_description& desc,
                                              po::variables_map& vm);

std::wstring build_process_command_line(std::string_view exe_path,
                                        const std::vector<std::string>& args);

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
        ("dir", dir, "Ren_mushi_CE project folder")
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

inline std::optional<int> parse_command_line(int argc, char** argv,
                                             const po::options_description& desc,
                                             po::variables_map& vm) {
    try {
        const auto parsed = po::command_line_parser(argc, argv).options(desc).run();
        const auto stray = po::collect_unrecognized(parsed.options, po::include_positional);
        if (!stray.empty()) {
            std::cerr << "[ERROR] unexpected argument '" << stray.front() << "'\n\n"
                      << desc << std::endl;
            return kExitError;
        }

        po::store(parsed, vm);
        if (vm.count("help")) {
            std::cout << desc << std::endl;
            return kExitHelp;
        }
        po::notify(vm);
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return kExitError;
    }
    return std::nullopt;
}

}  // namespace exm::apps
