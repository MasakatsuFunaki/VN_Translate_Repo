// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_build_tsv -- speaker gate, apply cached translations, build translation_table.tsv.
#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "common/util.h"

#include <exception>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace frat;

namespace {

constexpr int kRefusedExitCode = 2;

}  // namespace

int main() {
    setup_console_utf8();
    namespace po = apps::po;

    try {
        std::string project;
        std::string game;
        std::string out_tsv;
        bool rebuild_table = false;
        po::options_description desc(
            "02_build_tsv -- speaker gate, apply cached translations, build translation_table.tsv");
        desc.add(apps::common_options(project, game));
        desc.add_options()
            ("out", po::value(&out_tsv),
             "translation table path (default <game-dir>\\translation_table.tsv)")
            ("rebuild-table", po::bool_switch(&rebuild_table),
             "rebuild translation_table.tsv from the translated document already on "
             "disk and stop");
        po::variables_map vm;
        if (auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::string out_dir = project + "\\script_output";
        const std::string doc = out_dir + "\\translated_text.json";
        // --out defaults off --game-dir; cannot be a static default_value.
        const std::string tsv =
            out_tsv.empty() ? game + "\\translation_table.tsv" : out_tsv;

        if (rebuild_table) {
            if (!fs::exists(fs::u8path(doc))) {
                log_info("[ERROR] no translated document at " + doc);
                return kRefusedExitCode;
            }
            return build_tsv::run_build(doc, tsv);
        }

        fs::create_directories(fs::u8path(out_dir));

        translate::TranslateOptions opt;
        opt.input_file = out_dir + "\\extracted_text.json";
        opt.cache_file = out_dir + "\\translation_cache_anthropic.json";
        opt.output_file = doc;
        opt.tsv_file = tsv;

        return translate::translate_all(opt);
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return apps::kExitError;
    }
}
