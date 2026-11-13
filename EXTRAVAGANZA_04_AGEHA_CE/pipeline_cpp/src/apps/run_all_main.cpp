// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// 00_run_all -- AGEHA ~EXTRAVAGANZA~ CE English translation pipeline orchestrator.
#include "apps/paths.h"
#include "apps/pipeline_steps.h"
#include "cache/cache_core.h"
#include "cache/gemini_client.h"
#include "common/util.h"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace exm;

namespace {

constexpr int kDefaultBatchArg = 150;
constexpr int kNoTestBatches = 0;
constexpr int kDefaultMaxBatches = 0;
constexpr std::size_t kBannerWidth = 60;
constexpr std::size_t kExtractStepIndex = 0;
constexpr std::size_t kBuildTsvStepIndex = 1;
constexpr std::size_t kDeployStepIndex = 2;

bool run_step(const std::string& exe, const std::vector<std::string>& extra_args,
              const std::string& project, const std::string& game) {
    const std::string exe_path = apps::exe_dir() + "\\" + exe + ".exe";
    std::vector<std::string> full_args = {"--dir", project, "--game-dir", game};
    full_args.insert(full_args.end(), extra_args.begin(), extra_args.end());

    std::string display = "\"" + exe_path + "\"";
    for (const std::string& arg : full_args) {
        display += " \"" + arg + "\"";
    }
    print_line("Running: " + display);

    const int rc = apps::run_child_process(exe_path, full_args);
    if (rc != 0) {
        log_info("\n[ERROR] " + exe + " failed");
        return false;
    }
    return true;
}

bool deploy(const std::string& project, const std::string& game) {
    if (apps::wide_env_utf8(L"VN_DIST_BUILD").has_value()) {
        log_info("[DIST] VN_DIST_BUILD set -- skipping the copy to the game folder.");
        return true;
    }

    std::error_code ec;
    fs::path dll;
    for (const auto& candidate : {project + "\\build\\proxy\\Release\\winmm.dll",
                                  apps::exe_dir() + "\\..\\game\\winmm.dll"}) {
        if (fs::exists(fs::u8path(candidate), ec)) {
            dll = fs::u8path(candidate);
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
    } else if (!have_table) {
        log_info("  WARN: translation_table.tsv not in the game folder -- "
                 "02_build_tsv writes it there.");
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
        po::options_description desc("00_run_all -- AGEHA CE translation pipeline");
        desc.add(apps::common_options(project, game));
        desc.add_options()
            ("batch", po::value(&batch)->default_value(kDefaultBatchArg),
             "lines per API request, passed to the translation step")
            ("max-batches", po::value(&max_batches)->default_value(kDefaultMaxBatches),
             "stop after N API requests, keeping the cache and outputs (0 = no limit)")
            ("clean", po::bool_switch(&clean), "delete cached JSON files before running")
            ("discard-cache", po::bool_switch(&discard_cache),
             "allow --clean to delete a cache that already holds paid work");
        po::variables_map vm;
        if (const auto rc = apps::parse_process_command_line(desc, vm)) {
            return *rc;
        }

        if (const auto why = translate::validate_options(batch, kNoTestBatches, max_batches)) {
            log_info("[ERROR] " + *why);
            return apps::kExitRefused;
        }

        const std::string out_dir = project + "\\script_output";
        const std::string cache_file = out_dir + "\\translation_cache_gemini.json";
        if (clean) {
            try {
                if (const auto why = translate::refuse_cache_discard(
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
        log_info("AGEHA ~EXTRAVAGANZA~ CE");
        log_info("English Translation Pipeline (Gemini)");
        log_info(bar);

        gemini::load_api_key(project);

        const bool has_api_key = apps::wide_env_utf8(L"GEMINI_API_KEY").has_value();
        if (clean && !has_api_key) {
            log_info("[ERROR] GEMINI_API_KEY not set -- refusing to discard the cache "
                     "for a run that cannot translate.");
            return apps::kExitRefused;
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

        const auto& steps = apps::pipeline_steps();
        auto banner = [&](std::size_t i) {
            log_info("\n" + bar);
            log_info(std::string(steps[i].name) + ": " + steps[i].what);
            log_info(bar);
        };

        banner(kExtractStepIndex);
        if (!run_step(steps[kExtractStepIndex].name, {}, project, game)) {
            return apps::kExitError;
        }

        banner(kBuildTsvStepIndex);
        {
            std::vector<std::string> args = {"--batch", std::to_string(batch)};
            if (max_batches > 0) {
                args.emplace_back("--max-batches");
                args.emplace_back(std::to_string(max_batches));
            }
            if (!run_step(steps[kBuildTsvStepIndex].name, args, project, game)) {
                return apps::kExitError;
            }
        }

        banner(kDeployStepIndex);
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
