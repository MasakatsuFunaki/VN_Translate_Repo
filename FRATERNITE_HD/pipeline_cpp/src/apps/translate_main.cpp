// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_translate -- speaker gate, translate via Claude, build translation_table.tsv.
#include <exception>
#include <filesystem>
#include <string>

#include "common/util.h"
#include "translate/translate_core.h"

#include "apps/paths.h"

namespace fs = std::filesystem;
using namespace frat;

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = apps::po;

    std::string project, game, only, out_tsv;
    int batch = 0, test_n = 0, max_batches = 0;
    bool retranslate = false, discard_cache = false;
    po::options_description desc(
        "02_translate -- speaker gate, translate via Claude, build translation_table.tsv");
    desc.add(apps::common_options(project, game));
    desc.add_options()
        ("batch", po::value(&batch)->default_value(150), "lines per API request")
        ("retranslate", po::bool_switch(&retranslate), "ignore cache, retranslate everything")
        ("file", po::value(&only), "translate only this archive key")
        ("test", po::value(&test_n)->implicit_value(1)->default_value(0),
         "smoke run: N batches then stop, from a wiped cache (bare --test = 1)")
        ("max-batches", po::value(&max_batches)->default_value(0),
         "stop after N API requests, keeping the cache and outputs (0 = no limit)")
        ("out", po::value(&out_tsv),
         "translation table path (default <game-dir>\\translation_table.tsv)")
        ("discard-cache", po::bool_switch(&discard_cache),
         "allow --test or --retranslate to delete a cache that already holds "
         "paid work");
    po::variables_map vm;
    if (auto rc = apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    // Fail-fast: a mistyped cap must cost nothing.
    if (auto why = translate::validate_options(batch, test_n, max_batches)) {
        log_info("[ERROR] " + *why);
        return 2;
    }

    const std::string out_dir = project + "\\script_output";
    const std::string cache_file = out_dir + "\\translation_cache_anthropic.json";

    if (test_n > 0 || retranslate) {
        // An unparseable cache is a refusal, not a crash.
        try {
            if (auto why = translate::refuse_cache_discard(
                    translate::cache_entry_count(cache_file),
                    test_n > 0 ? "--test" : "--retranslate", discard_cache)) {
                log_info("[ERROR] " + *why);
                return 2;
            }
        } catch (const std::exception& e) {
            log_info(std::string("[ERROR] ") + e.what());
            return 2;
        }
    }

    fs::create_directories(fs::u8path(out_dir));

    translate::TranslateOptions opt;
    opt.input_file = out_dir + "\\extracted_text.json";
    opt.cache_file = cache_file;
    opt.output_file = out_dir + "\\translated_text.json";
    // --out defaults off --game-dir; cannot be a static default_value.
    opt.tsv_file = out_tsv.empty() ? game + "\\translation_table.tsv" : out_tsv;
    opt.test_dir = project + "\\test";
    opt.batch_size = batch;
    opt.retranslate = retranslate;
    if (!only.empty()) opt.only_file = only;
    // --test wins over --max-batches when both are given.
    opt.test_batches = test_n > 0 ? test_n : max_batches;
    opt.test_mode = test_n > 0;
    // --test wipes cache+output so the table reflects only this run.
    opt.fresh_run = test_n > 0;

    try {
        return translate::translate_all(opt);
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
