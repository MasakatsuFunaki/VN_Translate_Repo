// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 01_extract -- extract all Japanese text from sn.bin.
//
// The extractor's own correctness gate is the ctest "UT" suites, so this app
// does nothing but parse the command line and run the extraction.
#include <filesystem>

#include "common/util.h"
#include "extract/extractor.h"

#include "apps/paths.h"

int main(int argc, char** argv) {
    crc::setup_console_utf8();
    namespace po = crc::apps::po;

    std::string project, game;
    po::options_description desc("01_extract -- extract Japanese text from sn.bin");
    desc.add(crc::apps::common_options(project, game));
    po::variables_map vm;
    if (auto rc = crc::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    std::filesystem::create_directories(
        std::filesystem::u8path(crc::apps::output_dir(project)));

    crc::log_info("=== Step 1: Extract text from sn.bin ===");
    crc::log_info("Game directory: " + game + "\n");
    try {
        return crc::extract::run_extract(crc::apps::sn_bin(game),
                                         crc::apps::extracted_json(project));
    } catch (const std::exception& e) {
        crc::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
