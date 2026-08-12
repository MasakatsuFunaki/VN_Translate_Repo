// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 04_find_narrative_cg -- scan dwq/*.gpk for images carrying Japanese text,
// translate them via Claude vision and repack the archives.
//
// Usage:
//   04_find_narrative_cg              # scan + translate + repack
//   04_find_narrative_cg --scan-only  # scan and save candidates.json only
//   04_find_narrative_cg --repack     # repack using existing patched images
//   04_find_narrative_cg --no-resume  # scan from scratch
#include "cg/cg_pipeline.h"
#include "common/util.h"

#include "apps/paths.h"

int main(int argc, char** argv) {
    mgi::setup_console_utf8();
    namespace po = mgi::apps::po;

    mgi::cg::CgOptions opt;
    po::options_description desc(
        "04_find_narrative_cg -- scan/translate/repack narrative CGs");
    desc.add(mgi::apps::common_options(opt.project_dir, opt.game_dir));
    desc.add_options()
        ("scan-only", po::bool_switch(&opt.scan_only),
         "Only scan; do not translate or repack")
        ("repack", po::bool_switch(&opt.repack_only),
         "Skip scan+translate; just repack existing patched images")
        ("no-resume", po::bool_switch(&opt.no_resume), "Start scan from scratch")
        ("replay", po::value(&opt.replay_file),
         "verification only: replay recorded vision responses from a JSONL file");
    po::variables_map vm;
    if (auto rc = mgi::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    try {
        return mgi::cg::run_find_narrative_cg(opt);
    } catch (const std::exception& e) {
        mgi::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
