// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "extract/extractor.h"
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
constexpr const char* kProxyDllFilename = "winmm.dll";
constexpr int kDefaultTestBatches = 1;
constexpr int kExitRuntimeFailure = 1;

void DeployArtifactsToGameDir(const fs::path& install_game_dir,
                              const fs::path& tsv_path,
                              const fs::path& game_dir) {
    if (game_dir.empty() || !fs::exists(game_dir)) {
        return;
    }

    if (oretsuba::apps::UnwrapInnerPeIfNeeded(game_dir)) {
        std::cout << "[Deploy] Unwrapped inner Lucifen PE to "
                  << (game_dir / "ORE_TUBA.exe").u8string() << "\n";
    }

    std::error_code ec;
    if (fs::exists(tsv_path, ec)) {
        const fs::path dst_tsv = game_dir / kTsvFilename;
        fs::copy_file(tsv_path, dst_tsv, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::cout << "[Deploy] Copied " << dst_tsv.u8string() << "\n";
        }
    }

    const fs::path src_dll = install_game_dir / kProxyDllFilename;
    if (fs::exists(src_dll, ec)) {
        const fs::path dst_dll = game_dir / kProxyDllFilename;
        fs::copy_file(src_dll, dst_dll, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::cout << "[Deploy] Copied " << dst_dll.u8string() << "\n";
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    oretsuba::SetupConsoleUtf8();

    std::string project_dir;
    std::string game_dir;
    po::options_description desc = oretsuba::apps::BuildCommonOptions(project_dir, game_dir);

    oretsuba::translate::TranslateOptions opts;
    std::string out_tsv;
    bool skip_extract = false;
    bool skip_translate = false;
    bool skip_deploy = false;

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
        ("skip-extract",
         po::bool_switch(&skip_extract),
         "Skip 01_extract step if extracted_text.json already exists")
        ("skip-translate",
         po::bool_switch(&skip_translate),
         "Skip 02_translate step and only rebuild translation_table.tsv")
        ("skip-deploy",
         po::bool_switch(&skip_deploy),
         "Skip unwrapping ORE_TUBA.exe and copying artifacts to game directory")
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
        const fs::path install_game_dir =
            proj_path / kBuildSubdir / kInstallSubdir / kGameSubdir;

        opts.project_dir = project_dir;
        opts.extracted_file = (script_out / kExtractedJsonFilename).u8string();
        opts.translated_file = (script_out / kTranslatedJsonFilename).u8string();
        opts.cache_file = (script_out / kCacheJsonFilename).u8string();
        opts.last_batch_log_file =
            (script_out / kLastBatchSubdir / kLastBatchFilename).u8string();
        opts.tone_file = (proj_path / kToneMarkdownFilename).u8string();
        opts.tsv_file = out_tsv.empty()
                            ? (install_game_dir / kTsvFilename).u8string()
                            : out_tsv;

        if (!skip_extract && !opts.rebuild_table_only) {
            std::cout << "=== [1/4] Extracting SCRIPT.LPK ===\n";
            const oretsuba::extract::Extractor extractor;
            const int extract_rc = extractor.Run(game_dir, opts.extracted_file);
            if (extract_rc != 0) {
                return extract_rc;
            }
        }

        if (!skip_translate) {
            std::cout << "=== [2/4] Translating & [3/4] Building TSV ===\n";
            oretsuba::translate::TranslateCore core;
            const int translate_rc = core.Run(opts);
            if (translate_rc != 0) {
                return translate_rc;
            }
        } else {
            std::cout << "=== [3/4] Building TSV ===\n";
            const oretsuba::build_tsv::TsvBuilder builder;
            const int tsv_rc = builder.Run(opts.translated_file, opts.cache_file, opts.tsv_file);
            if (tsv_rc != 0) {
                return tsv_rc;
            }
        }

        if (!skip_deploy) {
            std::cout << "=== [4/4] Unwrapping Inner PE & Deploying ===\n";
            DeployArtifactsToGameDir(
                install_game_dir, fs::u8path(opts.tsv_file), fs::u8path(game_dir));
        }

        return oretsuba::apps::kExitSuccess;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[ERROR] %s\n", ex.what());
        return kExitRuntimeFailure;
    }
}
