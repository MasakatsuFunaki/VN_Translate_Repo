// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_find_narrative_cg -- scan the DDP CG archives for images carrying Japanese
// text, translate them via Claude vision and repack the archives.
//
// Usage:
//   03_find_narrative_cg              # scan + translate + repack
//   03_find_narrative_cg --scan-only  # scan and save candidates.json only
//   03_find_narrative_cg --repack     # repack using existing patched BMPs
//   03_find_narrative_cg --no-resume  # scan from scratch
#include "cg/cg_pipeline.h"
#include "common/util.h"

#include "apps/paths.h"

int main(int argc, char** argv) {
    shin::setup_console_utf8();
    namespace po = shin::apps::po;

    shin::cg::CgOptions opt;
    po::options_description desc(
        "03_find_narrative_cg -- scan/translate/repack narrative CGs");
    desc.add(shin::apps::common_options(opt.project_dir, opt.game_dir));
    desc.add_options()
        ("scan-only", po::bool_switch(&opt.scan_only),
         "Only scan; do not translate or repack")
        ("repack", po::bool_switch(&opt.repack_only),
         "Skip scan+translate; just repack existing patched BMPs")
        ("no-resume", po::bool_switch(&opt.no_resume), "Start scan from scratch")
        ("extract-ddp", po::value(&opt.extract_ddp),
         "path to extract_ddp.exe (default: <repo>\\TOOLS\\garbro\\extract_ddp.exe)")
        ("replay", po::value(&opt.replay_file),
         "verification only: replay recorded vision responses from a JSONL file");
    po::variables_map vm;
    if (auto rc = shin::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    try {
        return shin::cg::run_find_narrative_cg(opt);
    } catch (const std::exception& e) {
        shin::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
