// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 00_run_all -- orchestrator: 01_extract -> 02_build_tsv -> deploy.
#include "apps/paths.h"
#include "apps/pipeline_steps.h"
#include "common/util.h"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace frat;

namespace {

constexpr int kExitOk = 0;
constexpr std::size_t kBannerBarWidth = 60;
constexpr std::size_t kStageExtract = 0;
constexpr std::size_t kStageBuildTsv = 1;
constexpr std::size_t kStageDeploy = 2;

void run_child_tool(const std::string& exe_stem, const std::string& project,
                    const std::string& game) {
    const std::string bin_dir = apps::exe_dir();
    if (bin_dir.empty()) {
        throw std::runtime_error("cannot resolve executable directory");
    }
    const std::string exe_path =
        (fs::u8path(bin_dir) / (exe_stem + ".exe")).u8string();
    const std::vector<std::string> child_args = {
        "--dir", project, "--game-dir", game};

    print_line("Running: \"" + exe_path + "\" --dir \"" + project +
               "\" --game-dir \"" + game + "\"");
    const int rc = apps::run_process(exe_path, child_args);
    if (rc != kExitOk) {
        throw std::runtime_error(exe_stem + " failed (exit code " +
                                 std::to_string(rc) + ")");
    }
}

// Copy winmm.dll to the game folder. The table is not copied — 02_build_tsv
// writes it directly.
bool deploy(const std::string& project, const std::string& game) {
    if (const char* dist = std::getenv("VN_DIST_BUILD"); dist && *dist) {
        log_info("[DIST] VN_DIST_BUILD set -- skipping the copy to the game folder.");
        return true;
    }

    // Non-throwing: a status query that fails is not proof the file is absent.
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
                 "02_build_tsv writes it.");
    }
    return true;
}

}  // namespace

int main() {
    setup_console_utf8();
    namespace po = apps::po;

    try {
        std::string project;
        std::string game;
        po::options_description desc(
            "00_run_all -- Fraternite HD Translation Pipeline");
        desc.add(apps::common_options(project, game));
        po::variables_map vm;
        if (auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::string bar(kBannerBarWidth, '=');
        log_info(bar);
        log_info("Fraternite HD Remaster (フラテルニテ HDリマスター)");
        log_info("English Translation Pipeline");
        log_info(bar);

        const auto& stages = apps::pipeline_steps();
        auto banner = [&](std::size_t i) {
            log_info("\n" + bar);
            log_info("STEP " + std::to_string(i + 1) + ": " + stages[i].what);
            log_info(bar);
        };

        banner(kStageExtract);
        run_child_tool(stages[kStageExtract].name, project, game);

        banner(kStageBuildTsv);
        run_child_tool(stages[kStageBuildTsv].name, project, game);

        banner(kStageDeploy);
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
