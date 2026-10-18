// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 01_extract -- extract Japanese text from BGI BurikoCompiledScript files
// (see src/extract/extractor.cpp for the walker).
#include <filesystem>

#include "common/util.h"
#include "extract/extractor.h"

#include "apps/paths.h"

int main(int argc, char** argv) {
    snu::setup_console_utf8();
    namespace po = snu::apps::po;

    std::string project, game;
    po::options_description desc("01_extract -- extract Japanese text from BGI archives");
    desc.add(snu::apps::common_options(project, game));
    po::variables_map vm;
    if (auto rc = snu::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    std::string out_dir = project + "\\script_output";
    std::filesystem::create_directories(std::filesystem::u8path(out_dir));
    try {
        return snu::extract::run_extract(game, out_dir + "\\extracted_text.json");
    } catch (const std::exception& e) {
        snu::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
