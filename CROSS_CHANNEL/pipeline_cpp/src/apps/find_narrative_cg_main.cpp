// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_find_narrative_cg -- scan the WillPlus CPK archives for CG / UI images
// with Japanese text, translate them via Claude vision and render the English
// onto the extracted BMPs.
//
//   03_find_narrative_cg              # scan + translate
//   03_find_narrative_cg --scan-only  # scan and save candidates.json only
//   03_find_narrative_cg --no-resume  # scan from scratch
//   03_find_narrative_cg --yes        # skip the interactive confirm
#include <string>

#include "cg/cg_pipeline.h"
#include "common/util.h"

#include "apps/paths.h"

int main(int argc, char** argv) {
    crc::setup_console_utf8();
    namespace po = crc::apps::po;

    std::string project, game;
    bool scan_only = false, no_resume = false, assume_yes = false;
    po::options_description desc("03_find_narrative_cg -- narrative CG scanner / translator");
    desc.add(crc::apps::common_options(project, game));
    desc.add_options()
        ("scan-only", po::bool_switch(&scan_only), "Only scan; do not translate")
        ("no-resume", po::bool_switch(&no_resume), "Start scan from scratch")
        ("yes", po::bool_switch(&assume_yes),
         "skip the interactive Claude-API confirmation (non-tty runs)");
    po::variables_map vm;
    if (auto rc = crc::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    crc::cg::CgOptions opt;
    opt.project_dir = project;
    opt.game_dir = game;
    opt.scan_only = scan_only;
    opt.no_resume = no_resume;
    opt.assume_yes = assume_yes;
    try {
        return crc::cg::run_find_narrative_cg(opt);
    } catch (const std::exception& e) {
        crc::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
