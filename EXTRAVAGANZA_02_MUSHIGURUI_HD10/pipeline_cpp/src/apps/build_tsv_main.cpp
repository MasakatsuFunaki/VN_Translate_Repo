// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_build_tsv -- speaker gate, cached script translation assembly into
// translated_text.json, and generation of translation_table.tsv.
//
// Usage:
//   02_build_tsv --game-dir <dir>
//   02_build_tsv --rebuild-table --game-dir <dir> [--out <path>]
#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "common/util.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace mgi;

int main() {
    namespace po = apps::po;

    try {
        setup_console_utf8();

        std::string project;
        std::string game;
        std::string out_tsv;
        bool rebuild_table = false;
        po::options_description desc(
            "02_build_tsv -- speaker gate, assemble translated_text.json, build translation_table.tsv");
        desc.add(apps::common_options(project, game));
        desc.add_options()
            ("out", po::value(&out_tsv),
             "translation table path (default <game-dir>\\translation_table.tsv)")
            ("rebuild-table", po::bool_switch(&rebuild_table),
             "rebuild translation_table.tsv from the translated document already on "
             "disk and stop; no API request is made");
        po::variables_map vm;
        if (const auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const fs::path project_root = fs::u8path(project);
        const fs::path out_dir = project_root / "script_output";
        const std::string tsv_path = out_tsv.empty()
                                         ? (fs::u8path(game) / "translation_table.tsv").u8string()
                                         : out_tsv;

        if (rebuild_table) {
            const fs::path doc = out_dir / "translated_text.json";
            if (!fs::exists(doc)) {
                log_info("[ERROR] no translated document at " + doc.u8string());
                return apps::kExitRefused;
            }
            return build_tsv::run_build(doc.u8string(), tsv_path);
        }

        fs::create_directories(out_dir);

        translate::TranslateOptions opt;
        opt.input_file = (out_dir / "extracted_text.json").u8string();
        opt.cache_file = (out_dir / "translation_cache_anthropic.json").u8string();
        opt.output_file = (out_dir / "translated_text.json").u8string();
        opt.tsv_file = tsv_path;
        opt.test_dir = (project_root / "test").u8string();

        return translate::translate_all(opt);
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return apps::kExitError;
    }
}
