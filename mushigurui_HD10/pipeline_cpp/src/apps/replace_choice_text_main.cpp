// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 05_replace_choice_text -- translate the yellow choice-menu text in the
// screenshots under <project>\choices and render the English in place.
//
// A one-shot asset tool, not part of 00_run_all: it works on screenshots, not
// on the script text.
#include "choices/choice_render.h"
#include "common/util.h"

#include "apps/paths.h"

int main(int argc, char** argv) {
    mgi::setup_console_utf8();
    namespace po = mgi::apps::po;

    mgi::choices::Options opt;
    std::string game_dir;  // registered but unused, like every other mgi app
    po::options_description desc(
        "05_replace_choice_text -- render English over choice-menu screenshots");
    desc.add(mgi::apps::common_options(opt.project_dir, game_dir));
    po::variables_map vm;
    if (auto rc = mgi::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    try {
        return mgi::choices::run_replace_choice_text(opt);
    } catch (const std::exception& e) {
        mgi::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
