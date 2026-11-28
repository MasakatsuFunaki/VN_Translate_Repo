// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 00_run_all -- Adieu English translation pipeline orchestrator.
//
//   00_run_all --game-dir <dir>                  # the whole pipeline
//   00_run_all --game-dir <dir> --max-batches N  # cap the run at N API requests
//   00_run_all --game-dir <dir> --batch N        # lines per API request
//   00_run_all --game-dir <dir> --skip-menus     # skip rebuilding menu_patch.bin
//   00_run_all --game-dir <dir> --clean          # delete cached JSON, then run
//
// Sequence: 01_extract -> 03_speaker_gate -> 02_translate -> 04_patch_menus ->
// deploy.  The translation stage builds translation_table.tsv, 04_patch_menus
// builds menu_patch.bin when Gameexe.dat and g00/ are present, and deploy ships
// winmm.dll, translation_table.tsv, and menu_patch.bin into the game folder.
#include "apps/paths.h"
#include "common/util.h"
#include "translate/gemini_client.h"
#include "translate/translate_core.h"

#include <cstddef>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace adi;

namespace {

constexpr int kDefaultBatchSize = vn::llm::kDefaultBatchSize;
constexpr int kUnlimitedBatches = 0;
constexpr int kDisabledTestBatchCap = 0;
constexpr std::size_t kBannerWidth = 60;

std::string format_command_for_log(const std::string& exe_path,
                                   const std::vector<std::string>& args) {
    std::string line = "\"" + exe_path + "\"";
    for (const auto& arg : args) {
        if (arg.empty() || arg.find_first_of(" \t\"") != std::string::npos) {
            line += " \"" + arg + "\"";
            continue;
        }
        line += " " + arg;
    }
    return line;
}

bool run_child_tool(const std::string& exe_stem,
                    const std::vector<std::string>& args) {
    const std::string exe_path = apps::exe_dir() + "\\" + exe_stem + ".exe";
    print_line("Running: " + format_command_for_log(exe_path, args));
    if (apps::run_process_utf8(exe_path, args) != apps::kExitOk) {
        log_info("\n[ERROR] " + exe_stem + " failed");
        return false;
    }
    return true;
}

std::optional<fs::path> find_game_data_dir_with_menus(const fs::path& base_game_dir) {
    std::error_code ec;
    for (const fs::path& rel : {fs::path("."), fs::path("StartData") / "GameData",
                                fs::path("GameData")}) {
        const fs::path cand = (rel == fs::path(".")) ? base_game_dir : (base_game_dir / rel);
        if (fs::is_regular_file(cand / "Gameexe.dat", ec) &&
            fs::is_directory(cand / "g00", ec)) {
            return cand;
        }
    }
    return std::nullopt;
}

fs::path resolve_deploy_target_dir(const fs::path& base_game_dir) {
    std::error_code ec;
    for (const fs::path& rel : {fs::path("."), fs::path("StartData") / "GameData",
                                fs::path("GameData")}) {
        const fs::path cand = (rel == fs::path(".")) ? base_game_dir : (base_game_dir / rel);
        if (fs::is_regular_file(cand / "SiglusEngine.exe", ec) ||
            fs::is_regular_file(cand / "Gameexe.dat", ec)) {
            return cand;
        }
    }
    return base_game_dir;
}

fs::path first_existing_path(std::initializer_list<std::string> candidates) {
    for (const std::string& cand : candidates) {
        std::error_code ec;
        const fs::path p = fs::u8path(cand);
        if (fs::exists(p, ec)) {
            return p;
        }
        if (ec) {
            log_info("  WARN: cannot check " + cand + ": " + ec.message());
        }
    }
    return {};
}

// Ship the runtime artifacts the game reads (winmm.dll, translation_table.tsv,
// and menu_patch.bin).  Nothing is rebuilt here: a rebuild would relink the
// very executable running this, and the copy would then fail on a locked
// binary whenever a pipeline source had changed.
bool deploy(const std::string& project, const std::string& game) {
    if (const wchar_t* dist = _wgetenv(L"VN_DIST_BUILD");
        dist != nullptr && *dist != L'\0') {
        log_info("[DIST] VN_DIST_BUILD set -- skipping the copy to the game folder.");
        return true;
    }

    const fs::path deploy_dir = resolve_deploy_target_dir(fs::u8path(game));
    const std::string deploy_dir_utf8 = deploy_dir.u8string();

    const fs::path dll = first_existing_path({
        project + "\\build\\proxy\\Release\\winmm.dll",
        apps::exe_dir() + "\\..\\game\\winmm.dll",
    });
    if (dll.empty()) {
        log_info("[ERROR] winmm.dll not found -- build the proxy half first "
                 "(python build.py).");
        return false;
    }

    std::error_code ec;
    fs::copy_file(dll, deploy_dir / "winmm.dll",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_info("[ERROR] copying winmm.dll failed (" + ec.message() +
                 ") -- is the game running?  Close it and retry.");
        return false;
    }
    log_info("  winmm.dll -> " + deploy_dir_utf8);

    const fs::path tsv = first_existing_path({
        project + "\\translation_table.tsv",
        apps::exe_dir() + "\\..\\game\\translation_table.tsv",
    });
    if (tsv.empty()) {
        log_info("  WARN: translation_table.tsv not found -- 02_translate produces it.");
    } else {
        fs::copy_file(tsv, deploy_dir / "translation_table.tsv",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            log_info("[ERROR] copying translation_table.tsv failed (" + ec.message() + ")");
            return false;
        }
        log_info("  translation_table.tsv -> " + deploy_dir_utf8);
    }

    const fs::path menu_patch = first_existing_path({
        project + "\\menu_patch.bin",
        apps::exe_dir() + "\\..\\game\\menu_patch.bin",
    });
    if (menu_patch.empty()) {
        log_info("  WARN: menu_patch.bin not found -- 04_patch_menus produces it.");
    } else {
        fs::copy_file(menu_patch, deploy_dir / "menu_patch.bin",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            log_info("[ERROR] copying menu_patch.bin failed (" + ec.message() + ")");
            return false;
        }
        // Clear any stale applied sentinel so the proxy DLL re-evaluates the
        // deployed manifest hash on next launch.
        std::error_code rm_ec;
        fs::remove(deploy_dir / "menu_patch.applied", rm_ec);
        log_info("  menu_patch.bin -> " + deploy_dir_utf8);
    }

    return true;
}

}  // namespace

int main() {
    setup_console_utf8();
    try {
        namespace po = apps::po;

        std::string project;
        std::string game;
        int batch = kDefaultBatchSize;
        int max_batches = kUnlimitedBatches;
        bool clean = false;
        bool discard_cache = false;
        bool skip_menus = false;
        po::options_description desc("00_run_all -- Adieu translation pipeline");
        desc.add(apps::common_options(project, game));
        desc.add_options()
            ("batch", po::value(&batch)->default_value(kDefaultBatchSize),
             "lines per API request, passed to the translation stage")
            ("max-batches", po::value(&max_batches)->default_value(kUnlimitedBatches),
             "stop after N API requests, keeping the cache and outputs (0 = no limit)")
            ("skip-menus", po::bool_switch(&skip_menus),
             "skip rebuilding menu_patch.bin via 04_patch_menus")
            ("clean", po::bool_switch(&clean), "delete cached JSON files before running")
            ("discard-cache", po::bool_switch(&discard_cache),
             "allow --clean to delete a cache that already holds paid work");
        po::variables_map vm;
        if (auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        if (auto why = translate::validate_options(batch, kDisabledTestBatchCap, max_batches)) {
            log_info("[ERROR] " + *why);
            return apps::kExitRefused;
        }

        const std::string out_dir = project + "\\script_output";
        const std::string cache_file = out_dir + "\\translation_cache_gemini.json";
        if (clean) {
            try {
                if (auto why = translate::refuse_cache_discard(
                        translate::cache_entry_count(cache_file),
                        "--clean", discard_cache)) {
                    log_info("[ERROR] " + *why);
                    return apps::kExitRefused;
                }
            } catch (const std::exception& e) {
                log_info(std::string("[ERROR] ") + e.what());
                return apps::kExitRefused;
            }
        }

        const std::string bar(kBannerWidth, '=');
        log_info(bar);
        log_info("Adieu");
        log_info("English Translation Pipeline (Gemini)");
        log_info(bar);

        if (clean && gemini::load_api_key().empty()) {
            log_info("[ERROR] GEMINI_API_KEY not set -- refusing to discard the "
                     "cache for a run that cannot translate.");
            return apps::kExitRefused;
        }

        if (clean) {
            for (const char* f : {"extracted_text.json", "translated_text.json",
                                  "translation_cache_gemini.json"}) {
                fs::path p = fs::u8path(out_dir + "\\" + f);
                std::error_code ec;
                const bool present = fs::exists(p, ec);
                if (!ec && !present) {
                    continue;
                }
                const bool removed = !ec && fs::remove(p, ec);
                if (ec) {
                    log_info("[ERROR] cannot delete " + out_dir + "\\" + f + ": " +
                             ec.message() +
                             " -- close whatever has it open, then retry.");
                    return apps::kExitError;
                }
                log_info(std::string("[clean] ") + f +
                         (removed ? "" : " was already gone"));
            }
        }

        log_info("\n" + bar);
        log_info("STEP 1: Extract text and attribute speakers from Scene.pck");
        log_info(bar);
        if (!run_child_tool("01_extract", {"--dir", project, "--game-dir", game})) {
            return apps::kExitError;
        }

        log_info("\n" + bar);
        log_info("STEP 2: Verify speaker attribution and glossary coverage");
        log_info(bar);
        if (!run_child_tool("03_speaker_gate", {"--dir", project})) {
            return apps::kExitError;
        }

        log_info("\n" + bar);
        log_info("STEP 3: Translate via Gemini, build translation_table.tsv");
        log_info(bar);
        {
            std::vector<std::string> args = {
                "--dir", project, "--batch", std::to_string(batch)};
            if (max_batches > 0) {
                args.emplace_back("--max-batches");
                args.emplace_back(std::to_string(max_batches));
            }
            if (!run_child_tool("02_translate", args)) {
                return apps::kExitError;
            }
        }

        log_info("\n" + bar);
        log_info("STEP 4: Build menu_patch.bin (Gameexe.dat + .g00 UI sprites)");
        log_info(bar);
        if (skip_menus) {
            log_info("[SKIP] --skip-menus set -- skipping 04_patch_menus.");
        } else if (const auto menu_game_dir =
                       find_game_data_dir_with_menus(fs::u8path(game))) {
            const std::string out_patch = project + "\\menu_patch.bin";
            if (!run_child_tool("04_patch_menus",
                                {"--dir", project, "--game-dir",
                                 menu_game_dir->u8string(), "--out", out_patch})) {
                return apps::kExitError;
            }
        } else {
            log_info("[SKIP] Gameexe.dat or g00/ not present under --game-dir -- "
                     "skipping 04_patch_menus.");
        }

        log_info("\n" + bar);
        log_info("STEP 5: Deploy winmm.dll, translation_table.tsv, and menu_patch.bin");
        log_info(bar);
        if (!deploy(project, game)) {
            return apps::kExitError;
        }

        log_info("\n" + bar);
        log_info("TRANSLATION COMPLETE!");
        log_info(bar);
        return apps::kExitOk;
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return apps::kExitError;
    }
}
