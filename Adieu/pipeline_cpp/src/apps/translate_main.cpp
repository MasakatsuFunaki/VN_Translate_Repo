// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 02_translate -- batched translation via Google Gemini, translated_text.json,
// and the translation_table.tsv the game loads.
//
// Usage:
//   02_translate
//   02_translate --batch 40
//   02_translate --max-batches 4       # cap the run, keep the cache
//   02_translate --rebuild-table [--game-dir <GameDataDir>]
//   02_translate --retranslate --discard-cache
#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "translate/gemini_client.h"
#include "translate/translate_core.h"

#include <exception>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace adi;

namespace {

constexpr int kDefaultBatchSize = vn::llm::kDefaultBatchSize;
constexpr int kUnlimitedBatches = 0;
constexpr int kDisabledTestBatchCap = 0;

}  // namespace

int main() {
    setup_console_utf8();
    try {
        namespace po = apps::po;

        std::string project;
        std::string out_tsv;
        std::string game_dir;
        int batch = kDefaultBatchSize;
        int max_batches = kUnlimitedBatches;
        bool retranslate = false;
        bool discard_cache = false;
        bool rebuild_table = false;
        bool sync_install = false;
        po::options_description desc(
            "02_translate -- translate Adieu text via Gemini and build the runtime table");
        desc.add(apps::project_only_options(project));
        desc.add_options()
            ("batch", po::value(&batch)->default_value(kDefaultBatchSize),
             "Lines per API request (default: 40)")
            ("max-batches", po::value(&max_batches)->default_value(kUnlimitedBatches),
             "Stop after N API requests globally, keeping cache and outputs (0 = no limit)")
            ("out", po::value(&out_tsv),
             "Translation table output path (default: <dir>\\translation_table.tsv)")
            ("game-dir", po::value(&game_dir),
             "Game installation folder (GameData), copied when --rebuild-table is set")
            ("sync-install", po::bool_switch(&sync_install),
             "Refresh build\\install\\game\\translation_table.tsv when --rebuild-table is set")
            ("rebuild-table", po::bool_switch(&rebuild_table),
             "Rebuild translation_table.tsv from script_output\\translated_text.json without API calls")
            ("retranslate", po::bool_switch(&retranslate), "Ignore cache, retranslate everything")
            ("discard-cache", po::bool_switch(&discard_cache),
             "Allow --retranslate to delete a cache that already holds paid work");
        po::variables_map vm;
        if (auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        if (rebuild_table) {
            build_tsv::RebuildTableOptions ropt;
            ropt.project_dir = project;
            ropt.out_tsv = out_tsv;
            ropt.game_dir = game_dir;
            ropt.sync_install = sync_install;
            return build_tsv::run_rebuild_table(ropt);
        }

        if (auto why = translate::validate_options(batch, kDisabledTestBatchCap, max_batches)) {
            log_info("[ERROR] " + *why);
            return apps::kExitRefused;
        }

        const std::string out_dir = project + "\\script_output";
        const std::string translated_file = out_dir + "\\translated_text.json";
        const std::string tsv_file =
            out_tsv.empty() ? project + "\\translation_table.tsv" : out_tsv;
        const std::string cache_file =
            out_dir + "\\translation_cache_gemini.json";

        if (retranslate) {
            try {
                if (auto why = translate::refuse_cache_discard(
                        translate::cache_entry_count(cache_file),
                        "--retranslate", discard_cache)) {
                    log_info("[ERROR] " + *why);
                    return apps::kExitRefused;
                }
            } catch (const std::exception& e) {
                log_info(std::string("[ERROR] ") + e.what());
                return apps::kExitRefused;
            }
        }

        fs::create_directories(fs::u8path(out_dir));

        translate::TranslateOptions opt;
        opt.input_file = out_dir + "\\extracted_text.json";
        opt.cache_file = cache_file;
        opt.output_file = translated_file;
        opt.tsv_file = tsv_file;
        opt.test_dir = project + "\\test";
        opt.batch_size = batch;
        opt.retranslate = retranslate;
        opt.max_batches = max_batches;

        gemini::load_api_key();
        return translate::translate_all(opt);
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return apps::kExitError;
    }
}
