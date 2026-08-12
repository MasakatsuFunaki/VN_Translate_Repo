// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 01_extract -- extract all Japanese text from .spt script files
// (the SPT reader itself lives in src/spt/).
#include <filesystem>

#include "common/util.h"
#include "extract/extractor.h"

#include "apps/paths.h"

int main(int argc, char** argv) {
    exc::setup_console_utf8();
    namespace po = exc::apps::po;

    std::string project, game;
    po::options_description desc("01_extract -- extract Japanese text from SPT files");
    desc.add(exc::apps::common_options(project, game));
    po::variables_map vm;
    if (auto rc = exc::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    const std::string out_dir = project + "\\script_output";
    std::filesystem::create_directories(std::filesystem::u8path(out_dir));
    const std::string spt_dir = exc::extract::resolve_spt_dir(game);

    exc::log_info("=== Step 1: Extract text from SPT files ===");
    exc::log_info("SPT directory: " + spt_dir + "\n");
    try {
        return exc::extract::run_extract(spt_dir, out_dir + "\\extracted_text.json");
    } catch (const std::exception& e) {
        exc::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
