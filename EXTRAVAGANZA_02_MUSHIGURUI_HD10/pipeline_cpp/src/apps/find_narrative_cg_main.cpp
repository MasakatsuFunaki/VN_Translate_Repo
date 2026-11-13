// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 04_find_narrative_cg -- repack narrative CG overlays into dwq/*.gpk.
//
// Usage:
//   04_find_narrative_cg --game-dir <dir>           # verify cached translations and repack
//   04_find_narrative_cg --game-dir <dir> --repack  # repack from existing patched images
#include "apps/paths.h"
#include "cg/cg_pipeline.h"
#include "common/util.h"

#include <exception>
#include <iostream>
#include <string>

int main() {
    namespace po = mgi::apps::po;

    try {
        mgi::setup_console_utf8();

        mgi::cg::CgOptions opt;
        po::options_description desc(
            "04_find_narrative_cg -- repack translated narrative CGs");
        desc.add(mgi::apps::common_options(opt.project_dir, opt.game_dir));
        desc.add_options()
            ("repack", po::bool_switch(&opt.repack_only),
             "Skip scan+translate; just repack existing patched images");
        po::variables_map vm;
        if (const auto rc = mgi::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        return mgi::cg::run_find_narrative_cg(opt);
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return mgi::apps::kExitError;
    }
}
