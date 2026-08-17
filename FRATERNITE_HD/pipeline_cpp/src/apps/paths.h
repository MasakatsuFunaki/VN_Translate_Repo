// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Command-line handling for the pipeline apps (Boost.ProgramOptions).
#pragma once

#include <iostream>
#include <optional>
#include <string>

#include <boost/program_options.hpp>

namespace frat::apps {

namespace po = boost::program_options;

std::string exe_dir();

// Walks up from exe_dir() looking for a checkout or staged install layout.
// Empty when nothing matches — --dir then becomes required.
std::string default_project_dir();

// Every app registers both flags; 00_run_all forwards them to children.
inline po::options_description common_options(std::string& project_dir,
                                              std::string& game_dir) {
    auto* dir = po::value(&project_dir);
    if (const std::string found = default_project_dir(); found.empty())
        dir->required();
    else
        dir->default_value(found);

    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("dir", dir, "FRATERNITE_HD project folder")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

// Returns an exit code (0 = --help, 1 = error) or nullopt to proceed.
inline std::optional<int> parse_command_line(int argc, char** argv,
                                             const po::options_description& desc,
                                             po::variables_map& vm) {
    try {
        auto parsed = po::command_line_parser(argc, argv).options(desc).run();

        // Program_options silently drops stray bare tokens; reject them.
        auto stray = po::collect_unrecognized(parsed.options, po::include_positional);
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

}  // namespace frat::apps
