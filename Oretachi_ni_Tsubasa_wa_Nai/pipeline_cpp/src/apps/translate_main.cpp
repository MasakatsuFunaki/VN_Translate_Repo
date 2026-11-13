// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "apps/paths.h"
#include "common/util.h"
#include "translate/translate_core.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

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
constexpr int kUnlimitedBatches = 0;
constexpr int kExitRuntimeFailure = 1;

}  // namespace

int main() {
    try {
        oretsuba::SetupConsoleUtf8();

        std::string project_dir;
        po::options_description desc =
            oretsuba::apps::BuildProjectOnlyOptions(project_dir);

        oretsuba::translate::TranslateOptions opts;
        desc.add_options()
            ("max-batches",
             po::value<int>(&opts.max_batches)->default_value(kUnlimitedBatches)->value_name("N"),
             "Maximum number of batches to run (0 = all)")
            ("batch",
             po::value<int>(&opts.batch_size)->default_value(oretsuba::translate::kDefaultBatchSize)->value_name("N"),
             "Batch size")
            ("retranslate",
             po::bool_switch(&opts.retranslate),
             "Retranslate from scratch")
            ("discard-cache",
             po::bool_switch(&opts.discard_cache),
             "Allow discarding a non-empty cache when --retranslate is specified");

        po::variables_map vm;
        if (const auto rc = oretsuba::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const fs::path proj_path = fs::u8path(project_dir);
        const fs::path script_out = proj_path / kScriptOutputSubdir;

        opts.project_dir = project_dir;
        opts.extracted_file = (script_out / kExtractedJsonFilename).u8string();
        opts.translated_file = (script_out / kTranslatedJsonFilename).u8string();
        opts.cache_file = (script_out / kCacheJsonFilename).u8string();
        opts.last_batch_log_file =
            (script_out / kLastBatchSubdir / kLastBatchFilename).u8string();
        opts.tone_file = (proj_path / kToneMarkdownFilename).u8string();

        oretsuba::translate::TranslateCore core;
        return core.Run(opts);
    } catch (const std::exception& err) {
        std::fprintf(stderr, "[ERROR] %s\n", err.what());
        return kExitRuntimeFailure;
    }
}
