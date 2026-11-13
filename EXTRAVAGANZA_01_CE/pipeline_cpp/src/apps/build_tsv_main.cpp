// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_build_tsv -- speaker gate and cached assembly of SPT translations into
// script_output/translated_text.json.
#include "apps/paths.h"
#include "cache/cache_core.h"
#include "common/util.h"

#include <exception>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace exc;

int main() {
    setup_console_utf8();
    namespace po = apps::po;

    try {
        std::string project;
        po::options_description desc(
            "02_build_tsv -- speaker gate, then assemble translated_text.json");
        desc.add(apps::project_only_options(project));
        po::variables_map vm;
        if (const auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const fs::path project_root = fs::u8path(project);
        const fs::path out_dir = project_root / "script_output";
        fs::create_directories(out_dir);

        translate::TranslateOptions opt;
        opt.input_file = (out_dir / "extracted_text.json").u8string();
        opt.cache_file = (out_dir / "translation_cache_anthropic.json").u8string();
        opt.output_file = (out_dir / "translated_text.json").u8string();
        opt.test_dir = (project_root / "test").u8string();

        return translate::translate_all(opt);
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return apps::kExitError;
    }
}
