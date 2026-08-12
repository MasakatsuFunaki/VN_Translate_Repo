// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 00_run_all -- CROSS†CHANNEL -FINAL COMPLETE- English translation pipeline
// orchestrator.
//
//   00_run_all             # the whole pipeline
//   00_run_all --test      # 1 batch smoke test
//   00_run_all --test N    # N batches then stop
//   00_run_all --clean     # delete cached JSON, then run
//
// Steps: 01_extract -> 02_translate -> deploy.  The translation step carries
// the speaker gate and the runtime table with it, which is why there are three
// steps and not five: everything that must happen for the game to show English
// happens inside the command that pays for it.
//
// 03_find_narrative_cg is deliberately NOT part of the sequence: it spends
// vision tokens per image and is run by hand, behind its own confirmation.
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "common/util.h"
#include "translate/anthropic_client.h"

#include "apps/paths.h"
#include "apps/pipeline_steps.h"

namespace fs = std::filesystem;
using namespace crc;

namespace {

int run_command(const std::string& cmdline) {
    print_line("Running: " + cmdline);
    // std::system returns the child's exit code on Windows.
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

// Ship the two artifacts the game reads.  Nothing is rebuilt here: a rebuild
// would relink the very executable running this, and the copy would then fail
// on a locked binary whenever a pipeline source had changed.
//
// The DLL is a hard failure -- without it the game shows Japanese and nothing
// says why.  The table is a soft one: it is generated, so a missing one means
// the translation step has not produced it yet.
bool deploy(const std::string& project, const std::string& game) {
    if (const char* dist = std::getenv("VN_DIST_BUILD"); dist && *dist) {
        log_info("[DIST] VN_DIST_BUILD set -- skipping the copy to the game folder.");
        return true;
    }

    // The whole game builds into one tree, so the DLL is in build/proxy; when
    // this executable is the staged copy under build/install/bin, the DLL is
    // in the sibling game/ folder instead.
    fs::path dll;
    for (const auto& candidate : {project + "\\build\\proxy\\Release\\xinput1_3.dll",
                                  apps::exe_dir() + "\\..\\game\\xinput1_3.dll"}) {
        if (fs::exists(fs::u8path(candidate))) {
            dll = fs::u8path(candidate);
            break;
        }
    }
    if (dll.empty()) {
        log_info("[ERROR] xinput1_3.dll not found -- build the proxy half first "
                 "(python build.py).");
        return false;
    }

    std::error_code ec;
    fs::copy_file(dll, fs::u8path(game + "\\xinput1_3.dll"),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_info("[ERROR] copying xinput1_3.dll failed (" + ec.message() +
                 ") -- is the game running?  Close it and retry.");
        return false;
    }
    log_info("  xinput1_3.dll -> " + game);

    const fs::path tsv = fs::u8path(apps::out_tsv(project));
    if (!fs::exists(tsv)) {
        log_info("  WARN: translations.tsv not found -- 02_translate produces it.");
        return true;
    }
    fs::copy_file(tsv, fs::u8path(apps::game_tsv(game)),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_info("[ERROR] copying translations.tsv failed (" + ec.message() + ")");
        return false;
    }
    log_info("  translations.tsv -> " + game);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = apps::po;

    std::string project, game;
    int test_n = 0;
    bool clean = false;
    po::options_description desc("00_run_all -- CROSS_CHANNEL translation pipeline");
    desc.add(apps::common_options(project, game));
    desc.add_options()
        ("test", po::value(&test_n)->implicit_value(1)->default_value(0),
         "run only N translation batches then stop (bare --test = 1)")
        ("clean", po::bool_switch(&clean), "delete cached JSON files before running");
    po::variables_map vm;
    if (auto rc = apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    // The cap is validated here as well as in the translation step, because
    // this is the command that starts an unattended full run: a mistyped cap
    // must fail before a key is read and before an archive is touched.
    if (test_n < 0) {
        log_info("[ERROR] --test takes a positive batch count.");
        return 2;
    }

    // Children inherit explicit paths so a --dir override propagates.
    const std::string passthrough = " --dir \"" + project + "\" --game-dir \"" + game + "\"";

    const std::string bar(60, '=');
    log_info(bar);
    log_info("CROSS\xE2\x80\xA0" "CHANNEL -FINAL COMPLETE-");
    log_info("English Translation Pipeline (Anthropic)");
    log_info(bar);

    anthropic::load_api_key();
    if (const char* k = std::getenv("ANTHROPIC_API_KEY"); !k || !*k) {
        log_info("[X] ANTHROPIC_API_KEY not set.");
        return 1;
    }

    if (clean) {
        const std::string out_dir = apps::output_dir(project);
        // The CG state files are on this list too -- --clean forces a full
        // rescan of the narrative CGs as well as a retranslation.
        for (const char* f : {"extracted_text.json", "translated_text.json",
                              "translation_cache_anthropic.json", "narrative_candidates.json",
                              "narrative_scanned.json"}) {
            fs::path p = fs::u8path(out_dir + "\\" + f);
            if (fs::exists(p)) {
                fs::remove(p);
                log_info(std::string("[clean] ") + f);
            }
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
    run_step(steps[1].name,
             test_n ? std::vector<std::string>{"--test", std::to_string(test_n)}
                    : std::vector<std::string>{},
             passthrough);

    banner(2);
    if (!deploy(project, game)) return 1;

    log_info("\n" + bar);
    log_info("TRANSLATION COMPLETE!");
    log_info(bar);
    return 0;
}
