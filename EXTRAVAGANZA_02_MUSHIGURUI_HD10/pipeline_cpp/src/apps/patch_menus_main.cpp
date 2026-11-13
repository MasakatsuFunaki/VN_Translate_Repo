// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 06_patch_menus -- build `menu_patch.bin` ("MSGMENU1" v1) from pristine
// HD10 and Complete Edition game directories.
//
//   06_patch_menus --game-dir <hd10_dir> --ce-game-dir <ce_dir> [--output <path>]
#include "apps/paths.h"
#include "common/util.h"
#include "menu/menu_patcher.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kDefaultPatchBinFileName = "menu_patch.bin";

}  // namespace

int main() {
    namespace po = mgi::apps::po;

    try {
        mgi::setup_console_utf8();

        std::string hd10_game_dir;
        std::string ce_game_dir;
        std::string output_path;

        auto* out_val = po::value(&output_path);
        if (const std::string proj = mgi::apps::default_project_dir(); proj.empty()) {
            out_val->required();
        } else {
            out_val->default_value(
                (fs::u8path(proj) / fs::u8path(kDefaultPatchBinFileName)).u8string());
        }

        po::options_description desc(
            "06_patch_menus -- build English menu and UI patch bundle (menu_patch.bin)");
        desc.add_options()
            ("help,h", "show this help and exit")
            ("game-dir", po::value(&hd10_game_dir)->required(),
             "pristine mushigurui_HD10 game installation folder")
            ("ce-game-dir", po::value(&ce_game_dir)->required(),
             "pristine mushigurui Complete Edition game installation folder")
            ("output", out_val,
             "output path for menu_patch.bin");

        po::variables_map vm;
        if (const auto rc = mgi::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        mgi::menu::MenuPatchOptions opts;
        opts.hd10_game_dir = fs::u8path(hd10_game_dir);
        opts.ce_game_dir = fs::u8path(ce_game_dir);
        opts.output_patch_bin = fs::u8path(output_path);

        mgi::menu::MenuPatchBuilder builder(std::move(opts));
        const auto summary = builder.build();

        std::cout << "Built " << summary.output_patch_bin.u8string() << " ("
                  << mgi::comma(static_cast<long long>(summary.patch_bin_bytes))
                  << " bytes): " << summary.standalone_group_count
                  << " standalone groups (" << summary.standalone_variant_count
                  << " variants), " << summary.archive_count << " archives ("
                  << summary.patched_entry_count << " patched GPK entries).\n";
        return mgi::apps::kExitOk;
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return mgi::apps::kExitError;
    }
}
