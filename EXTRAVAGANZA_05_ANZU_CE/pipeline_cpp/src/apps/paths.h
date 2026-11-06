// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Command-line handling for the WelcomeToAnzU_CE pipeline apps (Boost.ProgramOptions).
#pragma once

#include <iostream>
#include <optional>
#include <string>

#include <boost/program_options.hpp>

namespace exm::apps {

namespace po = boost::program_options;

std::string exe_dir();
std::string default_project_dir();
std::string find_project_dir_from(const std::string& start_dir);

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
        ("dir", dir, "EXTRAVAGANZA_05_ANZU_CE project folder")
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
            return 1;
        }

        po::store(parsed, vm);
        if (vm.count("help")) {
            std::cout << desc << std::endl;
            return 0;
        }
        po::notify(vm);
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return 1;
    }
    return std::nullopt;
}

}  // namespace exm::apps
