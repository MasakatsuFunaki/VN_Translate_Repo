// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_translate -- the whole translation step: the cast-roster gate, the
// batched translation via Gemini, translated_text.json, and the
// translation_table.tsv the game loads.
//
// Usage:
//   02_translate
//   02_translate --batch 50
//   02_translate --test            # one batch, fresh
//   02_translate --test 4          # four batches, fresh
//   02_translate --max-batches 4   # four batches, keeping cache and outputs
//   02_translate --retranslate     # ignore cache
#include <exception>
#include <filesystem>
#include <string>

#include "common/util.h"
#include "translate/translate_core.h"

#include "apps/paths.h"

namespace fs = std::filesystem;
using namespace fav;

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = apps::po;

    std::string project, game, out_tsv;
    int batch = 0, test_n = 0, max_batches = 0;
    bool retranslate = false, discard_cache = false;
    po::options_description desc(
        "02_translate -- cast gate, translate via Gemini, build translation_table.tsv");
    desc.add(apps::common_options(project, game));
    desc.add_options()
        ("batch", po::value(&batch)->default_value(100), "lines per API request")
        ("retranslate", po::bool_switch(&retranslate), "ignore cache, retranslate everything")
        ("test", po::value(&test_n)->implicit_value(1)->default_value(0),
         "smoke run: N batches then stop (bare --test = 1)")
        ("max-batches", po::value(&max_batches)->default_value(0),
         "stop after N API requests, keeping the cache and outputs (0 = no limit)")
        ("out", po::value(&out_tsv),
         "translation table path (default <game-dir>\\translation_table.tsv)")
        ("discard-cache", po::bool_switch(&discard_cache),
         "allow --test or --retranslate to delete a cache that already holds "
         "paid work");
    po::variables_map vm;
    if (auto rc = apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    // Validate before anything is created, deleted, or authenticated: a
    // mistyped cap must cost nothing.
    if (auto why = translate::validate_options(batch, test_n, max_batches)) {
        log_info("[ERROR] " + *why);
        return 2;
    }

    const std::string out_dir = project + "\\script_output";
    const std::string cache_file =
        out_dir + "\\translation_cache_gemini.json";

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
    opt.hcb_file = game + "\\Sakura.hcb";
    opt.input_file = out_dir + "\\extracted_text.json";
    opt.cache_file = cache_file;
    opt.output_file = out_dir + "\\translated_text.json";
    // --out defaults off --game-dir, so it cannot be a static default_value.
    opt.tsv_file = out_tsv.empty() ? game + "\\translation_table.tsv" : out_tsv;
    opt.last_translate_dir = out_dir + "\\last_gemini_translate";
    opt.batch_size = batch;
    opt.retranslate = retranslate;
    // Both `--test N` and `--max-batches N` cap the number of API requests;
    // they differ only in what they do to the files on disk, so `--test` wins
    // when someone passes both.
    const int budget = test_n > 0 ? test_n : max_batches;
    opt.test_mode = budget > 0;
    opt.num_batches = budget > 0 ? budget : 1;
    // `--test N` means a fresh smoke run: the cache and the per-line document
    // go, so the table reflects exactly THIS run's output.  A full run keeps
    // both so progress is resume-safe, and so does `--max-batches`: capping
    // the work must never cost work already paid for.
    opt.fresh_run = test_n > 0;

    try {
        return translate::translate_all(opt);
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
