// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_build_tsv -- speaker gate, cached translation lookup, then build
// translated_text.json and translations.tsv.
#include <exception>
#include <filesystem>
#include <string>

#include "build_tsv/box_budget.h"
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "cache/cache_core.h"

#include "apps/paths.h"

namespace fs = std::filesystem;
using namespace snu;

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = apps::po;

    std::string project, game, only, out_tsv;
    int batch = 0, test_n = 0, max_batches = 0;
    bool retranslate = false, include_untranslated = false, box_audit = false;
    bool rebuild_table = false, discard_cache = false;
    po::options_description desc(
        "02_build_tsv -- speaker gate, apply cached translations, build translations.tsv");
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
        ("box-audit", po::bool_switch(&box_audit),
         "measure the corpus against the boxes it is drawn in and stop; no "
         "API request is made")
        ("rebuild-table", po::bool_switch(&rebuild_table),
         "rebuild translations.tsv from the translated document already on "
         "disk and stop; no API request is made")
        ("discard-cache", po::bool_switch(&discard_cache),
         "allow --test or --retranslate to delete a cache that already holds "
         "paid work");
    po::variables_map vm;
    if (auto rc = apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    // Read-only modes: no API cost, run before anything destructive.
    if (box_audit || rebuild_table) {
        const std::string doc = project + "\\script_output\\translated_text.json";
        const std::string tsv =
            out_tsv.empty() ? project + "\\translations.tsv" : out_tsv;
        if (!fs::exists(fs::u8path(doc))) {
            log_info("[ERROR] no translated document at " + doc);
            return 2;
        }
        // Rebuild the shipped table without re-buying the translations.
        if (rebuild_table) return build_tsv::run_build(doc, tsv, include_untranslated);
        const auto runtime = build_tsv::load_runtime_table(tsv);
        build_tsv::report_box_budget_audit(
            doc, build_tsv::measurement_table(doc, runtime));
        build_tsv::report_rect_array_audit(runtime);
        return 0;
    }

    // Validate early so a mistyped cap costs nothing.
    if (auto why = translate::validate_options(batch, test_n, max_batches)) {
        log_info("[ERROR] " + *why);
        return 2;
    }

    std::string out_dir = project + "\\script_output";
    const std::string cache_file = out_dir + "\\translation_cache_anthropic.json";

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
    // --out defaults off --dir, so it cannot be a static default_value.
    opt.tsv_file = out_tsv.empty() ? project + "\\translations.tsv" : out_tsv;
    opt.batch_size = batch;
    opt.retranslate = retranslate;
    opt.include_untranslated = include_untranslated;
    if (!only.empty()) opt.only_script = only;
    // Both cap API requests; --test also clears the cache, so it wins.
    const int budget = test_n > 0 ? test_n : max_batches;
    opt.test_mode = budget > 0;
    opt.num_batches = budget > 0 ? budget : 1;
    // --test is a fresh smoke run (cache cleared); --max-batches keeps it.
    opt.fresh_run = test_n > 0;

    try {
        return translate::translate_all(opt);
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
