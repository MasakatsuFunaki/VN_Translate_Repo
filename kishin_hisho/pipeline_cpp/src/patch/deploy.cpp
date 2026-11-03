// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "deploy.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "common/util.h"
#include "pak/nps_rebuilder.h"

namespace fs = std::filesystem;

namespace ksh::patch {

namespace {

bool ends_with_separator(const std::string& s) {
    return !s.empty() && (s.back() == '\\' || s.back() == '/');
}

// Quote an argument when flattening a vector into a command line: only when it
// contains a space, which is all CreateProcessW needs to keep the argument
// whole (the CMake paths that need it never contain quotes of their own).
std::string quote_if_needed(const std::string& arg) {
    return arg.find(' ') == std::string::npos ? arg : "\"" + arg + "\"";
}

}  // namespace

std::string path_join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    // A bare drive letter ("C:") is its own case: "C:" + "x" is the
    // drive-relative "C:x", not the rooted "C:\\x".
    if (ends_with_separator(dir) || (dir.size() == 2 && dir[1] == ':')) return dir + name;
    return dir + "\\" + name;
}

std::string base_name(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool can_write(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(dir), ec)) return false;
    const fs::path probe = fs::u8path(path_join(dir, ".__patch_write_probe__"));
    {
        std::ofstream f(probe, std::ios::binary);
        if (!f) return false;
        f.put('x');
        if (!f) return false;
    }
    fs::remove(probe, ec);
    return true;
}

bool is_admin() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool ok =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != 0;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::string command_display(const std::vector<std::string>& cmd) {
    std::string out;
    for (std::size_t i = 0; i < cmd.size(); ++i) {
        if (i) out += ' ';
        out += quote_if_needed(cmd[i]);
    }
    return out;
}

int run(const std::vector<std::string>& cmd, const std::string& cwd) {
    // CreateProcess needs every argument containing a space quoted, including
    // the program itself -- the same rule the display form uses.
    const std::string line = command_display(cmd);
    std::fflush(stdout);

    std::wstring wline(line.size() + 1, L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wline.data(),
                                      static_cast<int>(wline.size()));
    if (n <= 0) return -1;
    wline.resize(static_cast<std::size_t>(n) - 1);

    std::wstring wcwd;
    if (!cwd.empty()) {
        const int c = MultiByteToWideChar(CP_UTF8, 0, cwd.c_str(), -1, nullptr, 0);
        if (c <= 0) return -1;
        wcwd.resize(static_cast<std::size_t>(c) - 1);
        MultiByteToWideChar(CP_UTF8, 0, cwd.c_str(), -1, wcwd.data(), c);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, wline.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

void deploy_script_artifacts(const std::string& project_dir,
                             const std::string& game_dir,
                             const std::string& dist_game_dir) {
    std::error_code ec;
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

    if (dist_game_dir.empty()) {
        return;
    }
    const std::string src_pak = path_join(dist_game_dir, "0script.pak");
    if (fs::exists(fs::u8path(src_pak), ec)) {
        const std::string dst_pak = path_join(game_dir, "0script.pak");
        fs::copy_file(fs::u8path(src_pak), fs::u8path(dst_pak),
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            throw std::runtime_error("copy failed: " + src_pak + " -> " + dst_pak + ": " +
                                     ec.message());
        }
        log_info("[*] copy 0script.pak -> " + dst_pak);
    }

    const std::string src_script_dir = path_join(dist_game_dir, "script");
    if (fs::is_directory(fs::u8path(src_script_dir), ec)) {
        const std::string dst_script_dir = path_join(game_dir, "script");
        fs::create_directories(fs::u8path(dst_script_dir), ec);
        for (const auto& entry : fs::directory_iterator(fs::u8path(src_script_dir), ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const fs::path dst_file = fs::u8path(dst_script_dir) / entry.path().filename();
            fs::copy_file(entry.path(), dst_file, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                throw std::runtime_error("copy failed: " + entry.path().u8string() + " -> " +
                                         dst_file.u8string() + ": " + ec.message());
            }
        }
        log_info("[*] copy script\\* -> " + dst_script_dir);
    }
}

}  // namespace ksh::patch
