// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 00_run_all -- EXTRAVAGANZA ~Mushi Mederu Shoujo~ Matsuro CE English
// translation pipeline orchestrator.
//
//   00_run_all                  # the whole pipeline
//   00_run_all --test           # 1 batch smoke test
//   00_run_all --test N         # N batches then stop
//   00_run_all --max-batches N  # cap the run at N API requests
//   00_run_all --batch N        # lines per API request
//   00_run_all --clean          # delete cached JSON, then run
//
// Steps: 01_extract -> 02_translate -> deploy.  The translation step carries
// the speaker gate and the runtime table with it, which is why there are three
// steps and not five: everything that must happen for the game to show English
// happens inside the command that pays for it.
//
// The .xtx name plates are NOT touched here.  03_translate_xtx rewrites files
// inside the game installation and is run once per install, not once per
// translation run.
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
using namespace exm;

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

// Ship the one artifact the game cannot generate for itself.  Nothing is
// rebuilt here: a rebuild would relink the very executable running this, and
// the copy would then fail on a locked binary whenever a pipeline source had
// changed.
//
// The DLL is a hard failure -- without it the game shows Japanese and nothing
// says why.  The table is a soft one: 02_translate writes it straight into the
// game folder, so a missing one means the translation step has not produced it
// yet.
bool deploy(const std::string& project, const std::string& game) {
    if (const char* dist = std::getenv("VN_DIST_BUILD"); dist && *dist) {
        log_info("[DIST] VN_DIST_BUILD set -- skipping the copy to the game folder.");
        return true;
    }

    // The whole game builds into one tree, so the DLL is in build/proxy; when
    // this executable is the staged copy under build/install/bin, the DLL is
    // in the sibling game/ folder instead.
    // Asked for, not thrown, like the copies below: a status query that
    // fails says nothing about whether the file is there, and the throwing
    // overload would end the run from here with no diagnosis at all.
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
                 "02_translate writes it there.");
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    setup_console_utf8();
    namespace po = apps::po;

    std::string project, game;
    int batch = 0, test_n = 0, max_batches = 0;
    bool clean = false, discard_cache = false;
    po::options_description desc("00_run_all -- EXTRAVAGANZA MATSURO CE translation pipeline");
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
         "allow --clean to delete a cache that already holds paid "
         "work");
    po::variables_map vm;
    if (auto rc = apps::parse_command_line(argc, argv, desc, vm)) return *rc;

    // The caps are validated here as well as in the translation step, because
    // this is the command that starts an unattended full run: a mistyped cap
    // must fail before a key is read and before an archive is touched.
    if (auto why = translate::validate_options(batch, test_n, max_batches)) {
        log_info("[ERROR] " + *why);
        return 2;
    }

    // --clean discards the cache; guard runs early so a refusal costs nothing.
    const std::string out_dir = project + "\\script_output";
    const std::string cache_file = out_dir + "\\translation_cache_anthropic.json";
    if (clean) {
        // Counting the cache parses it, and a cache that cannot be parsed
        // throws.  Handled here because this guard runs before the banner: an
        // escaping throw would end the run with no output at all.  A cache
        // nobody could read is a refusal like any other, so nothing is
        // deleted and the file is left as it is.
        try {
            if (auto why = translate::refuse_cache_discard(
                    translate::cache_entry_count(cache_file),
                    "--clean", discard_cache)) {
                log_info("[ERROR] " + *why);
                return 2;
            }
        } catch (const std::exception& e) {
            log_info(std::string("[ERROR] ") + e.what());
            return 2;
        }
    }

    // Children inherit explicit paths so a --dir override propagates.
    const std::string passthrough =
        " --dir \"" + project + "\" --game-dir \"" + game + "\"";

    const std::string bar(60, '=');
    log_info(bar);
    log_info("EXTRAVAGANZA ~Mushi Mederu Shoujo~ Matsuro CE");
    log_info("English Translation Pipeline (Anthropic)");
    log_info(bar);

    anthropic::load_api_key();

    // --clean deletes the cache and --test translates a capped number of
    // batches, so either is certain to need the API.  Only --clean throws work
    // away, and the two are reported apart because a message that names the
    // wrong consequence sends the reader looking for a cache that is still
    // there.  Checked before the deletion: without this the cache would go and
    // the translation step would then fail for the missing key, losing work
    // that cannot be rebuilt.
    if ((clean || test_n > 0) && anthropic::load_api_key().empty()) {
        log_info(std::string("[ERROR] ANTHROPIC_API_KEY not set -- ") +
                 (clean ? "refusing to discard the cache for a run that cannot "
                          "translate."
                        : "--test translates a capped number of batches and "
                          "cannot run without the API."));
        return 2;
    }

    if (clean) {
        for (const char* f : {"extracted_text.json", "translated_text.json",
                              "translation_cache_anthropic.json"}) {
            // Reported, not thrown, at both steps: the throwing overloads
            // would end the run with no diagnosis.  A delete fails when
            // another process holds the file open without FILE_SHARE_DELETE,
            // which scanners, indexers and sync clients routinely do.
            fs::path p = fs::u8path(out_dir + "\\" + f);
            std::error_code ec;
            const bool present = fs::exists(p, ec);
            if (!ec && !present) continue;  // never written: nothing to clean
            const bool removed = !ec && fs::remove(p, ec);
            if (ec) {
                log_info("[ERROR] cannot delete " + out_dir + "\\" + f + ": " +
                         ec.message() +
                         " -- close whatever has it open, then retry.");
                return 1;
            }
            // A file that vanished between the two calls was
            // not deleted here.
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
