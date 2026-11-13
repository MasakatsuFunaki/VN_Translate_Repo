// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 00_run_all -- Sakura no Toki (サクラノ刻) English translation pipeline
// orchestrator.
//
//   00_run_all                  # the whole pipeline
//   00_run_all --max-batches N  # cap the run at N API requests
//   00_run_all --batch N        # lines per API request
//   00_run_all --clean          # delete cache then run the whole pipeline
//
// Sequence: 01_extract -> 02_translate -> deploy.  Translation carries the
// speaker gate and the runtime table with it: everything required for the game
// to display English runs inside the command that pays for it.
#include "apps/paths.h"
#include "apps/pipeline_steps.h"
#include "common/util.h"
#include "translate/gemini_client.h"
#include "translate/translate_core.h"

#include <cstddef>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace snt;

constexpr int kDefaultBatchSize = 150;
constexpr int kUnlimitedBatches = 0;
constexpr int kUnusedTestBatches = 0;
constexpr std::size_t kBannerWidth = 60;
constexpr std::size_t kExtractStageIndex = 0;
constexpr std::size_t kTranslateStageIndex = 1;
constexpr std::size_t kDeployStageIndex = 2;
constexpr std::size_t kDisplayIndexOffset = 1;

bool run_child(const std::string& exe_stem,
               const std::vector<std::string>& passthrough_args,
               const std::vector<std::string>& extra_args) {
    const fs::path exe_path =
        fs::u8path(apps::exe_dir()) / (exe_stem + ".exe");
    std::vector<std::string> full_args = passthrough_args;
    full_args.insert(full_args.end(), extra_args.begin(), extra_args.end());

    print_line(
        "Running: " +
        apps::format_windows_command_line(exe_path.u8string(), full_args));
    const int rc = apps::run_process(exe_path, full_args);
    if (rc != apps::kExitOk) {
        log_info("\n[ERROR] " + exe_stem + " failed");
        return false;
    }
    return true;
}

// Ship the two artifacts the game reads.  Nothing is rebuilt here: a rebuild
// would relink the very executable running this, and the copy would then fail
// on a locked binary whenever a pipeline source had changed.
//
// The DLL is a hard failure -- without it the game shows Japanese and nothing
// says why.  The table is a soft one: it is generated, so a missing one means
// translation has not produced it yet.
bool deploy(const std::string& project, const std::string& game) {
    const wchar_t* const dist = _wgetenv(L"VN_DIST_BUILD");
    if (dist != nullptr && *dist != L'\0') {
        log_info("[DIST] VN_DIST_BUILD set -- skipping the copy to the game folder.");
        return true;
    }

    std::error_code ec;
    fs::path dll;
    for (const auto& candidate : {project + "\\build\\proxy\\Release\\version.dll",
                                  apps::exe_dir() + "\\..\\game\\version.dll"}) {
        if (fs::exists(fs::u8path(candidate), ec)) {
            dll = fs::u8path(candidate);
            break;
        }
        if (ec) {
            log_info("  WARN: cannot check " + candidate + ": " + ec.message());
        }
    }
    if (dll.empty()) {
        log_info("[ERROR] version.dll not found -- build the proxy half first "
                 "(python build.py).");
        return false;
    }

    fs::copy_file(dll, fs::u8path(game + "\\version.dll"),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_info("[ERROR] copying version.dll failed (" + ec.message() +
                 ") -- is the game running?  Close it and retry.");
        return false;
    }
    log_info("  version.dll -> " + game);

    fs::path tsv;
    for (const auto& candidate : {project + "\\build\\install\\game\\translations.tsv",
                                  apps::exe_dir() + "\\..\\game\\translations.tsv"}) {
        if (fs::exists(fs::u8path(candidate), ec)) {
            tsv = fs::u8path(candidate);
            break;
        }
        if (ec) {
            log_info("  WARN: cannot check " + candidate + ": " + ec.message());
        }
    }
    if (tsv.empty()) {
        log_info("  WARN: translations.tsv not found -- 02_translate produces it.");
        return true;
    }
    fs::copy_file(tsv, fs::u8path(game + "\\translations.tsv"),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_info("[ERROR] copying translations.tsv failed (" + ec.message() + ")");
        return false;
    }
    log_info("  translations.tsv -> " + game);
    return true;
}

}  // namespace

int main() {
    snt::setup_console_utf8();
    try {
        namespace po = apps::po;

        std::string project;
        std::string game;
        int batch = kDefaultBatchSize;
        int max_batches = kUnlimitedBatches;
        bool clean = false;
        bool discard_cache = false;
        po::options_description desc("00_run_all -- Sakura no Toki translation pipeline");
        desc.add(apps::common_options(project, game));
        desc.add_options()
            ("batch", po::value(&batch)->default_value(kDefaultBatchSize),
             "strings per API request, passed to 02_translate")
            ("max-batches", po::value(&max_batches)->default_value(kUnlimitedBatches),
             "stop after N API requests, keeping the cache and outputs (0 = no limit)")
            ("clean", po::bool_switch(&clean), "delete cached JSON files before running")
            ("discard-cache", po::bool_switch(&discard_cache),
             "allow --clean to delete a cache that already holds paid work");
        po::variables_map vm;
        if (const auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        if (const auto why =
                translate::validate_options(batch, kUnusedTestBatches, max_batches)) {
            log_info("[ERROR] " + *why);
            return apps::kRefusedExitCode;
        }

        const std::string out_dir = project + "\\script_output";
        const std::string cache_file = out_dir + "\\translation_cache_gemini.json";
        if (clean) {
            try {
                if (const auto why = translate::refuse_cache_discard(
                        translate::cache_entry_count(cache_file),
                        "--clean", discard_cache)) {
                    log_info("[ERROR] " + *why);
                    return apps::kRefusedExitCode;
                }
            } catch (const std::exception& e) {
                log_info(std::string("[ERROR] ") + e.what());
                return apps::kRefusedExitCode;
            }
        }

        const std::vector<std::string> project_passthrough = {"--dir", project};
        const std::vector<std::string> full_passthrough = {
            "--dir", project, "--game-dir", game};

        const std::string bar(kBannerWidth, '=');
        log_info(bar);
        log_info("Sakura no Toki (サクラノ刻)");
        log_info("English Translation Pipeline (Gemini)");
        log_info(bar);

        gemini::load_api_key();

        if (clean && gemini::load_api_key().empty()) {
            log_info("[ERROR] GEMINI_API_KEY not set -- refusing to discard the "
                     "cache for a run that cannot translate.");
            return apps::kRefusedExitCode;
        }

        if (clean) {
            for (const char* f :
                 {"extracted_text.json", "translated_text.json",
                  "translation_cache_gemini.json", "narrative_candidates.json",
                  "narrative_scanned.json"}) {
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
                    return apps::kExitFailure;
                }
                log_info(std::string("[clean] ") + f +
                         (removed ? "" : " was already gone"));
            }
        }

        const auto& stages = apps::pipeline_steps();
        auto banner = [&](std::size_t i) {
            log_info("\n" + bar);
            log_info("[" + std::to_string(i + kDisplayIndexOffset) + "/" +
                     std::to_string(stages.size()) + "] " + stages[i].what);
            log_info(bar);
        };

        banner(kExtractStageIndex);
        if (!run_child(stages[kExtractStageIndex].name, full_passthrough, {})) {
            return apps::kExitFailure;
        }

        banner(kTranslateStageIndex);
        {
            std::vector<std::string> args = {"--batch", std::to_string(batch)};
            if (max_batches > 0) {
                args.emplace_back("--max-batches");
                args.emplace_back(std::to_string(max_batches));
            }
            if (!run_child(stages[kTranslateStageIndex].name, project_passthrough, args)) {
                return apps::kExitFailure;
            }
        }

        banner(kDeployStageIndex);
        if (!deploy(project, game)) {
            return apps::kExitFailure;
        }

        log_info("\n" + bar);
        log_info("PIPELINE COMPLETE");
        log_info(bar);
        return apps::kExitOk;
    } catch (const std::exception& e) {
        snt::log_info(std::string("[ERROR] ") + e.what());
        return snt::apps::kExitFailure;
    }
}
