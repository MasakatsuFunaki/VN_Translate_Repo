// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 01_extract -- extract all Japanese text from .spt script files
// (the SPT reader lives in src/spt/).
#include "apps/paths.h"
#include "common/util.h"
#include "extract/extractor.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

int main() {
    namespace po = mgi::apps::po;

    try {
        mgi::setup_console_utf8();

        std::string project;
        std::string game;
        po::options_description desc("01_extract -- extract Japanese text from SPT files");
        desc.add(mgi::apps::common_options(project, game));
        po::variables_map vm;
        if (const auto rc = mgi::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::filesystem::path out_dir =
            std::filesystem::u8path(project) / "script_output";
        std::filesystem::create_directories(out_dir);
        const std::string spt_dir = mgi::extract::resolve_spt_dir(game);

        mgi::log_info("=== Step 1: Extract text from SPT files ===");
        mgi::log_info("SPT directory: " + spt_dir + "\n");
        return mgi::extract::run_extract(
            spt_dir, (out_dir / "extracted_text.json").u8string());
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return mgi::apps::kExitError;
    }
}
