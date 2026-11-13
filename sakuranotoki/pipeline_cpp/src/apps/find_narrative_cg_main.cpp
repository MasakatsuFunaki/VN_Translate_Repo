// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_find_narrative_cg -- scan PFS archives for CGs carrying narrative
// Japanese text and record the hits.
#include "apps/paths.h"
#include "cg/cg_pipeline.h"
#include "common/util.h"
#include "translate/gemini_client.h"

#include <exception>
#include <string>

int main() {
    snt::setup_console_utf8();
    try {
        namespace po = snt::apps::po;

        std::string project;
        std::string game;
        bool no_resume = false;
        bool yes = false;
        po::options_description desc(
            "03_find_narrative_cg -- narrative-CG scanner");
        desc.add(snt::apps::common_options(project, game));
        desc.add_options()
            ("no-resume", po::bool_switch(&no_resume),
             "start the scan from scratch (ignore narrative_candidates.json)")
            ("yes,y", po::bool_switch(&yes),
             "skip the interactive confirmation before Gemini API calls");
        po::variables_map vm;
        if (const auto rc = snt::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        snt::gemini::load_api_key();

        snt::cg::CgOptions opt;
        opt.game_dir = game;
        opt.project_dir = project;
        opt.no_resume = no_resume;
        opt.assume_yes = yes;

        return snt::cg::run_find_narrative_cg(opt);
    } catch (const std::exception& e) {
        snt::log_info(std::string("[ERROR] ") + e.what());
        return snt::apps::kExitFailure;
    }
}
