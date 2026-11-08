// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include <exception>
#include <string>

#include "apps/paths.h"
#include "common/util.h"
#include "ui/ui_patcher.h"

int main(int argc, char** argv) {
    fav::setup_console_utf8();
    namespace po = fav::apps::po;

    std::string project, game, preview_dir;
    po::options_description desc(
        "03_patch_ui -- patch Options & System UI textures in graph.bin");
    desc.add(fav::apps::common_options(project, game));
    desc.add_options()
        ("preview-dir", po::value(&preview_dir)->default_value(""),
         "optional directory to export patched PNG previews");

    po::variables_map vm;
    if (auto rc = fav::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    try {
        fav::ui::PatchUiOptions opts;
        opts.game_dir = game;
        opts.preview_dir = preview_dir;
        return fav::ui::patch_ui_archive(opts);
    } catch (const std::exception& e) {
        fav::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
