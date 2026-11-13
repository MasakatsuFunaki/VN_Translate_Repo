// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 01_patch -- deploy pre-built alpharom_helper.dll, launch.exe, and rebuilt
// 0script.pak / loose script\*.nps artifacts into the game folder.
#include "apps/paths.h"
#include "common/util.h"
#include "patch/deploy.h"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace ksh;

namespace {

constexpr const char* kHelperDll = "alpharom_helper.dll";
constexpr const char* kLaunchExe = "launch.exe";
constexpr std::size_t kBannerWidth = 60;

// Both helper artifacts, in the order they are copied.
std::vector<std::string> artifacts_in(const std::string& dir) {
    return {patch::path_join(dir, kHelperDll), patch::path_join(dir, kLaunchExe)};
}

int fail(const std::string& msg) {
    std::cerr << msg << std::endl;
    return ksh::apps::kExitError;
}

}  // namespace

int main() {
    try {
        setup_console_utf8();
        namespace po = ksh::apps::po;

        std::string project;
        std::string game_dir;
        po::options_description desc(
            "01_patch -- deploy the AlphaROM helper and rebuilt script artifacts");
        desc.add(ksh::apps::common_options(project));
        desc.add_options()
            ("game-dir", po::value(&game_dir)->required(),
             "Game install directory");
        po::variables_map vm;
        if (auto rc = ksh::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::string exe_dir = ksh::apps::exe_dir();
        log_info("[*] project:  " + project);
        log_info("[*] game dir: " + game_dir);
        print_line("");

        std::error_code ec;
        if (!fs::is_directory(fs::u8path(game_dir), ec)) {
            return fail("!! game dir does not exist: " + game_dir);
        }

        if (!patch::can_write(game_dir)) {
            log_info("!! Cannot write to " + game_dir + ".");
            if (!patch::is_admin()) {
                log_info("!! Re-run from an elevated PowerShell:");
                log_info("!!   right-click PowerShell -> 'Run as Administrator'");
                return ksh::apps::kExitError;
            }
            log_info("!! (You are admin but the directory is still not writable.");
            log_info("!!  Check the path or pass --game-dir.)");
            return ksh::apps::kExitError;
        }

        const std::string artifact_dir = patch::find_helper_artifact_dir(project, exe_dir);
        if (artifact_dir.empty()) {
            return fail(
                "!! pre-built helper artifacts (alpharom_helper.dll, launch.exe) not found "
                "under " +
                project + "; run 'python build.py --install' first");
        }

        const std::vector<std::string> artifacts = artifacts_in(artifact_dir);
        for (const auto& src : artifacts) {
            const std::string dst = patch::path_join(game_dir, patch::base_name(src));
            const fs::path src_path = fs::u8path(src);
            const fs::path dst_path = fs::u8path(dst);
            log_info("[*] copy " + patch::base_name(src) + " -> " + dst);
            if (!fs::equivalent(src_path, dst_path, ec)) {
                ec.clear();
                fs::copy_file(src_path, dst_path, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    return fail("!! copy failed: " + src + " -> " + dst + ": " + ec.message());
                }
            }
        }

        const std::string staged_game_dir = patch::find_staged_game_dir(project, exe_dir);
        try {
            patch::deploy_script_artifacts(project, game_dir, staged_game_dir);
        } catch (const std::exception& e) {
            return fail(std::string("!! script pak deploy failed: ") + e.what());
        }

        print_line("");
        log_info(std::string(kBannerWidth, '='));
        log_info("DONE.  Run: " + patch::path_join(game_dir, kLaunchExe));
        log_info(std::string(kBannerWidth, '='));
        return ksh::apps::kExitOk;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return ksh::apps::kExitError;
    }
}
