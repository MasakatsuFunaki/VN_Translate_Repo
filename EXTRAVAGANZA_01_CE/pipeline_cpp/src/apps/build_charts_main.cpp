// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 05_build_charts -- translate and repack chart/*.fxf flowchart files using
// script_output/chart_translation_cache.json.
#include "apps/paths.h"
#include "charts/charts.h"
#include "common/util.h"

#include <exception>
#include <filesystem>
#include <string>

int main() {
    exc::setup_console_utf8();
    namespace po = exc::apps::po;

    try {
        std::string project;
        std::string game;
        po::options_description desc("05_build_charts -- translate flowchart files");
        desc.add(exc::apps::common_options(project, game));
        po::variables_map vm;
        if (const auto rc = exc::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::filesystem::path out_dir =
            std::filesystem::u8path(project) / "script_output";
        std::filesystem::create_directories(out_dir);

        const std::filesystem::path game_root = std::filesystem::u8path(game);
        exc::charts::ChartOptions opt;
        opt.chart_dir = (game_root / "chart").u8string();
        opt.backup_dir = (game_root / "chart_backup").u8string();
        opt.cache_file = (out_dir / "chart_translation_cache.json").u8string();

        return exc::charts::run_translate_charts(opt);
    } catch (const std::exception& e) {
        exc::log_info(std::string("[ERROR] ") + e.what());
        return exc::apps::kExitError;
    }
}
