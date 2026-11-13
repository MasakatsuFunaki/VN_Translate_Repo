// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 00_run_all -- さくら、もゆ。 -as the Night's, Reincarnation- (FAVORITE / FVP)
// English translation pipeline orchestrator.
//
//   00_run_all --game-dir <dir>                  # the whole pipeline
//   00_run_all --game-dir <dir> --max-batches N  # cap the run at N API requests
//   00_run_all --game-dir <dir> --batch N        # lines per API request
//   00_run_all --game-dir <dir> --clean          # delete cached JSONs, then run the pipeline
//
// Sequence: 01_extract -> 02_translate -> 03_patch_ui -> deploy.  The
// translation stage carries the cast-roster gate and the runtime table with
// it: everything that must happen for dialogue to show English happens inside
// the command that pays for it.
#include "apps/paths.h"
#include "apps/pipeline_steps.h"
#include "common/util.h"
#include "translate/gemini_client.h"
#include "translate/translate_core.h"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace fav;

namespace {

constexpr int kDefaultBatchArg = 100;
constexpr int kNoBatchLimit = 0;
constexpr int kNoTestBatches = 0;
constexpr int kRefusedExitCode = 2;
constexpr std::size_t kBannerWidth = 60;
constexpr std::size_t kDisplayIndexOffset = 1;
constexpr std::size_t kStageExtract = 0;
constexpr std::size_t kStageTranslate = 1;
constexpr std::size_t kStagePatchUi = 2;
constexpr std::size_t kStageDeploy = 3;

bool run_stage(const std::string& exe, const std::vector<std::string>& args) {
    const std::string dir = apps::exe_dir();
    if (dir.empty()) {
        log_info("[ERROR] cannot determine executable directory for " + exe);
        return false;
    }
    const fs::path exe_path = fs::u8path(dir) / (exe + ".exe");
    if (apps::run_child_process(exe_path, args) != apps::kExitOk) {
        log_info("\n[ERROR] " + exe + " failed");
        return false;
    }
    return true;
}

// Ship the artifact the game reads.  Nothing is rebuilt here: a rebuild would
// relink the very executable running this, and the copy would then fail on a
// locked binary whenever a pipeline source had changed.
//
// The DLL is the only file to copy.  Unlike the other games in this repo the
// translation table is written straight into the game folder by the
// translation stage, so there is nothing to move -- the check below is a
// reminder that the stage has not run yet, not a copy.
bool deploy(const std::string& project, const std::string& game) {
    if (const wchar_t* dist = _wgetenv(L"VN_DIST_BUILD");
        dist != nullptr && *dist != L'\0') {
        log_info("[DIST] VN_DIST_BUILD set -- skipping the copy to the game folder.");
        return true;
    }

    // The whole game builds into one tree, so the DLL is in build/proxy;
    // when this executable is the staged copy under build/install/bin, the
    // DLL is in the sibling game/ folder instead.
    std::error_code ec;
    fs::path dll;
    for (const auto& candidate : {project + "\\build\\proxy\\Release\\winmm.dll",
                                  apps::exe_dir() + "\\..\\game\\winmm.dll"}) {
        const fs::path candidate_path = fs::u8path(candidate);
        if (fs::exists(candidate_path, ec)) {
            dll = candidate_path;
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
                 ") -- is the game running?  Close it and retry.");
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
                 "02_translate writes it there.");
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
        int batch = 0;
        int max_batches = 0;
        bool clean = false;
        bool discard_cache = false;
        po::options_description desc(
            "00_run_all -- FAVORITE (Sakura, Moyu.) translation pipeline");
        desc.add(apps::common_options(project, game));
        desc.add_options()
            ("batch", po::value(&batch)->default_value(kDefaultBatchArg),
             "lines per API request, passed to the translation stage")
            ("max-batches", po::value(&max_batches)->default_value(kNoBatchLimit),
             "stop after N API requests, keeping the cache and outputs (0 = no limit)")
            ("clean", po::bool_switch(&clean), "delete cached JSON files before running")
            ("discard-cache", po::bool_switch(&discard_cache),
             "allow --clean to delete a cache that already holds paid work");
        po::variables_map vm;
        if (const auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        // The caps are validated here as well as in the translation stage,
        // because this is the command that starts an unattended full run: a
        // mistyped cap must fail before a key is read and before the bytecode
        // is touched.
        if (const auto why = translate::validate_options(batch, kNoTestBatches, max_batches)) {
            log_info("[ERROR] " + *why);
            return kRefusedExitCode;
        }

        const std::string out_dir = project + "\\script_output";
        const std::string cache_file = out_dir + "\\translation_cache_gemini.json";
        if (clean) {
            try {
                if (const auto why = translate::refuse_cache_discard(
                        translate::cache_entry_count(cache_file),
                        "--clean", discard_cache)) {
                    log_info("[ERROR] " + *why);
                    return kRefusedExitCode;
                }
            } catch (const std::exception& e) {
                log_info(std::string("[ERROR] ") + e.what());
                return kRefusedExitCode;
            }
        }

        const std::string bar(kBannerWidth, '=');
        log_info(bar);
        log_info("さくら、もゆ。 -as the Night's, Reincarnation-");
        log_info("FAVORITE / FVP engine -- English Translation Pipeline");
        log_info(bar);

        gemini::load_api_key();

        if (clean && gemini::load_api_key().empty()) {
            log_info("[ERROR] GEMINI_API_KEY not set -- refusing to discard the "
                     "cache for a run that cannot translate.");
            return kRefusedExitCode;
        }

        if (clean) {
            for (const char* f : {"extracted_text.json", "translated_text.json",
                                  "translation_cache_gemini.json"}) {
                const fs::path p = fs::u8path(out_dir + "\\" + f);
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

        const auto& stages = apps::pipeline_steps();
        auto banner = [&](std::size_t i) {
            log_info("\n" + bar);
            log_info("STEP " + std::to_string(i + kDisplayIndexOffset) + ": " + stages[i].what);
            log_info(bar);
        };

        banner(kStageExtract);
        if (!run_stage(stages[kStageExtract].name,
                       {"--dir", project, "--game-dir", game})) {
            return apps::kExitError;
        }

        banner(kStageTranslate);
        {
            std::vector<std::string> args = {
                "--dir", project,
                "--game-dir", game,
                "--batch", std::to_string(batch),
            };
            if (max_batches > kNoBatchLimit) {
                args.emplace_back("--max-batches");
                args.emplace_back(std::to_string(max_batches));
            }
            if (!run_stage(stages[kStageTranslate].name, args)) {
                return apps::kExitError;
            }
        }

        banner(kStagePatchUi);
        if (!run_stage(stages[kStagePatchUi].name, {"--game-dir", game})) {
            return apps::kExitError;
        }

        banner(kStageDeploy);
        if (!deploy(project, game)) {
            return apps::kExitError;
        }

        log_info("\n" + bar);
        log_info("PIPELINE DONE.");
        log_info(bar);
        return apps::kExitOk;
    } catch (const std::exception& e) {
        log_info(std::string("[ERROR] ") + e.what());
        return apps::kExitError;
    }
}
