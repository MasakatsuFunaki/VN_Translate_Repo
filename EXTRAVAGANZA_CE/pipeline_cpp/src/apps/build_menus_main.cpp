// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 06_build_menus -- repack dwq/sys0.gpk + dwq/sys0.gtb with the 64 translated
// menu sprites and patch init2/mainControl.fxf + init2/backlog.fxf.
//
//   06_build_menus --game-dir <dir>          # repack sys0.gpk/gtb + patch init2/*.fxf
//   06_build_menus --game-dir <dir> --test   # dry run: validate inputs in memory
#include <exception>
#include <string>

#include "apps/paths.h"
#include "common/util.h"
#include "menus/menu_patcher.h"

int main(int argc, char** argv) {
    exc::setup_console_utf8();
    namespace po = exc::apps::po;

    std::string project;
    std::string game;
    std::string png_dir;
    std::string sprites_dir;
    std::string patch_bin_path;
    std::string write_patch_bin_path;
    bool test_mode = false;

    po::options_description desc(
        "06_build_menus -- repack menu graphics archive and system UI configs");
    desc.add(exc::apps::common_options(project, game));
    desc.add_options()
        ("png-dir", po::value(&png_dir),
         "directory containing the 64 rendered menu PNG files")
        ("sprites-dir", po::value(&sprites_dir),
         "alias for --png-dir")
        ("patch-bin", po::value(&patch_bin_path),
         "path to existing extravaganza_patch.bin container")
        ("write-patch-bin", po::value(&write_patch_bin_path),
         "output path for generated extravaganza_patch.bin container")
        ("test", po::bool_switch(&test_mode),
         "dry run: validate archives and rendered sprites without writing files");

    po::variables_map vm;
    if (auto rc = exc::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    if (png_dir.empty() && !sprites_dir.empty()) {
        png_dir = sprites_dir;
    }
    if (png_dir.empty()) {
        png_dir = exc::menus::default_rendered_png_dir(project);
    }
    if (patch_bin_path.empty()) {
        patch_bin_path = project + "\\extravaganza_patch.bin";
    }

    exc::menus::MenuBuildOptions opt;
    opt.game_dir = game;
    opt.rendered_png_dir = png_dir;
    opt.patch_bin_path = patch_bin_path;
    opt.write_patch_bin_path = write_patch_bin_path;
    opt.test_mode = test_mode;

    try {
        const auto summary = exc::menus::run_build_menus(opt);
        exc::log_info("06_build_menus complete: " +
                      std::to_string(summary.patched_sprite_entries) + "/" +
                      std::to_string(summary.total_archive_entries) +
                      " sys0.gpk entries translated (" +
                      std::to_string(summary.untouched_sprite_entries) + " untouched).");
        return 0;
    } catch (const std::exception& e) {
        exc::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
