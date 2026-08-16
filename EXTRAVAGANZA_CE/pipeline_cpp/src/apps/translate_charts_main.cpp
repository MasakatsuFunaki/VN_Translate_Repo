// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 05_translate_charts -- translate the chart/*.fxf flowchart files.
//
//   05_translate_charts                # translate + repack
//   05_translate_charts --test         # dry run: list the texts, no API calls
//   05_translate_charts --retranslate  # ignore the cache
#include <exception>
#include <filesystem>
#include <string>

#include "charts/charts.h"
#include "common/util.h"
#include "translate/anthropic_client.h"
#include "translate/translate_core.h"

#include "apps/paths.h"

int main(int argc, char** argv) {
    exc::setup_console_utf8();
    namespace po = exc::apps::po;

    std::string project, game;
    bool test_mode = false, retranslate = false, discard_cache = false;
    po::options_description desc("05_translate_charts -- translate flowchart files");
    desc.add(exc::apps::common_options(project, game));
    desc.add_options()
        ("test", po::bool_switch(&test_mode), "dry run, show texts only")
        ("retranslate", po::bool_switch(&retranslate), "ignore cache")
        ("discard-cache", po::bool_switch(&discard_cache),
         "allow --retranslate to discard a chart cache that already holds "
         "translations");
    po::variables_map vm;
    if (auto rc = exc::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    const std::string out_dir = project + "\\script_output";
    const std::string cache_file = out_dir + "\\chart_translation_cache.json";

    // --retranslate throws the chart cache away, so the guard runs before the
    // directory is created and before a key is read: a refusal costs nothing.
    if (retranslate) {
        // Counting the cache parses it, and a cache that cannot be parsed
        // throws.  A cache nobody could read is a refusal like any other, so
        // nothing is discarded and the file is left as it is.
        try {
            if (auto why = exc::translate::refuse_cache_discard(
                    exc::translate::cache_entry_count(cache_file), "--retranslate",
                    discard_cache)) {
                exc::log_info("[ERROR] " + *why);
                return 2;
            }
        } catch (const std::exception& e) {
            exc::log_info(std::string("[ERROR] ") + e.what());
            return 2;
        }
    }

    std::filesystem::create_directories(std::filesystem::u8path(out_dir));

    exc::charts::ChartOptions opt;
    opt.chart_dir = game + "\\chart";
    opt.backup_dir = game + "\\chart_backup";
    opt.cache_file = cache_file;
    opt.test_mode = test_mode;
    opt.retranslate = retranslate;

    exc::anthropic::load_api_key();
    try {
        return exc::charts::run_translate_charts(opt);
    } catch (const std::exception& e) {
        exc::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
