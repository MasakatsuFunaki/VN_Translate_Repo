// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_repack -- write the translated text back into the .spt script files.
//
//   03_repack              # patch spt/ from spt_backup/ + translated_text.json
//   03_repack --restore    # copy spt_backup/ back over spt/
#include <string>

#include "common/util.h"
#include "repack/repack.h"

#include "apps/paths.h"

int main(int argc, char** argv) {
    exc::setup_console_utf8();
    namespace po = exc::apps::po;

    std::string project, game;
    bool restore = false;
    po::options_description desc("03_repack -- repack translated text into SPT files");
    desc.add(exc::apps::common_options(project, game));
    desc.add_options()("restore", po::bool_switch(&restore),
                      "restore the original files from backup");
    po::variables_map vm;
    if (auto rc = exc::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    const std::string spt_dir = game + "\\spt";
    const std::string backup_dir = game + "\\spt_backup";
    try {
        if (restore) return exc::repack::run_restore(spt_dir, backup_dir);
        return exc::repack::run_repack(project + "\\script_output\\translated_text.json",
                                       spt_dir, backup_dir);
    } catch (const std::exception& e) {
        exc::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
