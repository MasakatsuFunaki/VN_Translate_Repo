// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

#include "apps/paths.h"
#include "common/util.h"
#include "extract/extractor.h"

namespace {

namespace fs = std::filesystem;
namespace po = boost::program_options;

constexpr const char* kScriptOutputSubdir = "script_output";
constexpr const char* kExtractedJsonFilename = "extracted_text.json";
constexpr int kExitRuntimeFailure = 1;

}  // namespace

int main(int argc, char** argv) {
    oretsuba::SetupConsoleUtf8();

    std::string project_dir;
    std::string game_dir;
    po::options_description desc = oretsuba::apps::BuildCommonOptions(project_dir, game_dir);

    po::variables_map vm;
    if (const auto rc = oretsuba::apps::ParseCommandLine(argc, argv, desc, vm)) {
        return *rc;
    }

    try {
        const fs::path out_path =
            fs::u8path(project_dir) / kScriptOutputSubdir / kExtractedJsonFilename;
        const oretsuba::extract::Extractor extractor;
        return extractor.Run(game_dir, out_path.u8string());
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[ERROR] %s\n", ex.what());
        return kExitRuntimeFailure;
    }
}
