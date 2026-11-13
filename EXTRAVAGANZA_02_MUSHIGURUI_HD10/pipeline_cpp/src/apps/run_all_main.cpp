// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 00_run_all -- Mushigurui HD10 (蟲狂い) English
// translation pipeline orchestrator.
//
//   00_run_all --game-dir <dir>
//
// Steps: 01_extract -> 02_build_tsv -> deploy.  The translation step carries
// the speaker gate and the runtime table with it, so everything needed for the
// game to display English runs in sequence.
//
// The asset tools (03_build_xtx, 04_find_narrative_cg,
// 05_replace_choice_text, 06_patch_menus) are deliberately not here: they
// touch the shipped archives, the config files and the screenshots, not the
// script text.
#include "apps/paths.h"
#include "common/util.h"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace mgi;

namespace {

constexpr std::size_t kBannerWidth = 60;

bool run_stage(const std::string& exe, const std::vector<std::string>& stage_args) {
    const fs::path exe_path = fs::u8path(apps::exe_dir()) / fs::u8path(exe + ".exe");
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

// Ship the one artifact the game cannot produce for itself.  Nothing is
// rebuilt here: a rebuild would relink the very executable running this, and
// the copy would then fail on a locked binary whenever a pipeline source had
// changed.
//
// The DLL is a hard failure -- without it the game shows Japanese and nothing
// says why.  The table is a soft one: 02_build_tsv writes it straight into
// the game folder, so a missing one means the translation step has not
// produced it yet.
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

    fs::copy_file(dll, game_root / "winmm.dll",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_info("[ERROR] copying winmm.dll failed (" + ec.message() +
                 ") -- is the game running?  Close it and retry.");
        return false;
    }
    log_info("  winmm.dll -> " + game);

    const bool have_table = fs::exists(game_root / "translation_table.tsv", ec);
    if (ec) {
        log_info("  WARN: cannot check the game folder for "
                 "translation_table.tsv: " + ec.message());
        return true;
    }
    if (!have_table) {
        log_info("  WARN: translation_table.tsv not in the game folder -- "
                 "02_build_tsv produces it.");
    }
    return true;
}

}  // namespace

int main() {
    namespace po = apps::po;

    try {
        setup_console_utf8();

        std::string project;
        std::string game;
        po::options_description desc("00_run_all -- Mushigurui HD10 translation pipeline");
        desc.add(apps::common_options(project, game));
        po::variables_map vm;
        if (const auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::vector<std::string> passthrough = {
            "--dir", project, "--game-dir", game};

        const std::string bar(kBannerWidth, '=');
        log_info(bar);
        log_info("Mushigurui HD10 (Bug Madness)");
        log_info("English Translation Pipeline");
        log_info(bar);

        log_info("\n" + bar);
        log_info("STEP 1: Extract text from SPT files");
        log_info(bar);
        if (!run_stage("01_extract", passthrough)) {
            return apps::kExitError;
        }

        log_info("\n" + bar);
        log_info("STEP 2: Speaker gate, assemble translated script, build translation_table.tsv");
        log_info(bar);
        if (!run_stage("02_build_tsv", passthrough)) {
            return apps::kExitError;
        }

        log_info("\n" + bar);
        log_info("STEP 3: Deploy winmm.dll to the game folder");
        log_info(bar);
        if (!deploy(project, game)) {
            return apps::kExitError;
        }

        log_info("\n" + bar);
        log_info("TRANSLATION COMPLETE!");
        log_info(bar);
        return apps::kExitOk;
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return apps::kExitError;
    }
}
