// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 04_patch_menus -- localize Sakura no Uta's UI sprites, startup splash
// screens, Extra Music banners, confirmation dialogs, quick-menu tooltips, and
// choice option plates into the keyless XOR-delta container `menu_patch.bin`.
#include "apps/paths.h"
#include "common/util.h"
#include "menu/menu_patch_builder.h"

#include <cwchar>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;
namespace po = snu::apps::po;

std::string ResolveGameDir(const std::string& cli_game_dir) {
    if (!cli_game_dir.empty()) {
        return cli_game_dir;
    }
    const wchar_t* env = _wgetenv(L"VN_GAME_DIR");
    if (env != nullptr && env[0] != L'\0') {
        return snu::apps::wide_to_utf8(env);
    }
    return {};
}

}  // namespace

int main() {
    try {
        snu::setup_console_utf8();

        std::string project_dir;
        std::string game_dir_arg;
        std::string out_patch_arg;
        std::string contact_sheets_arg;
        std::string previews_arg;

        po::options_description desc(
            "04_patch_menus -- build keyless XOR-delta menu_patch.bin");
        desc.add(snu::apps::project_only_options(project_dir));
        desc.add_options()
            ("game-dir", po::value<std::string>(&game_dir_arg),
             "game installation folder (or set VN_GAME_DIR)")
            ("out", po::value<std::string>(&out_patch_arg),
             "output path for menu_patch.bin (defaults to <dir>/menu_patch.bin)")
            ("contact-sheets", po::value<std::string>(&contact_sheets_arg),
             "optional directory to write Original | Patched | Diff contact sheets")
            ("previews", po::value<std::string>(&previews_arg),
             "optional directory to write 1280x720 localized screen previews");

        po::variables_map vm;
        if (const auto rc = snu::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::string game_dir = ResolveGameDir(game_dir_arg);
        if (game_dir.empty()) {
            std::cerr << "[ERROR] --game-dir (or VN_GAME_DIR) is required\n\n"
                      << desc << std::endl;
            return snu::apps::kExitFailure;
        }

        snu::menu::MenuPatchBuildOptions opts;
        if (!project_dir.empty()) {
            opts.project_dir = fs::u8path(project_dir);
        }
        opts.game_dir = fs::u8path(game_dir);
        if (!out_patch_arg.empty()) {
            opts.output_patch_path = fs::u8path(out_patch_arg);
        } else if (!project_dir.empty()) {
            opts.output_patch_path = fs::u8path(project_dir) / "menu_patch.bin";
        } else {
            std::cerr << "[ERROR] --out or --dir is required to determine menu_patch.bin output path\n";
            return snu::apps::kExitFailure;
        }

        if (!contact_sheets_arg.empty()) {
            opts.contact_sheets_dir = fs::u8path(contact_sheets_arg);
        }
        if (!previews_arg.empty()) {
            opts.previews_dir = fs::u8path(previews_arg);
        }

        const snu::menu::MenuPatchBuildSummary summary =
            snu::menu::BuildMenuPatch(opts);

        std::ostringstream hash_hex;
        hash_hex << "0x" << std::hex << std::uppercase << std::setw(8)
                 << std::setfill('0') << summary.manifest_hash;
        snu::log_info("Built " + opts.output_patch_path.u8string() + " (" +
                      std::to_string(summary.total_records) + " records [splash=" +
                      std::to_string(summary.splash_records) + ", sysgrp=" +
                      std::to_string(summary.sysgrp_records) + ", choices=" +
                      std::to_string(summary.choice_records) + "], " +
                      std::to_string(summary.patch_bytes) +
                      " bytes, manifest_hash=" + hash_hex.str() + ")");
        if (!summary.contact_sheet_files.empty()) {
            snu::log_info("Wrote " +
                          std::to_string(summary.contact_sheet_files.size()) +
                          " contact sheet(s) to " +
                          opts.contact_sheets_dir.u8string());
        }
        if (!summary.preview_files.empty()) {
            snu::log_info("Wrote " +
                          std::to_string(summary.preview_files.size()) +
                          " preview mockup(s) to " +
                          opts.previews_dir.u8string());
        }
        return snu::apps::kExitOk;
    } catch (const std::exception& e) {
        snu::log_info(std::string("[ERROR] ") + e.what());
        return snu::apps::kExitFailure;
    }
}
