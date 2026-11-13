// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_repack -- write the translated text back into the .spt script files.
//
//   03_repack --game-dir <dir>              # patch spt/ from spt_backup/ + translated_text.json
//   03_repack --game-dir <dir> --restore    # copy spt_backup/ back over spt/
#include "apps/paths.h"
#include "common/util.h"
#include "repack/repack.h"

#include <exception>
#include <filesystem>
#include <string>

int main() {
    exc::setup_console_utf8();
    namespace po = exc::apps::po;

    try {
        std::string project;
        std::string game;
        bool restore = false;
        po::options_description desc("03_repack -- repack translated text into SPT files");
        desc.add(exc::apps::common_options(project, game));
        desc.add_options()("restore", po::bool_switch(&restore),
                          "restore the original files from backup");
        po::variables_map vm;
        if (const auto rc = exc::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::filesystem::path game_root = std::filesystem::u8path(game);
        const std::string spt_dir = (game_root / "spt").u8string();
        const std::string backup_dir = (game_root / "spt_backup").u8string();
        if (restore) {
            return exc::repack::run_restore(spt_dir, backup_dir);
        }
        const std::filesystem::path translated =
            std::filesystem::u8path(project) / "script_output" / "translated_text.json";
        return exc::repack::run_repack(translated.u8string(), spt_dir, backup_dir);
    } catch (const std::exception& e) {
        exc::log_info(std::string("[ERROR] ") + e.what());
        return exc::apps::kExitError;
    }
}
