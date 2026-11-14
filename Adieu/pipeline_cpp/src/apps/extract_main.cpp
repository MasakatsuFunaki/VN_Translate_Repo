// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 01_extract -- extract Japanese text from Adieu's Scene.pck.
// The SiglusEngine container reader and scene decoder live in src/siglus/.
#include "apps/paths.h"
#include "common/util.h"
#include "extract/extractor.h"

#include <exception>
#include <filesystem>
#include <string>

int main() {
    adi::setup_console_utf8();
    try {
        namespace po = adi::apps::po;

        std::string project;
        std::string game;
        po::options_description desc("01_extract -- extract Japanese text from Scene.pck");
        desc.add(adi::apps::common_options(project, game));
        po::variables_map vm;
        if (auto rc = adi::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::string out_dir = project + "\\script_output";
        std::filesystem::create_directories(std::filesystem::u8path(out_dir));

        adi::log_info("=== Step 1: Extract text from Adieu Scene.pck ===");
        return adi::extract::run_extract(game, out_dir + "\\extracted_text.json");
    } catch (const std::exception& e) {
        adi::log_info(std::string("[ERROR] ") + e.what());
        return adi::apps::kExitError;
    }
}
