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
#include "apps/paths.h"
#include "choices/choice_render.h"
#include "common/util.h"

#include <exception>
#include <iostream>
#include <string>

int main() {
    namespace po = mgi::apps::po;

    try {
        mgi::setup_console_utf8();

        mgi::choices::Options opt;
        po::options_description desc(
            "05_replace_choice_text -- render English over choice-menu screenshots");
        desc.add(mgi::apps::project_only_options(opt.project_dir));
        po::variables_map vm;
        if (const auto rc = mgi::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        return mgi::choices::run_replace_choice_text(opt);
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return mgi::apps::kExitError;
    }
}
