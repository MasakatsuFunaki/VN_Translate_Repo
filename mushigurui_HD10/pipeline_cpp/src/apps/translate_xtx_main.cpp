// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_translate_xtx -- translate the .xtx config files (character name plate).
//
// A one-shot asset tool, not part of 00_run_all: it rewrites files that ship
// with the game rather than the script text, and it only has to run once per
// installation.
#include <string>

#include "common/util.h"
#include "xtx/xtx.h"

#include "apps/paths.h"

int main(int argc, char** argv) {
    mgi::setup_console_utf8();
    namespace po = mgi::apps::po;

    std::string project, game;
    po::options_description desc("03_translate_xtx -- translate .xtx config files");
    desc.add(mgi::apps::common_options(project, game));
    po::variables_map vm;
    if (auto rc = mgi::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    try {
        return mgi::xtx::run_translate_xtx(game);
    } catch (const std::exception& e) {
        mgi::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
