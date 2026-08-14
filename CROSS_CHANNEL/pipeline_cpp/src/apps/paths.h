// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Command-line handling for the pipeline apps (Boost.ProgramOptions).
//
// Every app derives its paths from the CROSS_CHANNEL project folder, found at
// run time (see default_project_dir) and overridable with `--dir <path>`, plus
// `--game-dir <path>` for the game installation.
#pragma once

#include <iostream>
#include <optional>
#include <string>

#include <boost/program_options.hpp>

namespace crc::apps {

namespace po = boost::program_options;

// The folder holding this executable.  Empty when Windows will not say.
std::string exe_dir();

// The project folder: the nearest folder at or above this executable holding
// either `pipeline_cpp` and `build.py` (a checkout) or `bin` and `script_output`
// (a staged install\<game>).  Empty when neither is found, and `--dir` below
// then becomes required.
//
// Not the working directory, and not a compiled-in path: neither survives the
// tree moving.
std::string default_project_dir();

// --dir / --game-dir, bound to the caller's strings.
//
// EVERY app registers both, even the ones that never read --game-dir:
// 00_run_all passes the pair to all of its children, and unlike the
// hand-rolled parser this replaced, Program_options treats an unregistered
// option as an error rather than ignoring it.
//
// --game-dir has no default: the game install path differs per machine.
inline po::options_description common_options(std::string& project_dir,
                                              std::string& game_dir) {
    // Found: it is the default.  Not found -- this executable is not in the
    // project tree -- and --dir becomes required, so the run stops with a
    // message instead of deriving every path from a folder that merely looked
    // plausible.
    auto* dir = po::value(&project_dir);
    if (const std::string found = default_project_dir(); found.empty())
        dir->required();
    else
        dir->default_value(found);

    po::options_description desc("Common options");
    desc.add_options()
        ("help,h", "show this help and exit")
        ("dir", dir, "CROSS_CHANNEL project folder")
        ("game-dir", po::value(&game_dir)->required(),
         "game installation folder");
    return desc;
}

// Parses argv into `vm`.  Returns an exit code when main should stop
// immediately (0 for --help, 1 for a malformed command line), or nullopt
// to carry on.
inline std::optional<int> parse_command_line(int argc, char** argv,
                                             const po::options_description& desc,
                                             po::variables_map& vm) {
    try {
        auto parsed = po::command_line_parser(argc, argv).options(desc).run();

        // No app takes a positional argument.  An unknown --option is an
        // error, but Program_options drops a stray bare token silently --
        // so a typo would launch a full pipeline run instead of failing.
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

// ---- derived paths (every app agrees on these) -----------------------------

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
inline std::string test_dir(const std::string& project) { return project + "\\test"; }
// NOTE: the TSV sits in the project root, not in script_output.
inline std::string out_tsv(const std::string& project) { return project + "\\translations.tsv"; }
inline std::string game_tsv(const std::string& game) { return game + "\\translations.tsv"; }

}  // namespace crc::apps
