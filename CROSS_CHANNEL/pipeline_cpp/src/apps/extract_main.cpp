// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 01_extract -- extract all Japanese text from sn.bin.
#include "apps/paths.h"
#include "common/util.h"
#include "extract/extractor.h"

#include <exception>
#include <filesystem>
#include <string>

int main() {
    try {
        crc::setup_console_utf8();
        namespace po = crc::apps::po;

        std::string project;
        std::string game;
        po::options_description desc("01_extract -- extract Japanese text from sn.bin");
        desc.add(crc::apps::common_options(project, game));
        po::variables_map vm;
        if (const auto rc = crc::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        std::filesystem::create_directories(
            std::filesystem::u8path(crc::apps::output_dir(project)));

        crc::log_info("Extract text from sn.bin");
        crc::log_info("Game directory: " + game + "\n");
        return crc::extract::run_extract(crc::apps::sn_bin(game),
                                         crc::apps::extracted_json(project));
    } catch (const std::exception& e) {
        crc::log_info(std::string("[ERROR] ") + e.what());
        return crc::apps::kExitError;
    }
}
