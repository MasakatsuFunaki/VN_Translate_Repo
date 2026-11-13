// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 03_patch_menus -- repack pac/cgsys.ypf with translated menu PNG sprites.
#include "apps/paths.h"
#include "common/util.h"
#include "menu_patcher/menu_patcher.h"

#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kExitOk = 0;

std::string resolve_default_patch_file() {
    std::vector<fs::path> candidates;
    const std::string project = frat::apps::default_project_dir();
    if (!project.empty()) {
        const fs::path proj_root = fs::u8path(project);
        candidates.push_back(proj_root / "game" / "cgsys_patch.bin");
        candidates.push_back(proj_root / "cgsys_patch.bin");
    }
    const std::string exe = frat::apps::exe_dir();
    if (!exe.empty()) {
        const fs::path exe_root = fs::u8path(exe);
        candidates.push_back(exe_root / ".." / "game" / "cgsys_patch.bin");
        candidates.push_back(exe_root / "cgsys_patch.bin");
    }
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate.u8string();
        }
    }
    return {};
}

}  // namespace

int main() {
    frat::setup_console_utf8();
    namespace po = frat::apps::po;

    try {
        std::string game;
        po::options_description desc(
            "03_patch_menus -- patch pac/cgsys.ypf menu sprites for FRATERNITE_HD");
        desc.add(frat::apps::game_only_options(game));

        po::variables_map vm;
        if (auto rc = frat::apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        const std::string patch_file = resolve_default_patch_file();
        if (patch_file.empty()) {
            throw std::runtime_error(
                "cannot find cgsys_patch.bin next to executable or project folder");
        }

        frat::menu::MenuArchivePatcher patcher;
        frat::menu::PatchConfig cfg;
        cfg.game_dir = game;
        cfg.patch_file_path = patch_file;

        const frat::menu::PatchSummary summary = patcher.patch_archive(cfg);
        frat::log_info("Patched " + summary.archive_path + ": replaced " +
                       frat::comma(static_cast<long long>(summary.replaced_entries)) +
                       " / " +
                       frat::comma(static_cast<long long>(summary.total_entries)) +
                       " entries (" +
                       frat::comma(static_cast<long long>(summary.discovered_assets)) +
                       " assets discovered)");
        return kExitOk;
    } catch (const std::exception& e) {
        frat::log_info(std::string("[ERROR] ") + e.what());
        return frat::apps::kExitError;
    }
}
