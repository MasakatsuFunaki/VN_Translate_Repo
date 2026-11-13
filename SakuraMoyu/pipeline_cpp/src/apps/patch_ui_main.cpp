// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "apps/paths.h"
#include "common/util.h"
#include "ui/ui_patcher.h"

#include <exception>
#include <string>

int main() {
    fav::setup_console_utf8();
    namespace po = fav::apps::po;

    try {
        std::string game;
        po::options_description desc(
            "03_patch_ui -- patch Options & System UI textures in graph.bin");
        desc.add(fav::apps::game_only_options(game));

        po::variables_map vm;
        if (const auto rc = fav::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        fav::ui::PatchUiOptions opts;
        opts.game_dir = game;
        return fav::ui::patch_ui_archive(opts);
    } catch (const std::exception& e) {
        fav::log_info(std::string("[ERROR] ") + e.what());
        return fav::apps::kExitError;
    }
}
