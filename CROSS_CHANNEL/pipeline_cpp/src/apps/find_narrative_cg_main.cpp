// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_find_narrative_cg -- scan the WillPlus CPK archives for CG / UI images
// with Japanese text and render the cached English onto the extracted BMPs.
//
//   03_find_narrative_cg --game-dir <install>
#include "apps/paths.h"
#include "cg/cg_pipeline.h"
#include "common/util.h"

#include <exception>
#include <string>

int main() {
    try {
        crc::setup_console_utf8();
        namespace po = crc::apps::po;

        std::string project;
        std::string game;
        po::options_description desc("03_find_narrative_cg -- narrative CG scanner / renderer");
        desc.add(crc::apps::common_options(project, game));
        po::variables_map vm;
        if (const auto rc = crc::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        crc::cg::CgOptions opt;
        opt.project_dir = project;
        opt.game_dir = game;
        return crc::cg::run_find_narrative_cg(opt);
    } catch (const std::exception& e) {
        crc::log_info(std::string("[ERROR] ") + e.what());
        return crc::apps::kExitError;
    }
}
