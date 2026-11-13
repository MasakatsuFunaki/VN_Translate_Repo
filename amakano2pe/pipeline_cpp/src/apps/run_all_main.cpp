// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 00_run_all -- Amakano 2 ~Perfect Edition~ (CatSystem2 / cs2.exe)
// English translation pipeline orchestrator.
//
//   00_run_all --game-dir <install>
//   00_run_all --dir <project> --game-dir <install>
//
// Sequence: 01_extract -> 02_build_tsv -> deploy.
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
using namespace ama;

namespace {

constexpr int kExitOk = 0;
constexpr std::size_t kBannerBarWidth = 60;
constexpr std::size_t kDisplayIndexOffset = 1;
constexpr std::size_t kExtractIndex = 0;
constexpr std::size_t kBuildTsvIndex = 1;
constexpr std::size_t kDeployIndex = 2;

bool run_child(const std::string& exe, const std::vector<std::string>& child_args) {
    const std::string exe_path = apps::exe_dir() + "\\" + exe + ".exe";
    if (apps::run_process(exe_path, child_args) != kExitOk) {
        log_info("\n[ERROR] " + exe + " failed");
        return false;
    }
    return true;
}

// Ship the one artifact CMake builds.  Nothing is rebuilt here: a rebuild would
// relink the very executable running this, and the copy would then fail on a
// locked binary whenever a pipeline source had changed.
//
// The DLL is a hard failure -- without it the game shows Japanese and nothing
// says why.  The table is only checked: 02_build_tsv writes it into the game
// folder itself, so there is nothing here to copy and a missing one just means
// the translation pass has not produced it yet.
bool deploy(const std::string& project, const std::string& game) {
    if (const wchar_t* dist = _wgetenv(L"VN_DIST_BUILD"); dist != nullptr && *dist != L'\0') {
        log_info("[DIST] VN_DIST_BUILD set -- skipping the copy to the game folder.");
        return true;
    }

    // The whole game builds into one tree, so the DLL is in build/proxy; when
    // this executable is the staged copy under build/install/bin, the DLL is in
    // the sibling game/ folder instead.
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
                 ") -- is the game running?  Close it and retry, or point "
                 "--game-dir at the install folder.");
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
        po::options_description desc("00_run_all -- Amakano 2 PE translation pipeline");
        desc.add(apps::common_options(project, game));
        po::variables_map vm;
        if (const auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        // Children inherit explicit UTF-8 paths so a --dir override propagates.
        const std::vector<std::string> child_args = {
            "--dir",
            project,
            "--game-dir",
            game,
        };

        const std::string bar(kBannerBarWidth, '=');
        log_info(bar);
        log_info("Amakano 2 ~Perfect Edition~");
        log_info("English Translation Pipeline");
        log_info(bar);

        const auto& steps = apps::pipeline_steps();
        const auto banner = [&](std::size_t i) {
            log_info("\n" + bar);
            log_info("STEP " + std::to_string(i + kDisplayIndexOffset) + ": " +
                     steps[i].what);
            log_info(bar);
        };

        banner(kExtractIndex);
        if (!run_child(steps[kExtractIndex].name, child_args)) {
            return apps::kExitError;
        }

        banner(kBuildTsvIndex);
        if (!run_child(steps[kBuildTsvIndex].name, child_args)) {
            return apps::kExitError;
        }

        banner(kDeployIndex);
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
