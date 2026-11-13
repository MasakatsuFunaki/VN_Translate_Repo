// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_build_pak -- rebuild English .nps scripts and pack 0script.pak (v4
// KishinHisyouDB archive) from script_output/translated_text.json and
// script_output/nps_reconstruction_map.json.
#include "apps/paths.h"
#include "common/util.h"
#include "pak/nps_rebuilder.h"
#include "patch/deploy.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace ksh;

int main() {
    try {
        setup_console_utf8();
        namespace po = ksh::apps::po;

        std::string project_dir;
        std::string game_dir;
        bool verify = false;

        po::options_description desc(
            "02_build_pak -- rebuild Kishin Hisho English .nps scripts and 0script.pak");
        desc.add(ksh::apps::common_options(project_dir));
        desc.add_options()
            ("game-dir", po::value(&game_dir),
             "Output game directory to deploy 0script.pak and script\\*.nps into")
            ("verify", po::bool_switch(&verify),
             "Round-trip unpack and verify every entry in the generated 0script.pak");

        po::variables_map vm;
        if (auto rc = ksh::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        std::vector<std::string> out_dirs;
        if (!game_dir.empty()) {
            out_dirs.push_back(game_dir);
        }
        if (game_dir.empty()) {
            out_dirs.push_back(patch::path_join(
                patch::path_join(patch::path_join(project_dir, "build"), "install"), "game"));
            std::error_code ec;
            const fs::path parent_root = fs::u8path(project_dir).parent_path();
            const fs::path dist_game_root = parent_root / "install" / "kishin_hisho";
            if (fs::is_directory(dist_game_root, ec)) {
                out_dirs.push_back((dist_game_root / "game").u8string());
            }
        }

        const pak::NpsRebuilder rebuilder;
        const pak::NpsRebuildStats stats =
            rebuilder.RebuildAndDeploy(project_dir, out_dirs, verify);

        log_info("[02_build_pak] Rebuilt " + std::to_string(stats.total_nps_scripts) +
                 " .nps scripts (" + std::to_string(stats.total_pak_files) +
                 " total files in 0script.pak, " + std::to_string(stats.pak_size_bytes) +
                 " bytes).");
        log_info("[02_build_pak] Total blocks: " + std::to_string(stats.total_blocks) +
                 ", Spot-check modified: " + std::to_string(stats.modified_blocks) +
                 ", Word-wrapped to box budget: " + std::to_string(stats.rewrapped_blocks) + ".");
        for (const auto& dir : out_dirs) {
            log_info("[02_build_pak] Deployed loose script\\*.nps and 0script.pak -> " + dir);
        }
        return ksh::apps::kExitOk;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return ksh::apps::kExitError;
    }
}
