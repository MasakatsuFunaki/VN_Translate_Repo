// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_patch_menus -- repack pac/cgsys.ypf with translated menu PNG sprites.
#include "apps/paths.h"
#include "common/util.h"
#include "menu_patcher/menu_patcher.h"

#include <exception>
#include <string>

int main(int argc, char** argv) {
    frat::setup_console_utf8();
    namespace po = frat::apps::po;

    std::string project;
    std::string game;
    std::string assets_dir;

    po::options_description desc(
        "03_patch_menus -- patch pac/cgsys.ypf menu sprites for FRATERNITE_HD");
    desc.add(frat::apps::common_options(project, game));
    desc.add_options()(
        "assets-dir", po::value(&assets_dir),
        "translated menu assets directory (default: <dir>\\menu_assets)");

    po::variables_map vm;
    if (auto rc = frat::apps::parse_command_line(argc, argv, desc, vm)) {
        return *rc;
    }

    if (assets_dir.empty()) {
        assets_dir = project + "\\menu_assets";
    }

    try {
        frat::menu::PatchConfig cfg;
        cfg.game_dir = game;
        cfg.assets_dir = assets_dir;

        frat::menu::MenuArchivePatcher patcher;
        const frat::menu::PatchSummary summary = patcher.patch_archive(cfg);
        frat::log_info("Patched " + summary.archive_path + ": replaced " +
                       frat::comma(static_cast<long long>(summary.replaced_entries)) +
                       " / " +
                       frat::comma(static_cast<long long>(summary.total_entries)) +
                       " entries (" +
                       frat::comma(static_cast<long long>(summary.discovered_assets)) +
                       " assets discovered)");
        return 0;
    } catch (const std::exception& e) {
        frat::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
