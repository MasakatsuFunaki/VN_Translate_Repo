// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 04_patch_menus -- validate and repack Data/sin_sysd.dat and Bootup.dat from
// menu_patch.bin (SHINMENU v1 keyless XOR-delta patch) or replacement assets.

#include "apps/paths.h"
#include "common/util.h"
#include "menu/menu_archive_patcher.h"

#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr int kExitSuccessCode = 0;
constexpr int kExitFailureCode = 1;

static std::string ResolveDefaultPatchFile(const std::string& project_dir,
                                           const std::string& game_dir,
                                           const char* argv0) {
    std::vector<fs::path> candidates;
    if (!project_dir.empty()) {
        candidates.push_back(fs::u8path(project_dir) / "menu_patch.bin");
    }
    if (!game_dir.empty()) {
        candidates.push_back(fs::u8path(game_dir) / "menu_patch.bin");
    }
    if (argv0 != nullptr && argv0[0] != '\0') {
        const fs::path exe_dir = fs::u8path(argv0).parent_path();
        if (!exe_dir.empty()) {
            candidates.push_back(exe_dir / ".." / "game" / "menu_patch.bin");
        }
    }
    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate.u8string();
        }
    }
    if (!project_dir.empty()) {
        return (fs::u8path(project_dir) / "menu_patch.bin").u8string();
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    shin::setup_console_utf8();
    namespace po = shin::apps::po;

    std::string project_dir;
    shin::menu::MenuPatchOptions patch_options;

    po::options_description desc(
        "04_patch_menus -- validate and repack sin_sysd.dat and Bootup.dat menu assets");
    desc.add(shin::apps::common_options(project_dir, patch_options.game_dir));
    desc.add_options()
        ("assets-dir", po::value(&patch_options.assets_dir),
         "menu assets directory (default: <dir>/menu_assets)")
        ("patch-file", po::value(&patch_options.patch_file),
         "XOR delta menu patch file (default: <dir>/menu_patch.bin)")
        ("build-patch", po::value(&patch_options.build_patch_out),
         "build SHINMENU v1 XOR delta patch file at the given output path")
        ("verify", po::bool_switch(&patch_options.verify),
         "verify decompressed payload and TGA integrity of repacked archives");

    po::variables_map vm;
    if (const auto exit_code = shin::apps::parse_command_line(argc, argv, desc, vm)) {
        return *exit_code;
    }

    if (patch_options.assets_dir.empty()) {
        patch_options.assets_dir = (fs::u8path(project_dir) / "menu_assets").u8string();
    }
    if (patch_options.patch_file.empty()) {
        patch_options.patch_file = ResolveDefaultPatchFile(
            project_dir, patch_options.game_dir, argc > 0 ? argv[0] : nullptr);
    }

    try {
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
        return kExitFailureCode;
    }
}
