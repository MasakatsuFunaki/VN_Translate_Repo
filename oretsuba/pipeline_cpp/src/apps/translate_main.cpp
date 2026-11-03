// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

#include "apps/paths.h"
#include "common/util.h"
#include "translate/translate_core.h"

namespace {

namespace fs = std::filesystem;
namespace po = boost::program_options;

constexpr const char* kScriptOutputSubdir = "script_output";
constexpr const char* kExtractedJsonFilename = "extracted_text.json";
constexpr const char* kTranslatedJsonFilename = "translated_text.json";
constexpr const char* kCacheJsonFilename = "translation_cache_gemini.json";
constexpr const char* kLastBatchSubdir = "last_gemini_translate";
constexpr const char* kLastBatchFilename = "last_translation.json";
constexpr const char* kToneMarkdownFilename = "TONE_OF_NOVEL.md";
constexpr const char* kBuildSubdir = "build";
constexpr const char* kInstallSubdir = "install";
constexpr const char* kGameSubdir = "game";
constexpr const char* kTsvFilename = "translation_table.tsv";
constexpr int kDefaultTestBatches = 1;
constexpr int kExitRuntimeFailure = 1;

}  // namespace

int main(int argc, char** argv) {
    oretsuba::SetupConsoleUtf8();

    std::string project_dir;
    std::string game_dir;
    po::options_description desc = oretsuba::apps::BuildCommonOptions(project_dir, game_dir);

    oretsuba::translate::TranslateOptions opts;
    std::string out_tsv;
    desc.add_options()
        ("test",
         po::value<int>(&opts.test_batches)->implicit_value(kDefaultTestBatches)->value_name("N"),
         "Test mode (only N batches, default 1)")
        ("max-batches",
         po::value<int>(&opts.max_batches)->default_value(0)->value_name("N"),
         "Maximum number of batches to run (0 = all)")
        ("batch",
         po::value<int>(&opts.batch_size)->default_value(oretsuba::translate::kDefaultBatchSize)->value_name("N"),
         "Batch size")
        ("retranslate",
         po::bool_switch(&opts.retranslate),
         "Retranslate from scratch")
        ("discard-cache",
         po::bool_switch(&opts.discard_cache),
         "Allow discarding a non-empty cache when --retranslate is specified")
        ("rebuild-table",
         po::bool_switch(&opts.rebuild_table_only),
         "Only rebuild translation_table.tsv from existing translated_text.json and cache")
        ("out",
         po::value<std::string>(&out_tsv)->value_name("PATH"),
         "Output path for translation_table.tsv (defaults to build/install/game/translation_table.tsv)");

    po::variables_map vm;
    if (const auto rc = oretsuba::apps::ParseCommandLine(argc, argv, desc, vm)) {
        return *rc;
    }

    if (vm.count("test") > 0) {
        opts.test_mode = true;
    }

    try {
        const fs::path proj_path = fs::u8path(project_dir);
        const fs::path script_out = proj_path / kScriptOutputSubdir;

        opts.project_dir = project_dir;
        opts.extracted_file = (script_out / kExtractedJsonFilename).u8string();
        opts.translated_file = (script_out / kTranslatedJsonFilename).u8string();
        opts.cache_file = (script_out / kCacheJsonFilename).u8string();
        opts.last_batch_log_file =
            (script_out / kLastBatchSubdir / kLastBatchFilename).u8string();
        opts.tone_file = (proj_path / kToneMarkdownFilename).u8string();

        if (!out_tsv.empty()) {
            opts.tsv_file = out_tsv;
        } else {
            opts.tsv_file =
                (proj_path / kBuildSubdir / kInstallSubdir / kGameSubdir / kTsvFilename)
                    .u8string();
        }

        oretsuba::translate::TranslateCore core;
        return core.Run(opts);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[ERROR] %s\n", ex.what());
        return kExitRuntimeFailure;
    }
}
