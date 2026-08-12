// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 00_run_all -- Mushigurui HD10 (蟲狂い) English
// translation pipeline orchestrator.
//
//   00_run_all             # the whole pipeline
//   00_run_all --test      # 1 batch smoke test
//   00_run_all --test N    # N batches then stop
//   00_run_all --batch N   # lines per API request
//   00_run_all --clean     # delete cached JSON, then run
//
// Steps: 01_extract -> 02_translate -> deploy.  The translation step carries
// the speaker gate and the runtime table with it, which is why there are
// three steps and not five: everything that must happen for the game to show
// English happens inside the command that pays for it.
//
// The asset tools (03_translate_xtx, 04_find_narrative_cg,
// 05_replace_choice_text) are deliberately not here: they touch the shipped
// archives, the config files and the screenshots, not the script text.
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "common/util.h"
#include "translate/anthropic_client.h"
#include "translate/translate_core.h"

#include "apps/paths.h"

namespace fs = std::filesystem;
using namespace mgi;

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

// Ship the one artifact the game cannot produce for itself.  Nothing is
// rebuilt here: a rebuild would relink the very executable running this, and
// the copy would then fail on a locked binary whenever a pipeline source had
// changed.
//
// The DLL is a hard failure -- without it the game shows Japanese and nothing
// says why.  The table is a soft one: 02_translate writes it straight into
// the game folder, so a missing one means the translation step has not
// produced it yet.
bool deploy(const std::string& project, const std::string& game) {
    if (const char* dist = std::getenv("VN_DIST_BUILD"); dist && *dist) {
        log_info("[DIST] VN_DIST_BUILD set -- skipping the copy to the game folder.");
        return true;
    }

    // The whole game builds into one tree, so the DLL is in build/proxy;
    // when this executable is the staged copy under build/install/bin, the
    // DLL is in the sibling game/ folder instead.
    fs::path dll;
    for (const auto& candidate : {project + "\\build\\proxy\\Release\\winmm.dll",
                                  apps::exe_dir() + "\\..\\game\\winmm.dll"}) {
        if (fs::exists(fs::u8path(candidate))) {
            dll = fs::u8path(candidate);
            break;
        }
    }
    if (dll.empty()) {
        log_info("[ERROR] winmm.dll not found -- build the proxy half first "
                 "(python build.py).");
        return false;
    }

    std::error_code ec;
    fs::copy_file(dll, fs::u8path(game + "\\winmm.dll"),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_info("[ERROR] copying winmm.dll failed (" + ec.message() +
                 ") -- is the game running?  Close it and retry.");
        return false;
    }
    log_info("  winmm.dll -> " + game);

    if (!fs::exists(fs::u8path(game + "\\translation_table.tsv")))
        log_info("  WARN: translation_table.tsv not in the game folder -- "
                 "02_translate produces it.");
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = apps::po;

    std::string project, game;
    int batch = 0, test_n = 0;
    bool clean = false;
    po::options_description desc("00_run_all -- Mushigurui HD10 translation pipeline");
    desc.add(apps::common_options(project, game));
    desc.add_options()
        ("batch", po::value(&batch)->default_value(150),
         "lines per API request, passed to the translation step")
        ("test", po::value(&test_n)->implicit_value(1)->default_value(0),
         "run only N translation batches then stop (bare --test = 1)")
        ("clean", po::bool_switch(&clean), "delete cached JSON files before running");
    po::variables_map vm;
    if (auto rc = apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    // The caps are validated here as well as in the translation step, because
    // this is the command that starts an unattended full run: a mistyped cap
    // must fail before a key is read and before an archive is touched.
    if (auto why = translate::validate_options(batch, test_n)) {
        log_info("[ERROR] " + *why);
        return 2;
    }

    // Children inherit explicit paths so a --dir override propagates.
    const std::string passthrough =
        " --dir \"" + project + "\" --game-dir \"" + game + "\"";

    const std::string bar(60, '=');
    log_info(bar);
    log_info("Mushigurui HD10 (Bug Madness)");
    log_info("English Translation Pipeline (Anthropic)");
    log_info(bar);

    anthropic::load_api_key();
    if (const char* k = std::getenv("ANTHROPIC_API_KEY"); !k || !*k) {
        log_info("[X] ANTHROPIC_API_KEY not set.");
        return 1;
    }

    if (clean) {
        const std::string out_dir = project + "\\script_output";
        for (const char* f : {"extracted_text.json", "translated_text.json",
                              "translation_cache_anthropic.json"}) {
            fs::path p = fs::u8path(out_dir + "\\" + f);
            if (fs::exists(p)) {
                fs::remove(p);
                log_info(std::string("[clean] ") + f);
            }
        }
    }

    log_info("\n" + bar);
    log_info("STEP 1: Extract text from SPT files");
    log_info(bar);
    run_step("01_extract", {}, passthrough);

    log_info("\n" + bar);
    log_info("STEP 2: Speaker gate, translate via Claude, build translation_table.tsv");
    log_info(bar);
    {
        std::vector<std::string> args = {"--batch", std::to_string(batch)};
        if (test_n > 0) {
            args.emplace_back("--test");
            args.emplace_back(std::to_string(test_n));
        }
        run_step("02_translate", args, passthrough);
    }

    log_info("\n" + bar);
    log_info("STEP 3: Deploy winmm.dll to the game folder");
    log_info(bar);
    if (!deploy(project, game)) return 1;

    log_info("\n" + bar);
    log_info("TRANSLATION COMPLETE!");
    log_info(bar);
    return 0;
}
