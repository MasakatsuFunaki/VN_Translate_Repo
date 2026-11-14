// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 06_build_menus -- repack dwq/sys0.gpk + dwq/sys0.gtb with the 64 translated
// menu sprites and patch standalone game assets using extravaganza_patch.bin,
// or regenerate extravaganza_patch.bin from a prepared game directory.
//
//   06_build_menus --game-dir <dir> [--patch-bin <file>] [--test]
//   06_build_menus --game-dir <dir> --write-patch-bin <file>
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

fs::path resolve_patch_bin_path(const std::string& explicit_path) {
    std::error_code ec;
    if (!explicit_path.empty()) {
        const fs::path custom = fs::u8path(explicit_path);
        return fs::is_regular_file(custom, ec) ? custom : fs::path{};
    }

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
        std::string patch_bin_arg;
        std::string write_patch_bin_arg;
        bool test_mode = false;

        po::options_description desc(
            "06_build_menus -- repack menu graphics archive and standalone game assets");
        desc.add(exc::apps::game_only_options(game));
        desc.add_options()
            ("patch-bin", po::value(&patch_bin_arg),
             "explicit path to input extravaganza_patch.bin")
            ("write-patch-bin", po::value(&write_patch_bin_arg),
             "build and write extravaganza_patch.bin from --game-dir")
            ("test", po::bool_switch(&test_mode),
             "verify patch container against --game-dir without modifying files");

        po::variables_map vm;
        if (const auto rc = exc::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        if (!write_patch_bin_arg.empty()) {
            exc::log_info("Building keyless XOR-delta container " +
                          write_patch_bin_arg + " from " + game + "...");
            exc::menus::MenuArchivePatcher::write_patch_bin_from_game_dir(
                game, write_patch_bin_arg);
            const exc::Bytes written = exc::read_file(write_patch_bin_arg);
            const auto parsed = exc::menus::parse_patch_bin(written);
            exc::log_info("Wrote " + write_patch_bin_arg + " (" +
                          std::to_string(written.size()) + " bytes, " +
                          std::to_string(parsed.standalone_file_count) +
                          " standalone records, " +
                          std::to_string(parsed.gpk_entry_count) + " GPK entries).");
            return exc::apps::kExitOk;
        }

        const fs::path patch_bin_path = resolve_patch_bin_path(patch_bin_arg);
        if (patch_bin_path.empty()) {
            std::cerr << "[ERROR] extravaganza_patch.bin not found.\n";
            return exc::apps::kExitError;
        }

        exc::log_info((test_mode ? "Verifying keyless XOR-delta container "
                                 : "Applying keyless XOR-delta container ") +
                      patch_bin_path.u8string() + "...");
        const auto summary =
            exc::menus::MenuArchivePatcher::apply_patch_bin_to_game_dir(
                game, patch_bin_path.u8string(), test_mode);
        exc::log_info("06_build_menus complete: " +
                      std::to_string(summary.patched_standalone_files) +
                      " standalone files + " +
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
