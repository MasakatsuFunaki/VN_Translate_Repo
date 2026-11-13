// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 00_run_all -- CROSS†CHANNEL -FINAL COMPLETE- English translation pipeline
// orchestrator.
//
//   00_run_all --game-dir <install>
//   00_run_all --dir <project> --game-dir <install>
//
// Sequence: 01_extract -> 02_build_tsv -> deploy.
// 03_find_narrative_cg is deliberately NOT part of the sequence and is run by
// hand when inspecting narrative CGs.
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
using namespace crc;

namespace {

constexpr int kExitOk = 0;
constexpr std::size_t kBannerBarWidth = 60;
constexpr std::size_t kStageExtract = 0;
constexpr std::size_t kStageBuildTsv = 1;
constexpr std::size_t kStageDeploy = 2;

bool run_stage(const std::string& exe_stem, const std::string& project,
               const std::string& game) {
    const std::string exe_path = apps::exe_dir() + "\\" + exe_stem + ".exe";
    const std::vector<std::string> args = {"--dir", project, "--game-dir", game};
    print_line("Running: \"" + exe_path + "\" --dir \"" + project +
               "\" --game-dir \"" + game + "\"");
    if (apps::run_process_utf8(exe_path, args) != kExitOk) {
        log_info("\n[ERROR] " + exe_stem + " failed");
        return false;
    }
    return true;
}

// Ship the two artifacts the game reads.  Nothing is rebuilt here: a rebuild
// would relink the very executable running this, and the copy would then fail
// on a locked binary whenever a pipeline source had changed.
//
// The DLL is a hard failure -- without it the game shows Japanese and nothing
// says why.  The table is a soft one: it is generated, so a missing one means
// translation has not produced it yet.
bool deploy(const std::string& project, const std::string& game) {
    if (const wchar_t* dist = _wgetenv(L"VN_DIST_BUILD"); dist != nullptr && *dist != L'\0') {
        log_info("[DIST] VN_DIST_BUILD set -- skipping the copy to the game folder.");
        return true;
    }

    // The whole game builds into one tree, so the DLL is in build/proxy; when
    // this executable is the staged copy under build/install/bin, the DLL is
    // in the sibling game/ folder instead.
    std::error_code ec;
    fs::path dll;
    for (const auto& candidate : {project + "\\build\\proxy\\Release\\xinput1_3.dll",
                                  apps::exe_dir() + "\\..\\game\\xinput1_3.dll"}) {
        if (fs::exists(fs::u8path(candidate), ec)) {
            dll = fs::u8path(candidate);
            break;
        }
        if (ec) {
            log_info("  WARN: cannot check " + candidate + ": " + ec.message());
        }
    }
    if (dll.empty()) {
        log_info("[ERROR] xinput1_3.dll not found -- build the proxy half first "
                 "(python build.py).");
        return false;
    }

    fs::copy_file(dll, fs::u8path(game + "\\xinput1_3.dll"),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_info("[ERROR] copying xinput1_3.dll failed (" + ec.message() +
                 ") -- is the game running?  Close it and retry.");
        return false;
    }
    log_info("  xinput1_3.dll -> " + game);

    fs::path tsv;
    for (const auto& candidate : {apps::out_tsv(project),
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
    fs::copy_file(tsv, fs::u8path(apps::game_tsv(game)),
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
        po::options_description desc("00_run_all -- CROSS_CHANNEL translation pipeline");
        desc.add(apps::common_options(project, game));
        po::variables_map vm;
        if (const auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::string bar(kBannerBarWidth, '=');
        log_info(bar);
        log_info("CROSS†CHANNEL -FINAL COMPLETE-");
        log_info("English Translation Pipeline");
        log_info(bar);

        const auto& stages = apps::pipeline_steps();
        auto banner = [&](std::size_t i) {
            log_info("\n" + bar);
            log_info("STAGE " + std::to_string(i + 1) + ": " + stages[i].what);
            log_info(bar);
        };

        banner(kStageExtract);
        if (!run_stage(stages[kStageExtract].name, project, game)) {
            return apps::kExitError;
        }

        banner(kStageBuildTsv);
        if (!run_stage(stages[kStageBuildTsv].name, project, game)) {
            return apps::kExitError;
        }

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
