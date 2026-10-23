// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 01_extract -- extract Japanese text from Sakura no Toki Artemis-engine
// .ast scripts.  The PF8 reader lives in src/artemis/, the Lua line walker
// in src/extract/.
#include <filesystem>

#include "common/util.h"
#include "extract/extractor.h"

#include "apps/paths.h"

int main(int argc, char** argv) {
    snt::setup_console_utf8();
    namespace po = snt::apps::po;

    std::string project, game;
    po::options_description desc("01_extract -- extract Japanese text from .ast scripts");
    desc.add(snt::apps::common_options(project, game));
    po::variables_map vm;
    if (auto rc = snt::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    const std::string out_dir = project + "\\script_output";
    std::filesystem::create_directories(std::filesystem::u8path(out_dir));
    try {
        return snt::extract::run_extract(game, out_dir + "\\extracted_text.json");
    } catch (const std::exception& e) {
        snt::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
