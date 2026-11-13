// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "patch/deploy.h"

#include "common/util.h"
#include "pak/nps_rebuilder.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace ksh::patch {

namespace {

constexpr std::size_t kDrivePrefixLength = 2;
constexpr const char* kHelperDllName = "alpharom_helper.dll";
constexpr const char* kLaunchExeName = "launch.exe";
constexpr const char* kScriptPakName = "0script.pak";
constexpr const char* kScriptSubdirName = "script";

bool ends_with_separator(const std::string& s) {
    return !s.empty() && (s.back() == '\\' || s.back() == '/');
}

std::string parent_game_dir_of(const std::string& exe_dir) {
    if (exe_dir.empty()) {
        return {};
    }
    const fs::path exe_path = fs::u8path(exe_dir);
    const fs::path parent = exe_path.parent_path();
    if (parent.empty() || parent == exe_path) {
        return {};
    }
    return path_join(parent.u8string(), "game");
}

bool has_both_helper_artifacts(const std::string& dir) {
    if (dir.empty()) {
        return false;
    }
    std::error_code ec;
    return fs::is_regular_file(fs::u8path(path_join(dir, kHelperDllName)), ec) &&
           fs::is_regular_file(fs::u8path(path_join(dir, kLaunchExeName)), ec);
}

std::vector<std::string> staged_game_candidates(const std::string& project_dir,
                                                const std::string& exe_dir) {
    std::vector<std::string> candidates;
    if (!project_dir.empty()) {
        candidates.push_back(path_join(project_dir, "game"));
    }
    if (const std::string sibling = parent_game_dir_of(exe_dir); !sibling.empty()) {
        candidates.push_back(sibling);
    }
    if (!project_dir.empty()) {
        candidates.push_back(
            path_join(path_join(path_join(project_dir, "build"), "install"), "game"));
    }
    return candidates;
}

}  // namespace

std::string path_join(const std::string& dir, const std::string& name) {
    if (dir.empty()) {
        return name;
    }
    // A bare drive letter ("C:") is its own case: "C:" + "x" is the
    // drive-relative "C:x", not the rooted "C:\\x".
    if (ends_with_separator(dir) || (dir.size() == kDrivePrefixLength && dir[1] == ':')) {
        return dir + name;
    }
    return dir + "\\" + name;
}

std::string base_name(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

bool can_write(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(dir), ec)) {
        return false;
    }
    const fs::path probe = fs::u8path(path_join(dir, ".__patch_write_probe__"));
    {
        std::ofstream f(probe, std::ios::binary);
        if (!f) {
            return false;
        }
        f.put('x');
        if (!f) {
            return false;
        }
    }
    fs::remove(probe, ec);
    return true;
}

bool is_admin() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool ok =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != 0;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::string find_helper_artifact_dir(const std::string& project_dir,
                                     const std::string& exe_dir) {
    std::vector<std::string> candidates = staged_game_candidates(project_dir, exe_dir);
    if (!project_dir.empty()) {
        candidates.push_back(
            path_join(path_join(path_join(project_dir, "build"), "helper"), "Release"));
    }
    if (!exe_dir.empty()) {
        candidates.push_back(path_join(exe_dir, "helper_dll"));
    }

    for (const auto& candidate : candidates) {
        if (has_both_helper_artifacts(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::string find_staged_game_dir(const std::string& project_dir,
                                 const std::string& exe_dir) {
    const std::vector<std::string> candidates = staged_game_candidates(project_dir, exe_dir);
    std::error_code ec;

    for (const auto& candidate : candidates) {
        if (fs::is_regular_file(fs::u8path(path_join(candidate, kScriptPakName)), ec)) {
            return candidate;
        }
    }
    for (const auto& candidate : candidates) {
        if (fs::is_directory(fs::u8path(path_join(candidate, kScriptSubdirName)), ec)) {
            return candidate;
        }
    }
    for (const auto& candidate : candidates) {
        if (fs::is_directory(fs::u8path(candidate), ec)) {
            return candidate;
        }
    }
    return {};
}

void deploy_script_artifacts(const std::string& project_dir,
                             const std::string& game_dir,
                             const std::string& dist_game_dir) {
    std::error_code ec;
    if (!project_dir.empty()) {
        const std::string script_out = path_join(project_dir, "script_output");
        const std::string translated_json = path_join(script_out, "translated_text.json");
        const std::string recon_json = path_join(script_out, "nps_reconstruction_map.json");

        if (fs::exists(fs::u8path(translated_json), ec) &&
            fs::exists(fs::u8path(recon_json), ec)) {
            const pak::NpsRebuilder rebuilder;
            const pak::NpsRebuildStats stats =
                rebuilder.RebuildAndDeploy(project_dir, {game_dir}, true);
            log_info("[*] rebuilt 0script.pak (" + std::to_string(stats.total_pak_files) +
                     " files, " + std::to_string(stats.pak_size_bytes) +
                     " bytes) and loose script\\*.nps -> " + game_dir);
            return;
        }
    }

    if (dist_game_dir.empty()) {
        return;
    }
    const fs::path src_pak_path = fs::u8path(path_join(dist_game_dir, kScriptPakName));
    if (fs::exists(src_pak_path, ec)) {
        const std::string dst_pak = path_join(game_dir, kScriptPakName);
        const fs::path dst_pak_path = fs::u8path(dst_pak);
        if (!fs::equivalent(src_pak_path, dst_pak_path, ec)) {
            ec.clear();
            fs::copy_file(src_pak_path, dst_pak_path, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                throw std::runtime_error("copy failed: " + src_pak_path.u8string() + " -> " +
                                         dst_pak + ": " + ec.message());
            }
        }
        log_info("[*] copy 0script.pak -> " + dst_pak);
    }

    const fs::path src_script_path = fs::u8path(path_join(dist_game_dir, kScriptSubdirName));
    if (fs::is_directory(src_script_path, ec)) {
        const std::string dst_script_dir = path_join(game_dir, kScriptSubdirName);
        const fs::path dst_script_path = fs::u8path(dst_script_dir);
        fs::create_directories(dst_script_path, ec);
        for (const auto& entry : fs::directory_iterator(src_script_path, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const fs::path dst_file = dst_script_path / entry.path().filename();
            if (!fs::equivalent(entry.path(), dst_file, ec)) {
                ec.clear();
                fs::copy_file(entry.path(), dst_file, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    throw std::runtime_error("copy failed: " + entry.path().u8string() + " -> " +
                                             dst_file.u8string() + ": " + ec.message());
                }
            }
        }
        log_info("[*] copy script\\* -> " + dst_script_dir);
    }
}

}  // namespace ksh::patch
