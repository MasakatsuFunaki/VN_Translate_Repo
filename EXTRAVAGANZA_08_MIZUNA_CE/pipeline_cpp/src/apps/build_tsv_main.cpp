// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// 02_build_tsv -- speaker gate, cached translation, translated_text.json,
// and translation_table.tsv generation for EXTRA_VA_MIZUNA_CE.
#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "cache/gemini_client.h"
#include "common/util.h"

#include <exception>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace exm;

namespace {

constexpr int kDefaultBatchArg = 150;
constexpr int kNoBatchLimit = 0;
constexpr int kNoTestBatches = 0;
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
            "02_build_tsv -- speaker gate, translate script, build translation_table.tsv");
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
             "allow --retranslate to discard a cache that already holds paid work");
        po::variables_map vm;
        if (const auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        if (rebuild_table) {
            const std::string doc = project + "\\script_output\\translated_text.json";
            const std::string tsv =
                out_tsv.empty() ? game + "\\translation_table.tsv" : out_tsv;
            if (!fs::exists(fs::u8path(doc))) {
                log_info("[ERROR] no translated document at " + doc);
                return kRefusedExitCode;
            }
            return build_tsv::run_build(doc, tsv);
        }

        if (const auto why = translate::validate_options(batch, kNoTestBatches, max_batches)) {
            log_info("[ERROR] " + *why);
            return kRefusedExitCode;
        }

        const std::string out_dir = project + "\\script_output";
        const std::string cache_file = out_dir + "\\translation_cache_gemini.json";

        if (retranslate) {
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

        gemini::load_api_key(project);
        fs::create_directories(fs::u8path(out_dir));

        translate::TranslateOptions opt;
        opt.input_file = out_dir + "\\extracted_text.json";
        opt.cache_file = cache_file;
        opt.output_file = out_dir + "\\translated_text.json";
        opt.test_dir = project + "\\test";
        opt.tsv_file = out_tsv.empty() ? game + "\\translation_table.tsv" : out_tsv;
        opt.batch_size = batch;
        opt.retranslate = retranslate;
        opt.test_batches = kNoTestBatches;
        opt.max_batches = max_batches;

        return translate::translate_all(opt);
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return apps::kExitError;
    }
}
