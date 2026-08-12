// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 04_translate_xtx -- translate the .xtx config files (character name plate).
#include <string>

#include "common/util.h"
#include "xtx/xtx.h"

#include "apps/paths.h"

int main(int argc, char** argv) {
    exc::setup_console_utf8();
    namespace po = exc::apps::po;

    std::string project, game;
    po::options_description desc("04_translate_xtx -- translate .xtx config files");
    desc.add(exc::apps::common_options(project, game));
    po::variables_map vm;
    if (auto rc = exc::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    try {
        return exc::xtx::run_translate_xtx(game);
    } catch (const std::exception& e) {
        exc::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
