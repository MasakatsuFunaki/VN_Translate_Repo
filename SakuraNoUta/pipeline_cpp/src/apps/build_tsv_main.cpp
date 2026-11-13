// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_build_tsv -- speaker gate, cached translation lookup, then build
// translated_text.json and translations.tsv.
#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "common/util.h"

#include <exception>
#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;

constexpr bool kIncludeUntranslated = false;

}  // namespace

int main() {
    using namespace snu;
    try {
        setup_console_utf8();
        namespace po = apps::po;

        std::string project;
        std::string out_tsv;
        bool rebuild_table = false;
        po::options_description desc(
            "02_build_tsv -- speaker gate, apply cached translations, build translations.tsv");
        desc.add(apps::project_only_options(project));
        desc.add_options()
            ("out", po::value(&out_tsv),
             "translation table path (default <dir>\\build\\install\\game\\translations.tsv)")
            ("rebuild-table", po::bool_switch(&rebuild_table),
             "rebuild translations.tsv from the translated document already on "
             "disk and stop");
        po::variables_map vm;
        if (const auto rc = apps::parse_process_project_only_command_line(desc, vm)) {
            return *rc;
        }

        const std::string out_dir = project + "\\script_output";
        const std::string tsv =
            out_tsv.empty() ? project + "\\build\\install\\game\\translations.tsv" : out_tsv;

        if (rebuild_table) {
            const std::string doc = out_dir + "\\translated_text.json";
            if (!fs::exists(fs::u8path(doc))) {
                log_info("[ERROR] no translated document at " + doc);
                return apps::kRefusedExitCode;
            }
            return build_tsv::run_build(doc, tsv, kIncludeUntranslated);
        }

        fs::create_directories(fs::u8path(out_dir));

        translate::TranslateOptions opt;
        opt.input_file = out_dir + "\\extracted_text.json";
        opt.cache_file = out_dir + "\\translation_cache_anthropic.json";
        opt.output_file = out_dir + "\\translated_text.json";
        // --out defaults off --dir, so it cannot be a static default_value.
        opt.tsv_file = tsv;

        return translate::translate_all(opt);
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return apps::kExitFailure;
    }
}
