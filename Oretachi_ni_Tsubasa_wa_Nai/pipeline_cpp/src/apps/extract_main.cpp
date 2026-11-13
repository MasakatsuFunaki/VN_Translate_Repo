// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "apps/paths.h"
#include "common/util.h"
#include "extract/extractor.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;
namespace po = boost::program_options;

constexpr const char* kScriptOutputSubdir = "script_output";
constexpr const char* kExtractedJsonFilename = "extracted_text.json";
constexpr int kExitRuntimeFailure = 1;

}  // namespace

int main() {
    try {
        oretsuba::SetupConsoleUtf8();

        std::string project_dir;
        std::string game_dir;
        po::options_description desc =
            oretsuba::apps::BuildCommonOptions(project_dir, game_dir);

        po::variables_map vm;
        if (const auto rc = oretsuba::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const fs::path out_path =
            fs::u8path(project_dir) / kScriptOutputSubdir / kExtractedJsonFilename;
        const oretsuba::extract::Extractor extractor;
        return extractor.Run(game_dir, out_path.u8string());
    } catch (const std::exception& err) {
        std::fprintf(stderr, "[ERROR] %s\n", err.what());
        return kExitRuntimeFailure;
    }
}
