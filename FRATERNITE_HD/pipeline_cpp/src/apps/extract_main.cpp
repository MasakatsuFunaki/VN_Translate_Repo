// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 01_extract -- extract all Japanese text from bn.ypf.
//
// This app runs no self-tests: the component tests for the extractor live in
// the ctest "UT" label instead.
#include <filesystem>

#include "common/util.h"
#include "extract/extractor.h"

#include "apps/paths.h"

int main(int argc, char** argv) {
    frat::setup_console_utf8();
    namespace po = frat::apps::po;

    std::string project, game;
    po::options_description desc("01_extract -- extract Japanese text for FRATERNITE_HD");
    desc.add(frat::apps::common_options(project, game));
    po::variables_map vm;
    if (auto rc = frat::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    const std::string out_dir = project + "\\script_output";
    std::filesystem::create_directories(std::filesystem::u8path(out_dir));

    try {
        return frat::extract::run_extract(game + "\\pac", out_dir + "\\extracted_text.json");
    } catch (const std::exception& e) {
        frat::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
