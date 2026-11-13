// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 04_patch_menus -- repack dwq/sys0.gpk + dwq/sys0.gtb with the translated
// menu sprites and patch standalone UI files using menu_patch.bin.
//
//   04_patch_menus --game-dir <dir>
#include "apps/paths.h"
#include "common/util.h"
#include "menu/menu_patcher.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kPatchBinFileName = "menu_patch.bin";

fs::path resolve_patch_bin_path() {
    std::vector<fs::path> candidates;
    if (const std::string project = exm::apps::default_project_dir(); !project.empty()) {
        const fs::path proj_root = fs::u8path(project);
        candidates.push_back(proj_root / "game" / kPatchBinFileName);
        candidates.push_back(proj_root / kPatchBinFileName);
    }
    if (const std::string exe = exm::apps::exe_dir(); !exe.empty()) {
        const fs::path bin_dir = fs::u8path(exe);
        candidates.push_back(bin_dir / ".." / "game" / kPatchBinFileName);
        candidates.push_back(bin_dir / kPatchBinFileName);
    }

    std::error_code ec;
    for (const fs::path& candidate : candidates) {
        if (fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
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
            "04_patch_menus -- apply English menu graphics and UI layout patch");
        desc.add(exm::apps::game_only_options(game));

        po::variables_map vm;
        if (const auto rc = exm::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const fs::path patch_bin_path = resolve_patch_bin_path();
        if (patch_bin_path.empty()) {
            std::cerr << "[ERROR] menu_patch.bin not found.\n";
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
