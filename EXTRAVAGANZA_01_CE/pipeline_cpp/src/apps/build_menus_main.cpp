// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 06_build_menus -- repack dwq/sys0.gpk + dwq/sys0.gtb with the 64 translated
// menu sprites and patch init2/mainControl.fxf + init2/backlog.fxf using
// extravaganza_patch.bin.
//
//   06_build_menus --game-dir <dir>
#include "apps/paths.h"
#include "common/util.h"
#include "menus/menu_patcher.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kPatchBinFileName = "extravaganza_patch.bin";

fs::path resolve_patch_bin_path() {
    std::vector<fs::path> candidates;
    if (const std::string project = exc::apps::default_project_dir(); !project.empty()) {
        const fs::path proj_root = fs::u8path(project);
        candidates.push_back(proj_root / "game" / kPatchBinFileName);
        candidates.push_back(proj_root / kPatchBinFileName);
    }
    if (const std::string exe = exc::apps::exe_dir(); !exe.empty()) {
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
    exc::setup_console_utf8();
    namespace po = exc::apps::po;

    try {
        std::string game;
        po::options_description desc(
            "06_build_menus -- repack menu graphics archive and system UI configs");
        desc.add(exc::apps::game_only_options(game));

        po::variables_map vm;
        if (const auto rc = exc::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const fs::path patch_bin_path = resolve_patch_bin_path();
        if (patch_bin_path.empty()) {
            std::cerr << "[ERROR] extravaganza_patch.bin not found.\n";
            return exc::apps::kExitError;
        }

        exc::log_info("Applying keyless XOR-delta container " +
                      patch_bin_path.u8string() + "...");
        const auto summary =
            exc::menus::MenuArchivePatcher::apply_patch_bin_to_game_dir(
                game, patch_bin_path.u8string(), false);
        exc::log_info("06_build_menus complete: " +
                      std::to_string(summary.patched_sprite_entries) + "/" +
                      std::to_string(summary.total_archive_entries) +
                      " sys0.gpk entries translated (" +
                      std::to_string(summary.untouched_sprite_entries) + " untouched).");
        return exc::apps::kExitOk;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return exc::apps::kExitError;
    }
}
