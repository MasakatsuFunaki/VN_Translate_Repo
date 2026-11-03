// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "common/util.h"

namespace {

namespace fs = std::filesystem;
namespace po = boost::program_options;

constexpr const char* kScriptOutputSubdir = "script_output";
constexpr const char* kTranslatedJsonFilename = "translated_text.json";
constexpr const char* kCacheJsonFilename = "translation_cache_gemini.json";
constexpr const char* kBuildSubdir = "build";
constexpr const char* kInstallSubdir = "install";
constexpr const char* kGameSubdir = "game";
constexpr const char* kTsvFilename = "translation_table.tsv";
constexpr int kExitRuntimeFailure = 1;

}  // namespace

int main(int argc, char** argv) {
    oretsuba::SetupConsoleUtf8();

    std::string project_dir;
    std::string game_dir;
    po::options_description desc = oretsuba::apps::BuildCommonOptions(project_dir, game_dir);

    bool rebuild_table = false;
    std::string out_tsv;
    std::string translated_file;
    std::string cache_file;
    std::size_t wrap_columns = oretsuba::build_tsv::kDefaultWrapColumns;

    desc.add_options()
        ("rebuild-table",
         po::bool_switch(&rebuild_table),
         "Rebuild translation_table.tsv from translated_text.json and cache")
        ("out",
         po::value<std::string>(&out_tsv)->value_name("PATH"),
         "Output path for translation_table.tsv (defaults to build/install/game/translation_table.tsv)")
        ("translated",
         po::value<std::string>(&translated_file)->value_name("PATH"),
         "Path to translated_text.json")
        ("cache",
         po::value<std::string>(&cache_file)->value_name("PATH"),
         "Path to translation_cache_gemini.json")
        ("wrap-cols",
         po::value<std::size_t>(&wrap_columns)->default_value(oretsuba::build_tsv::kDefaultWrapColumns)->value_name("N"),
         "Maximum proportional columns per line before inserting '\\'");

    po::variables_map vm;
    if (const auto rc = oretsuba::apps::ParseCommandLine(argc, argv, desc, vm)) {
        return *rc;
    }

    try {
        if (!game_dir.empty()) {
            oretsuba::apps::UnwrapInnerPeIfNeeded(fs::u8path(game_dir));
        }

        const fs::path proj_path = fs::u8path(project_dir);
        const fs::path script_out = proj_path / kScriptOutputSubdir;

        if (translated_file.empty()) {
            translated_file = (script_out / kTranslatedJsonFilename).u8string();
        }
        if (cache_file.empty()) {
            cache_file = (script_out / kCacheJsonFilename).u8string();
        }
        if (out_tsv.empty()) {
            out_tsv =
                (proj_path / kBuildSubdir / kInstallSubdir / kGameSubdir / kTsvFilename)
                    .u8string();
        }

        const oretsuba::build_tsv::TsvBuilder builder(wrap_columns);
        return builder.Run(translated_file, cache_file, out_tsv);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[ERROR] %s\n", ex.what());
        return kExitRuntimeFailure;
    }
}
