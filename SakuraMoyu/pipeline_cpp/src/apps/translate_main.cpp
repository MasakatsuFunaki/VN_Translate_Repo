// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_translate -- the whole translation stage: the cast-roster gate, the
// batched translation via Gemini, translated_text.json, and the
// translation_table.tsv the game loads.
//
// Usage:
//   02_translate --game-dir <dir>
//   02_translate --game-dir <dir> --batch 50
//   02_translate --game-dir <dir> --max-batches 4   # four requests, keeping cache and outputs
//   02_translate --game-dir <dir> --rebuild-table   # rebuild TSV from translated_text.json
//   02_translate --game-dir <dir> --retranslate     # ignore cache
#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "translate/translate_core.h"

#include <exception>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace fav;

namespace {

constexpr int kDefaultBatchArg = 100;
constexpr int kNoBatchLimit = 0;
constexpr int kNoTestBatches = 0;
constexpr int kDefaultBatchCount = 1;
constexpr int kRefusedExitCode = 2;

}  // namespace

int main() {
    setup_console_utf8();
    namespace po = apps::po;

    try {
        std::string project;
        std::string game;
        std::string out_tsv;
        int batch = 0;
        int max_batches = 0;
        bool retranslate = false;
        bool rebuild_table = false;
        bool discard_cache = false;
        po::options_description desc(
            "02_translate -- cast gate, translate via Gemini, build translation_table.tsv");
        desc.add(apps::common_options(project, game));
        desc.add_options()
            ("batch", po::value(&batch)->default_value(kDefaultBatchArg), "lines per API request")
            ("retranslate", po::bool_switch(&retranslate), "ignore cache, retranslate everything")
            ("max-batches", po::value(&max_batches)->default_value(kNoBatchLimit),
             "stop after N API requests, keeping the cache and outputs (0 = no limit)")
            ("out", po::value(&out_tsv),
             "translation table path (default <game-dir>\\translation_table.tsv)")
            ("rebuild-table", po::bool_switch(&rebuild_table),
             "rebuild translation_table.tsv from the translated document already on "
             "disk and stop; no API request is made")
            ("discard-cache", po::bool_switch(&discard_cache),
             "allow --retranslate to delete a cache that already holds paid work");
        po::variables_map vm;
        if (const auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        if (rebuild_table) {
            const std::string doc = project + "\\script_output\\translated_text.json";
            const std::string cache = project + "\\script_output\\translation_cache_gemini.json";
            const std::string tsv =
                out_tsv.empty() ? game + "\\translation_table.tsv" : out_tsv;
            if (!fs::exists(fs::u8path(doc))) {
                log_info("[ERROR] no translated document at " + doc);
                return kRefusedExitCode;
            }
            return build_tsv::run_build(doc, cache, tsv);
        }

        // Validate before anything is created, deleted, or authenticated: a
        // mistyped cap must cost nothing.
        if (const auto why = translate::validate_options(batch, kNoTestBatches, max_batches)) {
            log_info("[ERROR] " + *why);
            return kRefusedExitCode;
        }

        const std::string out_dir = project + "\\script_output";
        const std::string cache_file =
            out_dir + "\\translation_cache_gemini.json";

        if (retranslate) {
            // Counting the cache parses it, and a cache that cannot be parsed
            // throws.  Handled here because this guard runs before the banner:
            // an escaping throw would end the run with no output at all.  A
            // cache nobody could read is a refusal like any other, so nothing
            // is deleted and the file is left as it is.
            try {
                if (const auto why = translate::refuse_cache_discard(
                        translate::cache_entry_count(cache_file),
                        "--retranslate", discard_cache)) {
                    log_info("[ERROR] " + *why);
                    return kRefusedExitCode;
                }
            } catch (const std::exception& e) {
                log_info(std::string("[ERROR] ") + e.what());
                return kRefusedExitCode;
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
        opt.test_mode = max_batches > kNoBatchLimit;
        opt.num_batches = max_batches > kNoBatchLimit ? max_batches : kDefaultBatchCount;
        opt.fresh_run = false;

        return translate::translate_all(opt);
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return apps::kExitError;
    }
}
