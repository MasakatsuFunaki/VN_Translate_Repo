// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 00_run_all -- pipeline orchestrator (01_extract -> 02_build_tsv -> deploy).
#include "apps/paths.h"
#include "apps/pipeline_steps.h"
#include "common/util.h"

#include <cstddef>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace snu;

namespace {

constexpr std::size_t kBannerWidth = 60;
constexpr std::size_t kExtractIndex = 0;
constexpr std::size_t kBuildTsvIndex = 1;
constexpr std::size_t kDeployIndex = 2;
constexpr std::size_t kDisplayIndexOffset = 1;

bool run_stage(const std::string& exe_stem, const std::vector<std::string>& args) {
    const std::string exe_path = apps::exe_dir() + "\\" + exe_stem + ".exe";
    if (apps::run_process(exe_path, args) != apps::kExitOk) {
        log_info("\n[ERROR] " + exe_stem + " failed");
        return false;
    }
    return true;
}

// Copy winmm.dll (hard failure if missing) and translations.tsv (soft --
// the translation pass may not have produced it yet) into the game folder.
bool deploy(const std::string& project, const std::string& game) {
    if (const wchar_t* dist = _wgetenv(L"VN_DIST_BUILD");
        dist != nullptr && *dist != L'\0') {
        log_info("[DIST] VN_DIST_BUILD set -- skipping the copy to the game folder.");
        return true;
    }

    // Try build/proxy first, then the install tree's game/ folder.
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

    fs::path tsv;
    for (const auto& candidate : {project + "\\build\\install\\game\\translations.tsv",
                                  apps::exe_dir() + "\\..\\game\\translations.tsv"}) {
        if (fs::exists(fs::u8path(candidate), ec)) {
            tsv = fs::u8path(candidate);
            break;
        }
        if (ec) {
            log_info("  WARN: cannot check " + candidate + ": " + ec.message());
        }
    }
    if (tsv.empty()) {
        log_info("  WARN: translations.tsv not found -- 02_build_tsv produces it.");
        return true;
    }
    fs::copy_file(tsv, fs::u8path(game + "\\translations.tsv"),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_info("[ERROR] copying translations.tsv failed (" + ec.message() + ")");
        return false;
    }
    log_info("  translations.tsv -> " + game);
    return true;
}

}  // namespace

int main() {
    try {
        setup_console_utf8();
        namespace po = apps::po;

        std::string project;
        std::string game;
        po::options_description desc("00_run_all -- Sakura no Uta translation pipeline");
        desc.add(apps::common_options(project, game));
        po::variables_map vm;
        if (const auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::vector<std::string> full_args = {
            "--dir", project, "--game-dir", game};
        const std::vector<std::string> project_args = {"--dir", project};

        const std::string bar(kBannerWidth, '=');
        log_info(bar);
        log_info("Sakura no Uta (サクラノ詩)");
        log_info("English Translation Pipeline");
        log_info(bar);

        const auto& stages = apps::pipeline_steps();
        auto banner = [&](std::size_t i) {
            log_info("\n" + bar);
            log_info("STEP " + std::to_string(i + kDisplayIndexOffset) + ": " +
                     stages[i].what);
            log_info(bar);
        };

        banner(kExtractIndex);
        if (!run_stage(stages[kExtractIndex].name, full_args)) {
            return apps::kExitFailure;
        }

        banner(kBuildTsvIndex);
        if (!run_stage(stages[kBuildTsvIndex].name, project_args)) {
            return apps::kExitFailure;
        }

        banner(kDeployIndex);
        if (!deploy(project, game)) {
            return apps::kExitFailure;
        }

        log_info("\n" + bar);
        log_info("TRANSLATION COMPLETE!");
        log_info(bar);
        return apps::kExitOk;
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return apps::kExitFailure;
    }
}
