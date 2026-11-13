// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_build_xtx -- translate the .xtx config files (character name plate).
//
// A one-shot asset tool, not part of 00_run_all: it rewrites files that ship
// with the game rather than the script text, and it only has to run once per
// installation.
#include "apps/paths.h"
#include "common/util.h"
#include "xtx/xtx.h"

#include <exception>
#include <iostream>
#include <string>

int main() {
    namespace po = mgi::apps::po;

    try {
        mgi::setup_console_utf8();

        std::string game;
        po::options_description desc("03_build_xtx -- translate .xtx config files");
        desc.add(mgi::apps::game_only_options(game));
        po::variables_map vm;
        if (const auto rc = mgi::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        return mgi::xtx::run_translate_xtx(game);
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return mgi::apps::kExitError;
    }
}
