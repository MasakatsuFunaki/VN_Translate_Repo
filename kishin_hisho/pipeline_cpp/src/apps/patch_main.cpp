// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 01_patch -- build helper_dll/ and deploy alpharom_helper.dll + launch.exe into
// the game folder.
//
// `--mode auto` keys off whether pre-built artifacts sit next to this executable:
//   DIST  <exe_dir>\helper_dll\{alpharom_helper.dll,launch.exe} both exist
//   DEV   otherwise -- configure and build <project>\helper_dll with CMake
// `--mode dev|dist` forces either one.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "common/util.h"
#include "patch/deploy.h"

#include "apps/paths.h"

namespace fs = std::filesystem;
using namespace ksh;

namespace {

constexpr const char* kHelperDll = "alpharom_helper.dll";
constexpr const char* kLaunchExe = "launch.exe";

std::string executable_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path().u8string();
}

// Both artifacts, in the order they are probed for and copied.
std::vector<std::string> artifacts_in(const std::string& dir) {
    return {patch::path_join(dir, kHelperDll), patch::path_join(dir, kLaunchExe)};
}

bool all_exist(const std::vector<std::string>& paths) {
    std::error_code ec;
    for (const auto& p : paths)
        if (!fs::exists(fs::u8path(p), ec)) return false;
    return true;
}

int fail(const std::string& msg) {
    std::cerr << msg << std::endl;
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = ksh::apps::po;

    std::string project, game_dir, mode;
    bool clean = false;
    po::options_description desc("01_patch -- build and deploy the AlphaROM helper");
    desc.add(ksh::apps::common_options(project));
    desc.add_options()
        ("game-dir", po::value(&game_dir)->required(),
         "Game install directory")
        ("clean", po::bool_switch(&clean), "Wipe the CMake build dir first (dev mode only)")
        ("mode", po::value(&mode)->default_value("auto"),
         "auto | dev (build from source) | dist (use bundled artifacts)");
    po::variables_map vm;
    if (auto rc = ksh::apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    if (mode != "auto" && mode != "dev" && mode != "dist")
        return fail("!! --mode must be auto, dev or dist (got '" + mode + "')");

    const std::string exe_dir = executable_dir();
    const std::string dist_helper_dir = patch::path_join(exe_dir, "helper_dll");
    const std::string helper_dir = patch::path_join(project, "helper_dll");
    // The game has ONE build folder with a subtree per architecture, so the
    // Win32 half builds into <project>\build\helper -- the same tree build.py
    // fills, not a build/ of its own inside helper_dll.
    const std::string build_dir =
        patch::path_join(patch::path_join(project, "build"), "helper");

    const bool is_dist =
        mode == "dist" || (mode == "auto" && all_exist(artifacts_in(dist_helper_dir)));

    log_info("[*] script:   " + (is_dist ? exe_dir : project));
    log_info(std::string("[*] mode:     ") +
             (is_dist ? "DIST (bundled artifacts)" : "DEV (build from source)"));
    log_info("[*] game dir: " + game_dir);
    print_line("");

    if (!patch::can_write(game_dir)) {
        log_info("!! Cannot write to " + game_dir + ".");
        if (!patch::is_admin()) {
            log_info("!! Re-run from an elevated PowerShell:");
            log_info("!!   right-click PowerShell -> 'Run as Administrator'");
        } else {
            log_info("!! (You are admin but the directory is still not writable.");
            log_info("!!  Check the path or pass --game-dir.)");
        }
        return 1;
    }

    std::error_code ec;
    if (!is_dist) {
        if (!fs::exists(fs::u8path(patch::path_join(helper_dir, "CMakeLists.txt")), ec))
            return fail("!! helper_dll/CMakeLists.txt not found at " + helper_dir);

        if (clean && fs::is_directory(fs::u8path(build_dir), ec)) {
            log_info("[*] removing existing build dir: " + build_dir);
            fs::remove_all(fs::u8path(build_dir), ec);
            if (ec) return fail("!! could not remove " + build_dir + ": " + ec.message());
        }
        fs::create_directories(fs::u8path(build_dir), ec);

        // This shares <project>\build\helper with the root build, so it has to
        // configure that tree exactly the way the root does or CMake refuses
        // the second one for generator mismatch.  Conan writes the toolchain
        // and, because the helper is 32-bit while Ninja takes its target
        // architecture from the compiler environment rather than from CMake,
        // also the conanvcvars.bat that selects the 32-bit toolchain.
        const std::string profile =
            patch::path_join(patch::path_join(helper_dir, "profiles"), "x86");
        const std::vector<std::string> conan_cmd{
            "conan", "install", helper_dir, "--output-folder=" + build_dir,
            "--build=missing", "-pr=" + profile, "-s", "build_type=Release",
            "-c", "tools.cmake.cmaketoolchain:generator=Ninja Multi-Config"};
        log_info("$ " + patch::command_display(conan_cmd));
        if (const int rc = patch::run(conan_cmd); rc != 0)
            return fail("!! command failed (exit " + std::to_string(rc) + ")");

        // A batch file cannot export its environment back into the calling
        // process, so the configure and the build run inside a wrapper that
        // applies conanvcvars.bat first -- the same shape the root CMakeLists
        // uses for this half.  `cd /d` is needed because `cmake --preset`
        // reads the preset files from the current working directory.
        const std::string wrapper = patch::path_join(build_dir, "patch-build.bat");
        {
            std::ofstream bat(fs::u8path(wrapper), std::ios::binary);
            if (!bat) return fail("!! could not write " + wrapper);
            bat << "@echo off\r\n"
                << "call \""
                << patch::path_join(patch::path_join(build_dir, "generators"),
                                    "conanvcvars.bat")
                << "\" || exit /b 1\r\n"
                << "cd /d \"" << helper_dir << "\" || exit /b 1\r\n"
                << "cmake --preset conan-windows-x86-release || exit /b 1\r\n"
                << "cmake --build \"" << build_dir << "\" --config Release\r\n";
            if (!bat) return fail("!! could not write " + wrapper);
        }
        const std::vector<std::string> build_cmd{wrapper};
        log_info("$ " + patch::command_display(build_cmd));
        if (const int rc = patch::run(build_cmd); rc != 0)
            return fail("!! command failed (exit " + std::to_string(rc) + ")");
    }

    const std::string artifact_dir =
        is_dist ? dist_helper_dir : patch::path_join(build_dir, "Release");
    const std::vector<std::string> artifacts = artifacts_in(artifact_dir);
    for (const auto& p : artifacts)
        if (!fs::exists(fs::u8path(p), ec)) return fail("!! artifact missing: " + p);

    if (!fs::is_directory(fs::u8path(game_dir), ec))
        return fail("!! game dir does not exist: " + game_dir);
    for (const auto& src : artifacts) {
        const std::string dst = patch::path_join(game_dir, patch::base_name(src));
        log_info("[*] copy " + patch::base_name(src) + " -> " + dst);
        fs::copy_file(fs::u8path(src), fs::u8path(dst), fs::copy_options::overwrite_existing, ec);
        if (ec) return fail("!! copy failed: " + src + " -> " + dst + ": " + ec.message());
    }

    try {
        patch::deploy_script_artifacts(project, game_dir, patch::path_join(exe_dir, "game"));
    } catch (const std::exception& e) {
        return fail(std::string("!! script pak deploy failed: ") + e.what());
    }

    print_line("");
    log_info(std::string(60, '='));
    log_info("DONE.  Run: " + patch::path_join(game_dir, kLaunchExe));
    log_info(std::string(60, '='));
    return 0;
}
