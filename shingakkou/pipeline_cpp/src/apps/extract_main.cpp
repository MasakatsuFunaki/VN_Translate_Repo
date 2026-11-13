// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 01_extract -- extract Japanese text from sin_text.dat.
// The DDP3/Shs/HXB reader lives in src/ddp/.
//
// There is no self-test pre-flight here: those assertions are the ctest "UT"
// suites, and an app must not shell out to a test runner.
#include "apps/paths.h"
#include "common/util.h"
#include "extract/extractor.h"

#include <exception>
#include <filesystem>
#include <string>

int main() {
    shin::setup_console_utf8();
    namespace po = shin::apps::po;

    try {
        std::string project;
        std::string game;
        po::options_description desc("01_extract -- extract Japanese text from sin_text.dat");
        desc.add(shin::apps::common_options(project, game));
        po::variables_map vm;
        if (const auto rc = shin::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::string out_dir = project + "\\script_output";
        std::filesystem::create_directories(std::filesystem::u8path(out_dir));

        return shin::extract::run_extract(game, out_dir + "\\extracted_text.json");
    } catch (const std::exception& e) {
        shin::log_info(std::string("[ERROR] ") + e.what());
        return shin::apps::kExitError;
    }
}
