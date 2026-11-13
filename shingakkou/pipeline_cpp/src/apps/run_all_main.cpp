// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 00_run_all -- Shingakkou ~Noli me tangere~ DL English translation pipeline
// orchestrator.
//
//   00_run_all --game-dir <G>
//
// Pipeline order: 01_extract -> 02_build_tsv -> deploy.
//
// 03_find_narrative_cg is standalone and is not invoked by 00_run_all.
#include "apps/paths.h"
#include "apps/pipeline_steps.h"
#include "common/util.h"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace shin;

namespace {

constexpr int kExitOk = 0;
constexpr std::size_t kBannerWidth = 60;
constexpr std::size_t kExtractStageIndex = 0;
constexpr std::size_t kBuildTsvStageIndex = 1;
constexpr std::size_t kDeployStageIndex = 2;
constexpr std::size_t kStageDisplayOffset = 1;

bool run_stage(const std::string& exe_stem,
               const std::vector<std::string>& extra_args,
               const std::string& project,
               const std::string& game) {
    const std::string exe_path =
        (fs::u8path(apps::exe_dir()) / (exe_stem + ".exe")).u8string();
    std::vector<std::string> child_args = {
        "--dir", project,
        "--game-dir", game,
    };
    child_args.insert(child_args.end(), extra_args.begin(), extra_args.end());

    std::string display = "\"" + exe_path + "\"";
    for (const std::string& arg : child_args) {
        display += " \"" + arg + "\"";
    }
    print_line("Running: " + display);

    if (apps::run_process(exe_path, child_args) != kExitOk) {
        log_info("\n[ERROR] " + exe_stem + " failed");
        return false;
    }
    return true;
}

// Ship what the game reads.  Nothing is rebuilt here: a rebuild would relink
// the very executable running this, and the copy would then fail on a locked
// binary whenever a pipeline source had changed.
//
// The DLL is a hard failure -- without it the game shows Japanese and nothing
// says why.  The table is only checked: unlike the other games in this repo it
// is not staged from the checkout, because 02_build_tsv writes it straight
// into the game folder.
bool deploy(const std::string& project, const std::string& game) {
    if (const char* dist = std::getenv("VN_DIST_BUILD"); dist && *dist) {
        log_info("[DIST] VN_DIST_BUILD set -- skipping the copy to the game folder.");
        return true;
    }

    std::error_code ec;
    fs::path dll;
    for (const auto& candidate : {project + "\\build\\proxy\\Release\\winmm.dll",
                                  apps::exe_dir() + "\\..\\game\\winmm.dll"}) {
        if (fs::exists(fs::u8path(candidate), ec)) {
            dll = fs::u8path(candidate);
            break;
        }
        if (ec) {
            log_info("  WARN: cannot check " + candidate + ": " + ec.message());
        }
    }
    if (dll.empty()) {
        log_info("[ERROR] winmm.dll not found -- build the proxy half first "
                 "(python build.py).");
        return false;
    }

    fs::copy_file(dll, fs::u8path(game + "\\winmm.dll"),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_info("[ERROR] copying winmm.dll failed (" + ec.message() +
                 ") -- is the game running?  Close it and retry.");
        return false;
    }
    log_info("  winmm.dll -> " + game);

    const bool have_table =
        fs::exists(fs::u8path(game + "\\translation_table.tsv"), ec);
    if (ec) {
        log_info("  WARN: cannot check the game folder for "
                 "translation_table.tsv: " + ec.message());
        return true;
    }
    if (!have_table) {
        log_info("  WARN: translation_table.tsv not in the game folder -- "
                 "02_build_tsv writes it there.");
        return true;
    }
    log_info("  translation_table.tsv is in place.");
    return true;
}

}  // namespace

int main() {
    setup_console_utf8();
    namespace po = apps::po;

    try {
        std::string project;
        std::string game;
        po::options_description desc("00_run_all -- Shingakkou translation pipeline");
        desc.add(apps::common_options(project, game));
        po::variables_map vm;
        if (const auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::string bar(kBannerWidth, '=');
        log_info(bar);
        log_info("Shingakkou ~Noli me tangere~ DL");
        log_info("English Translation Pipeline");
        log_info(bar);

        const auto& stages = apps::pipeline_steps();
        auto banner = [&](std::size_t i) {
            log_info("\n" + bar);
            log_info("STEP " + std::to_string(i + kStageDisplayOffset) + ": " + stages[i].what);
            log_info(bar);
        };

        banner(kExtractStageIndex);
        if (!run_stage(stages[kExtractStageIndex].name, {}, project, game)) {
            return apps::kExitError;
        }

        banner(kBuildTsvStageIndex);
        if (!run_stage(stages[kBuildTsvStageIndex].name, {}, project, game)) {
            return apps::kExitError;
        }

        banner(kDeployStageIndex);
        if (!deploy(project, game)) {
            return apps::kExitError;
        }

        log_info("\n" + bar);
        log_info("TRANSLATION COMPLETE!");
        log_info(bar);
        return kExitOk;
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return apps::kExitError;
    }
}
