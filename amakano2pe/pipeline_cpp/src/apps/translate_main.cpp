// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_translate -- the whole translation step: the speaker gate, the batched
// translation of scene.int text via Anthropic Claude, translated_text.json,
// and the translation_table.tsv the game loads.
//
// Usage:
//   02_translate
//   02_translate --batch 50
//   02_translate --batch 50 --test        # 1 batch only
//   02_translate --batch 50 --test 4      # 4 batches
//   02_translate --script 01_TGHGDD_01_k.Hqo
#include <exception>
#include <filesystem>
#include <string>

#include "common/util.h"
#include "translate/translate_core.h"

#include "apps/paths.h"

namespace fs = std::filesystem;
using namespace ama;

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = apps::po;

    std::string project, game, only, out_tsv;
    int batch = 0, test_n = 0;
    bool retranslate = false, discard_cache = false;
    po::options_description desc(
        "02_translate -- speaker gate, translate via Claude, build translation_table.tsv");
    desc.add(apps::common_options(project, game));
    desc.add_options()
        ("batch", po::value(&batch)->default_value(150), "lines per API request")
        ("retranslate", po::bool_switch(&retranslate), "ignore cache, retranslate everything")
        ("script", po::value(&only), "translate only this specific script")
        ("test", po::value(&test_n)->implicit_value(1)->default_value(0),
         "run only N API batches then stop (bare --test = 1)")
        ("out", po::value(&out_tsv),
         "translation table path (default <game-dir>\\translation_table.tsv)")
        ("discard-cache", po::bool_switch(&discard_cache),
         "allow --test or --retranslate to delete a cache that already holds "
         "paid work");
    po::variables_map vm;
    if (auto rc = apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    // Validate before anything is created, deleted, or authenticated: a
    // mistyped cap must cost nothing.
    if (auto why = translate::validate_options(batch, test_n)) {
        log_info("[ERROR] " + *why);
        return 2;
    }

    const std::string out_dir = project + "\\script_output";
    const std::string cache_file =
        out_dir + "\\translation_cache_anthropic.json";

    // Both discard the cache; guard runs early so a refusal costs nothing.
    if (test_n > 0 || retranslate) {
        // Counting the cache parses it, and a cache that cannot be parsed
        // throws.  Handled here because this guard runs before the banner: an
        // escaping throw would end the run with no output at all.  A cache
        // nobody could read is a refusal like any other, so nothing is
        // deleted and the file is left as it is.
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
    opt.last_translate_dir = out_dir + "\\last_anthropic_translate";
    // --out defaults off --game-dir, so it cannot be a static default_value.
    // The table is written straight into the game folder: the runtime hook
    // reads it from there, and nothing else in this tree consumes it.
    opt.tsv_file = out_tsv.empty() ? game + "\\translation_table.tsv" : out_tsv;
    opt.batch_size = batch;
    opt.retranslate = retranslate;
    if (!only.empty()) opt.only_script = only;
    opt.test_mode = test_n > 0;
    opt.num_batches = test_n > 0 ? test_n : 1;
    // `--test N` means a fresh smoke run: the cache and the per-line document
    // go, so every batch is translated from scratch and the resulting table
    // reflects exactly THIS run's output.  A full run keeps both files so
    // progress is resume-safe across crashes / Ctrl+C.  The removal itself
    // happens inside the step, after the gate has passed.
    opt.fresh_run = test_n > 0;

    try {
        return translate::translate_all(opt);
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
