// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "common/util.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

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

int main() {
    try {
        oretsuba::SetupConsoleUtf8();

        std::string project_dir;
        po::options_description desc =
            oretsuba::apps::BuildProjectOnlyOptions(project_dir);

        std::string out_tsv;
        desc.add_options()
            ("out",
             po::value<std::string>(&out_tsv)->value_name("PATH"),
             "Output path for translation_table.tsv (defaults to build/install/game/translation_table.tsv)");

        po::variables_map vm;
        if (const auto rc = oretsuba::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const fs::path proj_path = fs::u8path(project_dir);
        const fs::path script_out = proj_path / kScriptOutputSubdir;
        const std::string translated_file = (script_out / kTranslatedJsonFilename).u8string();
        const std::string cache_file = (script_out / kCacheJsonFilename).u8string();

        if (out_tsv.empty()) {
            out_tsv =
                (proj_path / kBuildSubdir / kInstallSubdir / kGameSubdir / kTsvFilename)
                    .u8string();
        }

        const oretsuba::build_tsv::TsvBuilder builder(oretsuba::build_tsv::kDefaultWrapColumns);
        return builder.Run(translated_file, cache_file, out_tsv);
    } catch (const std::exception& err) {
        std::fprintf(stderr, "[ERROR] %s\n", err.what());
        return kExitRuntimeFailure;
    }
}
