// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_find_narrative_cg -- scan PFS archives for CGs carrying narrative
// Japanese text and record the hits.
#include <string>

#include "cg/cg_pipeline.h"
#include "common/util.h"
#include "translate/gemini_client.h"

#include "apps/paths.h"

int main(int argc, char** argv) {
    snt::setup_console_utf8();
    namespace po = snt::apps::po;

    std::string project, game;
    int max_images = 0;
    bool scan_only = false, no_resume = false, yes = false;
    po::options_description desc("03_find_narrative_cg -- narrative-CG scanner");
    desc.add(snt::apps::common_options(project, game));
    desc.add_options()
        ("max", po::value(&max_images)->default_value(0),
         "stop after scanning N images (for sanity tests)")
        ("scan-only", po::bool_switch(&scan_only), "only detect; skip translate/render")
        ("no-resume", po::bool_switch(&no_resume),
         "start the scan from scratch (ignore narrative_candidates.json)")
        ("yes,y", po::bool_switch(&yes),
         "skip the interactive confirmation before Gemini API calls");
    po::variables_map vm;
    if (auto rc = snt::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    // The scan itself decides whether a GEMINI_API_KEY is needed: a resume file
    // that already covers every target makes no request.
    snt::gemini::load_api_key();

    snt::cg::CgOptions opt;
    opt.game_dir = game;
    opt.project_dir = project;
    opt.scan_only = scan_only;
    opt.no_resume = no_resume;
    opt.assume_yes = yes;
    if (max_images > 0) opt.max_images = max_images;

    try {
        return snt::cg::run_find_narrative_cg(opt);
    } catch (const std::exception& e) {
        snt::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
