// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 00_run_all -- orchestrator: 01_extract -> 02_translate -> deploy.
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "common/util.h"
#include "translate/anthropic_client.h"
#include "translate/translate_core.h"

#include "apps/paths.h"
#include "apps/pipeline_steps.h"

namespace fs = std::filesystem;
using namespace frat;

namespace {

int run_command(const std::string& cmdline) {
    print_line("Running: " + cmdline);
    return std::system(("\"" + cmdline + "\"").c_str());
}

void run_step(const std::string& exe, const std::vector<std::string>& args,
              const std::string& passthrough) {
    std::string cmd = "\"" + apps::exe_dir() + "\\" + exe + ".exe\"" + passthrough;
    for (const auto& a : args) cmd += " " + a;
    if (run_command(cmd) != 0) {
        log_info("\n[ERROR] " + exe + " failed");
        std::exit(1);
    }
}

// Copy winmm.dll to the game folder. The table is not copied — 02_translate
// writes it directly.
bool deploy(const std::string& project, const std::string& game) {
    if (const char* dist = std::getenv("VN_DIST_BUILD"); dist && *dist) {
        log_info("[DIST] VN_DIST_BUILD set -- skipping the copy to the game folder.");
        return true;
    }

    // Non-throwing: a status query that fails is not proof the file is absent.
    std::error_code ec;
    fs::path dll;
    for (const auto& candidate : {project + "\\build\\proxy\\Release\\winmm.dll",
                                  apps::exe_dir() + "\\..\\game\\winmm.dll"}) {
        if (fs::exists(fs::u8path(candidate), ec)) {
            dll = fs::u8path(candidate);
            break;
        }
        if (ec)
            log_info("  WARN: cannot check " + candidate + ": " + ec.message());
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
    if (ec)
        log_info("  WARN: cannot check the game folder for "
                 "translation_table.tsv: " + ec.message());
    else if (!have_table)
        log_info("  WARN: translation_table.tsv not in the game folder -- "
                 "02_translate writes it.");
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = apps::po;

    std::string project, game;
    int batch = 0, test_n = 0, max_batches = 0;
    bool clean = false, discard_cache = false;
    po::options_description desc("00_run_all -- Fraternite HD Translation Pipeline");
    desc.add(apps::common_options(project, game));
    desc.add_options()
        ("batch", po::value(&batch)->default_value(150),
         "lines per API request, passed to the translation step")
        ("test", po::value(&test_n)->implicit_value(1)->default_value(0),
         "smoke run: N batches then stop (bare --test = 1)")
        ("max-batches", po::value(&max_batches)->default_value(0),
         "stop after N API requests, keeping the cache and outputs (0 = no limit)")
        ("clean", po::bool_switch(&clean), "delete cached JSON files before running")
        ("discard-cache", po::bool_switch(&discard_cache),
         "allow --test or --clean to delete a cache that already holds paid "
         "work");
    po::variables_map vm;
    if (auto rc = apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    // Fail-fast: a mistyped cap must cost nothing.
    if (auto why = translate::validate_options(batch, test_n, max_batches)) {
        log_info("[ERROR] " + *why);
        return 2;
    }

    const std::string out_dir = project + "\\script_output";
    const std::string cache_file = out_dir + "\\translation_cache_anthropic.json";
    if (clean || test_n > 0) {
        // An unparseable cache is a refusal, not a crash.
        try {
            if (auto why = translate::refuse_cache_discard(
                    translate::cache_entry_count(cache_file),
                    clean ? "--clean" : "--test", discard_cache)) {
                log_info("[ERROR] " + *why);
                return 2;
            }
        } catch (const std::exception& e) {
            log_info(std::string("[ERROR] ") + e.what());
            return 2;
        }
    }

    const std::string passthrough =
        " --dir \"" + project + "\" --game-dir \"" + game + "\"";

    const std::string bar(60, '=');
    log_info(bar);
    log_info("Fraternite HD Remaster (フラテルニテ HDリマスター)");
    log_info("English Translation Pipeline (Anthropic)");
    log_info(bar);

    // No unconditional load here: a run whose cache already covers the script
    // never calls the API, and must not ask the user for a key to do nothing.
    // The two places that need one load it themselves -- the guard below, and
    // the client when a batch is actually sent.

    // Check before deletion: losing the cache without a key loses paid work.
    if ((clean || test_n > 0) && anthropic::load_api_key().empty()) {
        log_info("[ERROR] ANTHROPIC_API_KEY not set -- refusing to discard the "
                 "cache for a run that cannot translate.");
        return 2;
    }

    if (clean) {
        for (const char* f : {"extracted_text.json", "translated_text.json",
                              "translation_cache_anthropic.json"}) {
            fs::path p = fs::u8path(out_dir + "\\" + f);
            std::error_code ec;
            const bool present = fs::exists(p, ec);
            if (!ec && !present) continue;
            const bool removed = !ec && fs::remove(p, ec);
            if (ec) {
                log_info("[ERROR] cannot delete " + out_dir + "\\" + f + ": " +
                         ec.message() +
                         " -- close whatever has it open, then retry.");
                return 1;
            }
            log_info(std::string("[clean] ") + f +
                     (removed ? "" : " was already gone"));
        }
    }

    const auto& steps = apps::pipeline_steps();
    auto banner = [&](std::size_t i) {
        log_info("\n" + bar);
        log_info("STEP " + std::to_string(i + 1) + ": " + steps[i].what);
        log_info(bar);
    };

    banner(0);
    run_step(steps[0].name, {}, passthrough);

    banner(1);
    {
        std::vector<std::string> args = {"--batch", std::to_string(batch)};
        if (test_n > 0) {
            args.emplace_back("--test");
            args.emplace_back(std::to_string(test_n));
            if (discard_cache) args.emplace_back("--discard-cache");
        } else if (max_batches > 0) {
            args.emplace_back("--max-batches");
            args.emplace_back(std::to_string(max_batches));
        }
        run_step(steps[1].name, args, passthrough);
    }

    banner(2);
    if (!deploy(project, game)) return 1;

    log_info("\n" + bar);
    log_info("TRANSLATION COMPLETE!");
    log_info(bar);
    return 0;
}
