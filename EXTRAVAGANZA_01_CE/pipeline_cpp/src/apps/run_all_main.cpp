// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 00_run_all -- EXTRAVAGANZA ~Mushi Mederu Shoujo~ Complete Edition English
// translation pipeline orchestrator.
//
//   00_run_all --game-dir <dir>
//
// Runs 01_extract -> 02_build_tsv -> 03_repack -> 04_build_xtx ->
// 05_build_charts -> 06_build_menus -> deploy.
#include "apps/paths.h"
#include "apps/pipeline_steps.h"
#include "common/util.h"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace exc;

namespace {

constexpr std::size_t kBannerWidth = 60;
constexpr std::size_t kExtractStageIndex = 0;
constexpr std::size_t kTranslateStageIndex = 1;
constexpr std::size_t kFirstPostTranslateIndex = 2;

bool run_stage(const std::string& exe, const std::vector<std::string>& stage_args) {
    const fs::path exe_path = fs::u8path(apps::exe_dir()) / (exe + ".exe");
    std::string display = "\"" + exe_path.u8string() + "\"";
    for (const std::string& arg : stage_args) {
        display += " \"" + arg + "\"";
    }
    print_line("Running: " + display);
    if (apps::run_child_process(exe_path.u8string(), stage_args) != apps::kExitOk) {
        log_info("\n[ERROR] " + exe + " failed");
        return false;
    }
    return true;
}

bool is_game_only_tool(std::string_view tool_name) {
    return tool_name == "04_build_xtx" || tool_name == "06_build_menus";
}

bool deploy(const std::string& project, const std::string& game) {
    if (apps::wide_env_utf8(L"VN_DIST_BUILD").has_value()) {
        log_info("[DIST] VN_DIST_BUILD set -- skipping the copy to the game folder.");
        return true;
    }

    const fs::path project_root = fs::u8path(project);
    const fs::path game_root = fs::u8path(game);
    const fs::path exe_root = fs::u8path(apps::exe_dir());

    std::error_code ec;
    fs::path dll;
    for (const fs::path& candidate :
         {project_root / "game" / "winmm.dll",
          project_root / "build" / "proxy" / "Release" / "winmm.dll",
          exe_root / ".." / "game" / "winmm.dll"}) {
        if (fs::exists(candidate, ec)) {
            dll = candidate;
            break;
        }
        if (ec) {
            log_info("  WARN: cannot check " + candidate.u8string() + ": " + ec.message());
        }
    }
    if (dll.empty()) {
        log_info("[ERROR] winmm.dll not found -- build the proxy half first "
                 "(python build.py).");
        return false;
    }

    fs::copy_file(dll, game_root / "winmm.dll", fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_info("[ERROR] copying winmm.dll failed (" + ec.message() +
                 ") -- is the game running?  Close it and retry.");
        return false;
    }
    log_info("  winmm.dll -> " + game);

    for (const fs::path& candidate :
         {project_root / "game" / "extravaganza_patch.bin",
          project_root / "extravaganza_patch.bin",
          exe_root / ".." / "game" / "extravaganza_patch.bin"}) {
        std::error_code patch_ec;
        if (fs::exists(candidate, patch_ec)) {
            fs::copy_file(candidate, game_root / "extravaganza_patch.bin",
                          fs::copy_options::overwrite_existing, patch_ec);
            if (!patch_ec) {
                log_info("  extravaganza_patch.bin -> " + game);
            }
            break;
        }
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
        po::options_description desc("00_run_all -- EXTRAVAGANZA CE translation pipeline");
        desc.add(apps::common_options(project, game));
        po::variables_map vm;
        if (const auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::vector<std::string> project_args = {"--dir", project};
        const std::vector<std::string> game_args = {"--game-dir", game};
        const std::vector<std::string> both_args = {"--dir", project, "--game-dir", game};

        const std::string bar(kBannerWidth, '=');
        log_info(bar);
        log_info("EXTRAVAGANZA ~Mushi Mederu Shoujo~ Complete Edition");
        log_info("English Translation Pipeline");
        log_info(bar);

        const auto& stages = apps::pipeline_steps();
        const auto banner = [&](std::size_t i) {
            log_info("\n" + bar);
            log_info("[" + std::to_string(i + 1) + "/" + std::to_string(stages.size()) +
                     "] " + stages[i].what);
            log_info(bar);
        };

        banner(kExtractStageIndex);
        if (!run_stage(stages[kExtractStageIndex].name, both_args)) {
            return apps::kExitError;
        }

        banner(kTranslateStageIndex);
        if (!run_stage(stages[kTranslateStageIndex].name, project_args)) {
            return apps::kExitError;
        }

        for (std::size_t i = kFirstPostTranslateIndex; i + 1 < stages.size(); ++i) {
            banner(i);
            const auto& args = is_game_only_tool(stages[i].name) ? game_args : both_args;
            if (!run_stage(stages[i].name, args)) {
                return apps::kExitError;
            }
        }

        banner(stages.size() - 1);
        if (!deploy(project, game)) {
            return apps::kExitError;
        }

        log_info("\n" + bar);
        log_info("TRANSLATION COMPLETE!");
        log_info(bar);
        return apps::kExitOk;
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return apps::kExitError;
    }
}
