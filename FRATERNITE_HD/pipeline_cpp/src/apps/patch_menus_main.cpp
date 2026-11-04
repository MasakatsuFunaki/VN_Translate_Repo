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
#include <vector>

namespace {

std::string resolve_default_patch_file(const std::string& project,
                                       const std::string& game,
                                       const char* argv0) {
    std::vector<std::filesystem::path> candidates;
    if (!project.empty()) {
        candidates.push_back(std::filesystem::u8path(project) / "cgsys_patch.bin");
    }
    if (!game.empty()) {
        candidates.push_back(std::filesystem::u8path(game) / "cgsys_patch.bin");
    }
    if (argv0 != nullptr && argv0[0] != '\0') {
        const std::filesystem::path exe_dir =
            std::filesystem::u8path(argv0).parent_path();
        if (!exe_dir.empty()) {
            candidates.push_back(exe_dir / ".." / "game" / "cgsys_patch.bin");
        }
    }
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate.u8string();
        }
    }
    if (!project.empty()) {
        return (std::filesystem::u8path(project) / "cgsys_patch.bin").u8string();
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    frat::setup_console_utf8();
    namespace po = frat::apps::po;

    std::string project;
    std::string game;
    std::string assets_dir;
    std::string patch_file;
    std::string orig_ypf;
    std::string patched_ypf;
    bool build_patch = false;

    po::options_description desc(
        "03_patch_menus -- patch pac/cgsys.ypf menu sprites for FRATERNITE_HD");
    desc.add(frat::apps::common_options(project, game));
    desc.add_options()(
        "assets-dir", po::value(&assets_dir),
        "translated menu assets directory (default: <dir>\\menu_assets)")(
        "patch-file", po::value(&patch_file),
        "XOR delta menu patch file (default: <dir>\\cgsys_patch.bin)")(
        "build-patch", po::bool_switch(&build_patch),
        "build XOR delta patch file from --orig-ypf and --patched-ypf")(
        "orig-ypf", po::value(&orig_ypf),
        "original unpatched cgsys.ypf(.bak) for --build-patch")(
        "patched-ypf", po::value(&patched_ypf),
        "patched cgsys.ypf for --build-patch");

    po::variables_map vm;
    if (auto rc = frat::apps::parse_command_line(argc, argv, desc, vm)) {
        return *rc;
    }

    if (assets_dir.empty()) {
        assets_dir = project + "\\menu_assets";
    }
    if (patch_file.empty()) {
        patch_file = resolve_default_patch_file(
            project, game, argc > 0 ? argv[0] : nullptr);
    }

    try {
        frat::menu::MenuArchivePatcher patcher;
        if (build_patch || (!orig_ypf.empty() && !patched_ypf.empty())) {
            if (orig_ypf.empty() || patched_ypf.empty() || patch_file.empty()) {
                throw std::runtime_error(
                    "--build-patch requires --orig-ypf, --patched-ypf, and --patch-file");
            }
            const frat::Bytes orig_bytes = frat::read_file(orig_ypf);
            const frat::Bytes patched_bytes = frat::read_file(patched_ypf);
            const frat::Bytes patch_blob =
                patcher.build_xor_patch_bytes(orig_bytes, patched_bytes);
            frat::menu::PatchSummary verify_summary;
            const frat::Bytes reconstructed =
                patcher.apply_xor_patch_bytes(orig_bytes, patch_blob, &verify_summary);
            if (reconstructed != patched_bytes) {
                throw std::runtime_error(
                    "XOR patch round-trip verification failed: reconstructed archive differs");
            }
            frat::write_file(patch_file, patch_blob);
            frat::log_info("Built and verified " + patch_file + " (" +
                           frat::comma(static_cast<long long>(patch_blob.size())) +
                           " bytes, " +
                           frat::comma(static_cast<long long>(verify_summary.replaced_entries)) +
                           " / " +
                           frat::comma(static_cast<long long>(verify_summary.total_entries)) +
                           " entries)");
            return 0;
        }

        frat::menu::PatchConfig cfg;
        cfg.game_dir = game;
        cfg.assets_dir = assets_dir;
        cfg.patch_file_path = patch_file;

        const frat::menu::PatchSummary summary = patcher.patch_archive(cfg);
        frat::log_info("Patched " + summary.archive_path + ": replaced " +
                       frat::comma(static_cast<long long>(summary.replaced_entries)) +
                       " / " +
                       frat::comma(static_cast<long long>(summary.total_entries)) +
                       " entries (" +
                       frat::comma(static_cast<long long>(summary.discovered_assets)) +
                       " assets discovered)");
        return 0;
    } catch (const std::exception& e) {
        frat::log_info(std::string("[ERROR] ") + e.what());
        return 1;
    }
}
