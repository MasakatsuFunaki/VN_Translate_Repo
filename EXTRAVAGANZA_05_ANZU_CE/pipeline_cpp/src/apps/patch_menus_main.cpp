// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// 04_patch_menus -- apply menu_patch.bin to game menu/UI archives and configs.
#include "apps/paths.h"
#include "common/util.h"
#include "menu/menu_patcher.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path resolve_menu_patch_bin() {
    std::vector<fs::path> candidates;
    if (const std::string proj = exm::apps::default_project_dir(); !proj.empty()) {
        const fs::path proj_root = fs::u8path(proj);
        candidates.push_back(proj_root / "game" / "menu_patch.bin");
        candidates.push_back(proj_root / "menu_patch.bin");
    }
    if (const std::string exe = exm::apps::exe_dir(); !exe.empty()) {
        const fs::path exe_root = fs::u8path(exe);
        candidates.push_back(exe_root / ".." / "game" / "menu_patch.bin");
        candidates.push_back(exe_root / "menu_patch.bin");
    }

    std::error_code ec;
    for (const fs::path& candidate : candidates) {
        if (fs::is_regular_file(candidate, ec) && !ec) {
            return candidate;
        }
        ec.clear();
    }
    return {};
}

}  // namespace

int main() {
    exm::setup_console_utf8();
    namespace po = exm::apps::po;

    try {
        std::string game;
        po::options_description desc(
            "04_patch_menus -- apply menu_patch.bin to game menu/UI files");
        desc.add(exm::apps::game_only_options(game));
        po::variables_map vm;
        if (const auto rc = exm::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const fs::path patch_bin_path = resolve_menu_patch_bin();
        if (patch_bin_path.empty()) {
            std::cerr << "[ERROR] menu_patch.bin not found relative to project or "
                         "executable directory.\n";
            return exm::apps::kExitError;
        }

        const auto summary = exm::menu::MenuArchivePatcher::apply_patch_bin_to_game_dir(
            fs::u8path(game), patch_bin_path, false);
        std::cout << "Applied " << patch_bin_path.u8string() << ": "
                  << summary.patched_sprite_entries << "/"
                  << summary.total_archive_entries << " sys0.gpk sprites and "
                  << summary.patched_standalone_files << " standalone files.\n";
        return exm::apps::kExitOk;
    } catch (const std::exception& ex) {
        std::cerr << "[ERROR] " << ex.what() << "\n";
        return exm::apps::kExitError;
    }
}
