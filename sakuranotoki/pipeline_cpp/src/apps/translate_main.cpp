// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_translate -- speaker gate, batched translation via Google Gemini,
// translated_text.json, and the translations.tsv the game loads.
//
// Usage:
//   02_translate
//   02_translate --rebuild-table
//   02_translate --batch 60 --max-batches 1
//   02_translate --retranslate
#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "common/coverage.h"
#include "common/util.h"
#include "translate/translate_core.h"

#include <exception>
#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;

constexpr int kDefaultBatchSize = 150;
constexpr int kUnlimitedBatches = 0;
constexpr int kUnusedTestBatches = 0;
constexpr int kDefaultSingleBatch = 1;
constexpr bool kIncludeUntranslated = false;
constexpr bool kFlowBreaks = false;

}  // namespace

int main() {
    using namespace snt;
    setup_console_utf8();
    try {
        namespace po = apps::po;

        std::string project;
        std::string out_tsv;
        int batch = kDefaultBatchSize;
        int max_batches = kUnlimitedBatches;
        bool retranslate = false;
        bool discard_cache = false;
        bool rebuild_table = false;
        po::options_description desc(
            "02_translate -- speaker gate, translate via Gemini, build translations.tsv");
        desc.add(apps::project_only_options(project));
        desc.add_options()
            ("batch", po::value(&batch)->default_value(kDefaultBatchSize),
             "strings per API request")
            ("retranslate", po::bool_switch(&retranslate), "ignore cached translations")
            ("max-batches", po::value(&max_batches)->default_value(kUnlimitedBatches),
             "stop after N API requests, keeping the cache and outputs (0 = no limit)")
            ("out", po::value(&out_tsv),
             "translation table path (default <dir>\\build\\install\\game\\translations.tsv)")
            ("rebuild-table", po::bool_switch(&rebuild_table),
             "rebuild translations.tsv and the coverage report from the translated "
             "document already on disk and stop; reads no API key and sends no request")
            ("discard-cache", po::bool_switch(&discard_cache),
             "allow --retranslate to delete a cache that already holds paid work");
        po::variables_map vm;
        if (const auto rc =
                apps::parse_process_project_only_command_line(desc, vm)) {
            return *rc;
        }

        const std::string out_dir = project + "\\script_output";
        const std::string cache_file =
            out_dir + "\\translation_cache_gemini.json";

        if (rebuild_table) {
            const std::string doc = out_dir + "\\translated_text.json";
            if (!fs::exists(fs::u8path(doc))) {
                log_info("[ERROR] no translated document at " + doc);
                return apps::kRefusedExitCode;
            }
            const std::string tsv =
                out_tsv.empty()
                    ? project + "\\build\\install\\game\\translations.tsv"
                    : out_tsv;
            const int rc = build_tsv::run_build(
                doc, cache_file, tsv, kIncludeUntranslated, kFlowBreaks);
            print_line("");
            report_coverage(json_parse_file(doc).get_object(), doc);
            return rc;
        }

        if (const auto why =
                translate::validate_options(batch, kUnusedTestBatches, max_batches)) {
            log_info("[ERROR] " + *why);
            return apps::kRefusedExitCode;
        }

        if (retranslate) {
            try {
                if (const auto why = translate::refuse_cache_discard(
                        translate::cache_entry_count(cache_file),
                        "--retranslate", discard_cache)) {
                    log_info("[ERROR] " + *why);
                    return apps::kRefusedExitCode;
                }
            } catch (const std::exception& e) {
                log_info(std::string("[ERROR] ") + e.what());
                return apps::kRefusedExitCode;
            }
        }

        fs::create_directories(fs::u8path(out_dir));

        translate::TranslateOptions opt;
        opt.input_file = out_dir + "\\extracted_text.json";
        opt.cache_file = cache_file;
        opt.output_file = out_dir + "\\translated_text.json";
        opt.tsv_file =
            out_tsv.empty()
                ? project + "\\build\\install\\game\\translations.tsv"
                : out_tsv;
        opt.batch_size = batch;
        opt.retranslate = retranslate;
        opt.test_mode = max_batches > 0;
        opt.num_batches = max_batches > 0 ? max_batches : kDefaultSingleBatch;

        return translate::translate_all(opt);
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return apps::kExitFailure;
    }
}
