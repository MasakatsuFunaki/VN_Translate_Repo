// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 04_patch_menus -- validate and repack Data/sin_sysd.dat and Bootup.dat from
// menu_patch.bin (SHINMENU v1 keyless XOR-delta patch).

#include "apps/paths.h"
#include "common/util.h"
#include "menu/menu_archive_patcher.h"

#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr int kExitSuccessCode = 0;

std::string ResolveDefaultPatchFile(const std::string& project_dir,
                                    const std::string& game_dir,
                                    const std::string& exe_dir) {
    std::vector<fs::path> candidates;
    if (!project_dir.empty()) {
        const fs::path project = fs::u8path(project_dir);
        candidates.push_back(project / "game" / "menu_patch.bin");
        candidates.push_back(project / "menu_patch.bin");
    }
    if (!game_dir.empty()) {
        candidates.push_back(fs::u8path(game_dir) / "menu_patch.bin");
    }
    if (!exe_dir.empty()) {
        const fs::path exe = fs::u8path(exe_dir);
        candidates.push_back(exe / ".." / "game" / "menu_patch.bin");
        candidates.push_back(exe / "menu_patch.bin");
    }
    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate.u8string();
        }
    }
    return {};
}

}  // namespace

int main() {
    shin::setup_console_utf8();
    namespace po = shin::apps::po;

    try {
        shin::menu::MenuPatchOptions patch_options;

        po::options_description desc(
            "04_patch_menus -- validate and repack sin_sysd.dat and Bootup.dat menu assets");
        desc.add(shin::apps::game_only_options(patch_options.game_dir));
        desc.add_options()
            ("verify", po::bool_switch(&patch_options.verify),
             "verify decompressed payload and TGA integrity of repacked archives");

        po::variables_map vm;
        if (const auto exit_code = shin::apps::parse_process_command_line(desc, vm)) {
            return *exit_code;
        }

        patch_options.patch_file = ResolveDefaultPatchFile(
            shin::apps::default_project_dir(), patch_options.game_dir, shin::apps::exe_dir());
        if (patch_options.patch_file.empty()) {
            shin::log_info("[ERROR] menu_patch.bin not found");
            return shin::apps::kExitError;
        }

        shin::menu::MenuArchivePatcher patcher(std::move(patch_options));
        const shin::menu::MenuPatchStats stats = patcher.PatchArchives();
        shin::log_info(
            "Patched sin_sysd.dat (" + std::to_string(stats.sin_sysd_patched_tga_count) +
            " TGAs, " + std::to_string(stats.sin_sysd_patched_csv_count) + " CSVs, " +
            std::to_string(stats.sin_sysd_patched_raw_count) + " raw of " +
            std::to_string(stats.sin_sysd_total_entries) + " entries) and Bootup.dat (" +
            std::to_string(stats.bootup_patched_count) + " of " +
            std::to_string(stats.bootup_total_entries) + " entries)" +
            (stats.verified ? " [VERIFIED]" : ""));
        return kExitSuccessCode;
    } catch (const std::exception& ex) {
        shin::log_info(std::string("[ERROR] ") + ex.what());
        return shin::apps::kExitError;
    }
}
