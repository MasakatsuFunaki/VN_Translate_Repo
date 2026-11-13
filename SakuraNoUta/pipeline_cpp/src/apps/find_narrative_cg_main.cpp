// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_find_narrative_cg -- render cached English narrative CG overlays and
// repack game archives.
#include "apps/paths.h"
#include "cg/cg_pipeline.h"
#include "common/util.h"

#include <exception>
#include <string>

int main() {
    try {
        snu::setup_console_utf8();
        namespace po = snu::apps::po;

        snu::cg::CgOptions opt;
        po::options_description desc(
            "03_find_narrative_cg -- render/repack narrative CGs");
        desc.add(snu::apps::common_options(opt.project_dir, opt.game_dir));
        desc.add_options()
            ("repack", po::bool_switch(&opt.repack_only),
             "repack using the existing candidates.json");
        po::variables_map vm;
        if (const auto rc = snu::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        return snu::cg::run_find_narrative_cg(opt);
    } catch (const std::exception& e) {
        snu::log_info(std::string("[ERROR] ") + e.what());
        return snu::apps::kExitFailure;
    }
}
