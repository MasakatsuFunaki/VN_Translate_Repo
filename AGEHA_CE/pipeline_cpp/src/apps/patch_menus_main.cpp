// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/menu_patcher.h"

#include "apps/paths.h"
#include "common/util.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

std::filesystem::path resolve_default_sprites_dir(
    const std::filesystem::path& proj_root) {
    const std::filesystem::path assets_sys0 =
        proj_root / "assets" / "menus_en" / "sys0";
    if (std::filesystem::exists(assets_sys0)) {
        return assets_sys0;
    }
    return proj_root / "menu_sprites";
}

}  // namespace

int main(int argc, char* argv[]) {
    exm::setup_console_utf8();

    std::filesystem::path proj_root = exm::apps::default_project_dir();
    if (proj_root.empty()) {
        proj_root = std::filesystem::current_path();
    }

    exm::menu::PatchMenusOptions opt;
    opt.game_dir =
        "d:\\Kits\\my_downloadedGames\\EXTRAVAGANZA\\EXTRAVAGANZA Complete Edition\\AGEHA_CE";
    opt.rendered_dir = resolve_default_sprites_dir(proj_root);
    opt.manifest_path = opt.rendered_dir / "menu_manifest.json";
    opt.patch_bin_path = proj_root / "menu_patch.bin";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dir" && i + 1 < argc) {
            proj_root = argv[++i];
            opt.rendered_dir = resolve_default_sprites_dir(proj_root);
            opt.manifest_path = opt.rendered_dir / "menu_manifest.json";
            opt.patch_bin_path = proj_root / "menu_patch.bin";
        } else if (arg == "--game-dir" && i + 1 < argc) {
            opt.game_dir = argv[++i];
        } else if (arg == "--rendered-dir" && i + 1 < argc) {
            opt.rendered_dir = argv[++i];
        } else if (arg == "--manifest" && i + 1 < argc) {
            opt.manifest_path = argv[++i];
        } else if (arg == "--patch-bin" && i + 1 < argc) {
            opt.patch_bin_path = argv[++i];
        } else if (arg == "--write-patch-bin" && i + 1 < argc) {
            opt.write_patch_bin_path = argv[++i];
        } else if (arg == "--extract-only") {
            opt.extract_only = true;
        } else if (arg == "--extract-dir" && i + 1 < argc) {
            opt.extract_dir = argv[++i];
        } else if (arg == "--test") {
            opt.test_mode = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout
                << "Usage: 04_patch_menus [--dir <project_dir>] [--game-dir <dir>] "
                   "[--rendered-dir <dir>] [--manifest <json>] [--patch-bin <path>] "
                   "[--write-patch-bin <path>] [--extract-only] [--extract-dir <dir>] "
                   "[--test]\n";
            return 0;
        }
    }

    try {
        return exm::menu::run_patch_menus(opt);
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: " << ex.what() << "\n";
        return 1;
    }
}
