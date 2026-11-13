// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_build_xtx -- translate the .xtx config files (character name plate).
#include "apps/paths.h"
#include "common/util.h"
#include "xtx/xtx.h"

#include <exception>
#include <string>

int main() {
    exm::setup_console_utf8();
    namespace po = exm::apps::po;

    try {
        std::string game;
        po::options_description desc("03_build_xtx -- translate .xtx config files");
        desc.add(exm::apps::game_only_options(game));
        po::variables_map vm;
        if (const auto rc = exm::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        return exm::xtx::run_translate_xtx(game);
    } catch (const std::exception& e) {
        exm::log_info(std::string("[ERROR] ") + e.what());
        return exm::apps::kExitError;
    }
}
