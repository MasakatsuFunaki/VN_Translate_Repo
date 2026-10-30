// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_translate -- the whole translation step: the speaker gate, the batched
// translation via Google Gemini, translated_text.json, and the
// translations.tsv the game loads.
//
// Usage:
//   02_translate
//   02_translate --gate-only
//   02_translate --rebuild-table
//   02_translate --batch 60 --test
//   02_translate --script script\01_01.ast --max-batches 1
//   02_translate --retranslate
#include <exception>
#include <filesystem>
#include <string>

#include "build_tsv/build_tsv.h"
#include "common/coverage.h"
#include "common/util.h"
#include "translate/translate_core.h"

#include "apps/paths.h"

namespace fs = std::filesystem;
using namespace snt;

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = apps::po;

    std::string project, game, only, out_tsv;
    int batch = 0, test_n = 0, max_batches = 0;
    bool retranslate = false, discard_cache = false, include_untranslated = false;
    bool gate_only = false, rebuild_table = false, flow_breaks = false;
    po::options_description desc(
        "02_translate -- speaker gate, translate via Gemini, build translations.tsv");
    desc.add(apps::common_options(project, game));
    desc.add_options()
        ("batch", po::value(&batch)->default_value(150), "strings per API request")
        ("script", po::value(&only), "translate only this script")
        ("retranslate", po::bool_switch(&retranslate), "ignore cached translations")
        ("test", po::value(&test_n)->implicit_value(1)->default_value(0),
         "smoke run: N batches then stop (bare --test = 1)")
        ("max-batches", po::value(&max_batches)->default_value(0),
         "stop after N API requests, keeping the cache and outputs (0 = no limit)")
        ("out", po::value(&out_tsv), "translation table path (default <dir>\\translations.tsv)")
        ("include-untranslated", po::bool_switch(&include_untranslated),
         "emit table rows whose translation is still the raw Japanese")
        ("gate-only", po::bool_switch(&gate_only),
         "run the speaker gate over the extraction and exit 0 or 2; reads no "
         "API key, sends no request, writes no file")
        ("rebuild-table", po::bool_switch(&rebuild_table),
         "rebuild translations.tsv and the coverage report from the translated "
         "document already on disk and stop; reads no API key and sends no request")
        ("flow-breaks", po::bool_switch(&flow_breaks),
         "turn a line break inside a table value into a space and let the "
         "engine wrap it; the Japanese carries no break, so this acts only on "
         "breaks the English answer introduced")
        ("discard-cache", po::bool_switch(&discard_cache),
         "allow --test or --retranslate to delete a cache that already "
         "holds paid work");
    po::variables_map vm;
    if (auto rc = apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    std::string out_dir = project + "\\script_output";
    const std::string cache_file =
        out_dir + "\\translation_cache_gemini.json";

    // Read-only mode, above everything that could spend or delete: the table
    // and the coverage report are both derived from the document on disk, so
    // rebuilding them costs nothing and needs no API key.
    if (rebuild_table) {
        const std::string doc = out_dir + "\\translated_text.json";
        if (!fs::exists(fs::u8path(doc))) {
            log_info("[ERROR] no translated document at " + doc);
            return 2;
        }
        const std::string tsv =
            out_tsv.empty() ? project + "\\translations.tsv" : out_tsv;
        try {
            const int rc = build_tsv::run_build(doc, cache_file, tsv,
                                                include_untranslated, flow_breaks);
            print_line("");
            report_coverage(json_parse_file(doc).get_object(), doc);
            return rc;
        } catch (const std::exception& e) {
            log_info(std::string("[ERROR] ") + e.what());
            return 1;
        }
    }

    // Validate before anything is created, deleted, or authenticated: a
    // mistyped cap must cost nothing.
    if (auto why = translate::validate_options(batch, test_n, max_batches)) {
        log_info("[ERROR] " + *why);
        return 2;
    }

    // Both discard the cache; guard runs early so a refusal costs nothing.
    // --gate-only deletes nothing, so it is not held to the guard even when it
    // is passed alongside one of those flags.
    if (!gate_only && (test_n > 0 || retranslate)) {
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

    if (!gate_only) fs::create_directories(fs::u8path(out_dir));

    translate::TranslateOptions opt;
    opt.input_file = out_dir + "\\extracted_text.json";
    opt.cache_file = cache_file;
    opt.output_file = out_dir + "\\translated_text.json";
    // --out defaults off --dir, so it cannot be a static default_value.
    opt.tsv_file = out_tsv.empty() ? project + "\\translations.tsv" : out_tsv;
    opt.batch_size = batch;
    opt.retranslate = retranslate;
    opt.include_untranslated = include_untranslated;
    opt.flow_breaks = flow_breaks;
    if (!only.empty()) opt.only_script = only;
    // Both `--test N` and `--max-batches N` cap the number of API requests;
    // they differ only in what they do to the files on disk, so `--test` wins
    // when someone passes both.
    const int budget = test_n > 0 ? test_n : max_batches;
    opt.test_mode = budget > 0;
    opt.num_batches = budget > 0 ? budget : 1;
    // `--test N` means a fresh smoke run: the cache and the per-line document
    // go, so the table reflects exactly THIS run's output.  A full run keeps
    // both so progress is resume-safe, and so does `--max-batches`: capping
    // the work must never cost work already paid for.  The wipe happens inside
    // the step, after the gate, so a refused run leaves both files alone.
    opt.fresh_run = test_n > 0 && !gate_only;
    opt.gate_only = gate_only;

    try {
        return translate::translate_all(opt);
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
