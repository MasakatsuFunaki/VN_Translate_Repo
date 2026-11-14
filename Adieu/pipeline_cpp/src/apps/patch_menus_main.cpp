// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CLI driver for 04_patch_menus:
//   * Developer UI inspection mode:
//       04_patch_menus --dump-ui [--only <Filename.g00>] --game-dir <GameDataDir>
//                      --dump-dir <ScratchDir>
//   * Menu patch container build mode:
//       04_patch_menus --game-dir <GameDataDir> [--out <MenuPatchBin>]
//                      [--contact-sheets <SheetDir>]

#include "apps/paths.h"
#include "common/util.h"
#include "menu/adieu_menu_pipeline.h"
#include "menu/domain_renderer.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;
namespace po = boost::program_options;

std::string ResolveGameDirArgument(const std::string& cli_game_dir) {
    if (!cli_game_dir.empty()) {
        return cli_game_dir;
    }
    const char* env_dir = std::getenv("VN_GAME_DIR");
    if (env_dir != nullptr && env_dir[0] != '\0') {
        return std::string(env_dir);
    }
    return {};
}

}  // namespace

int main() {
    adi::setup_console_utf8();

    std::string project_dir;
    std::string game_dir_arg;
    std::string out_patch_arg;
    std::string contact_sheets_arg;
    std::string dump_dir_arg;
    std::string only_filename_arg;
    bool dump_ui = false;
    bool dump_all_g00 = false;

    po::options_description desc = adi::apps::common_options(project_dir, game_dir_arg);
    desc.add_options()(
        "out", po::value<std::string>(&out_patch_arg),
        "Output path for keyless XOR-delta container (defaults to <dir>/menu_patch.bin)")(
        "contact-sheets", po::value<std::string>(&contact_sheets_arg),
        "Optional directory to emit Original | Patched | Amplified Diff QA contact sheets")(
        "dump-ui", po::bool_switch(&dump_ui),
        "Unpack candidate UI .g00 sprites and decrypted Gameexe.txt into --dump-dir")(
        "only", po::value<std::string>(&only_filename_arg),
        "Restrict --dump-ui to a single .g00 asset (for example ME_02.g00)")(
        "dump-all-g00", po::bool_switch(&dump_all_g00),
        "Include non-UI .g00 backgrounds and CGs during --dump-ui")(
        "dump-dir", po::value<std::string>(&dump_dir_arg),
        "Scratch output directory for --dump-ui");

    po::variables_map vm;
    if (const auto early_exit = adi::apps::parse_process_command_line(desc, vm)) {
        return *early_exit;
    }

    try {
        const std::string resolved_game_arg = ResolveGameDirArgument(game_dir_arg);
        if (resolved_game_arg.empty()) {
            std::cerr << "Error: --game-dir (or VN_GAME_DIR) is required.\n";
            return adi::apps::kExitError;
        }

        if (dump_ui) {
            if (dump_dir_arg.empty()) {
                std::cerr << "Error: --dump-dir is required when --dump-ui is specified.\n";
                return adi::apps::kExitError;
            }
            adi::menu::UiDumpOptions dump_opts;
            dump_opts.game_dir = fs::u8path(resolved_game_arg);
            dump_opts.dump_dir = fs::u8path(dump_dir_arg);
            dump_opts.only_filename = only_filename_arg;
            dump_opts.include_non_ui_g00 = dump_all_g00;

            const adi::menu::UiDumpSummary summary = adi::menu::DumpUiAssets(dump_opts);
            adi::log_info("Wrote Gameexe.txt -> " + summary.gameexe_txt_path.u8string());
            adi::log_info("Dumped " + adi::comma(static_cast<long long>(summary.g00_files_dumped)) +
                          " .g00 asset(s) (" +
                          adi::comma(static_cast<long long>(summary.png_images_written)) +
                          " PNG image(s)) into " + dump_opts.dump_dir.u8string());
            return adi::apps::kExitOk;
        }

        fs::path output_patch_path;
        if (!out_patch_arg.empty()) {
            output_patch_path = fs::u8path(out_patch_arg);
        } else if (!project_dir.empty()) {
            output_patch_path = fs::u8path(project_dir) / "menu_patch.bin";
        } else {
            std::cerr << "Error: --out (or --dir) is required to determine menu_patch.bin path.\n";
            return adi::apps::kExitError;
        }

        adi::menu::MenuPatchBuildOptions build_opts;
        build_opts.game_dir = fs::u8path(resolved_game_arg);
        build_opts.output_patch_path = output_patch_path;
        if (!contact_sheets_arg.empty()) {
            build_opts.contact_sheet_dir = fs::u8path(contact_sheets_arg);
        }

        const adi::menu::MenuPatchPipeline pipeline =
            adi::menu::CreateAdieuMenuPatchPipeline();
        const adi::menu::MenuPatchBuildSummary summary = pipeline.Run(build_opts);

        std::ostringstream hash_hex;
        hash_hex << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
                 << summary.manifest_hash;
        adi::log_info("Built " + output_patch_path.u8string() + " (" +
                      adi::comma(static_cast<long long>(summary.record_count)) + " record(s), " +
                      adi::comma(static_cast<long long>(summary.patch_bytes)) +
                      " bytes, manifest_hash=" + hash_hex.str() + ")");
        return adi::apps::kExitOk;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return adi::apps::kExitError;
    }
}
