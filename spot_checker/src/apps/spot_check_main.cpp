// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include <boost/program_options.hpp>

#include "checker/spot_checker.h"
#include "common/util.h"

namespace po = boost::program_options;
namespace fs = std::filesystem;

namespace {

fs::path find_repo_root() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n =
            GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) break;
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    std::error_code ec;
    for (fs::path d = fs::path(buf).parent_path(); !d.empty(); d = d.parent_path()) {
        if (fs::is_directory(d / "spot_checker", ec) &&
            fs::is_directory(d / "CROSS_CHANNEL", ec)) {
            return d;
        }
        if (d == d.root_path()) break;
    }
    return fs::current_path(ec);
}

std::string resolve_game_dir(const std::string& game_dir_opt,
                             const std::string& game_name_opt) {
    if (!game_dir_opt.empty()) {
        return fs::absolute(fs::u8path(game_dir_opt)).u8string();
    }
    if (!game_name_opt.empty()) {
        const fs::path repo = find_repo_root();
        const fs::path cand = repo / fs::u8path(game_name_opt);
        if (fs::is_directory(cand)) {
            return fs::absolute(cand).u8string();
        }
        return fs::absolute(fs::u8path(game_name_opt)).u8string();
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    spot_checker::setup_console_utf8();

    std::string game_dir_arg;
    std::string game_name_arg;
    std::string mode_arg = "hotspot";
    double z_threshold = 0.0;
    double min_confidence = 0.85;
    bool review_all_in_window = false;
    bool include_h_scenes = false;
    bool fix_punctuation = false;
    bool dry_run = false;
    int radar_calls = 20;
    int offset = 0;
    int window_size = 40;
    int context_before = 40;
    int context_after = 20;
    int max_windows = 400;
    bool apply_only = false;
    std::string out_path;

    po::options_description desc("01_spot_check options");
    desc.add_options()
        ("help,h", "show this help message and exit")
        ("game-dir", po::value(&game_dir_arg), "path to game directory")
        ("game", po::value(&game_name_arg), "game folder name relative to repository root")
        ("mode", po::value(&mode_arg)->default_value("hotspot"),
         "selection mode: 'hotspot' (full-novel statistical KDE+CUSUM+NMS) or 'linear'")
        ("z-threshold", po::value(&z_threshold)->default_value(0.0),
         "minimum statistical Z-score for hotspot windows (0.0 = rank all non-H windows with positive weakness energy)")
        ("min-confidence", po::value(&min_confidence)->default_value(0.85),
         "minimum LLM confidence (0.0-1.0) required to accept a proposed fix")
        ("review-all-in-window", po::bool_switch(&review_all_in_window),
         "unlock all lines in each window instead of only mathematical suspect lines")
        ("include-h-scenes", po::bool_switch(&include_h_scenes),
         "include H-scene lines/windows (by default H-scenes are skipped)")
        ("fix-punctuation", po::bool_switch(&fix_punctuation),
         "enable punctuation, parenthesis/romaji-note, and bracket scoring/fixes (disabled by default)")
        ("dry-run", po::bool_switch(&dry_run),
         "write argon_spot_check.json only without editing translated_text.json or caches")
        ("radar-calls", po::value(&radar_calls)->default_value(20),
         "Stage 1 Packed LLM Radar calls (12 candidate passages per call) before Stage 2 repair")
        ("offset", po::value(&offset)->default_value(0),
         "starting translatable entry index (0-based, for linear mode)")
        ("window", po::value(&window_size)->default_value(40),
         "target lines per window")
        ("context-before", po::value(&context_before)->default_value(40),
         "preceding context lines included before target window")
        ("context-after", po::value(&context_after)->default_value(20),
         "following context lines included after target window")
        ("max-windows", po::value(&max_windows)->default_value(400),
         "maximum windows to check per run (400 windows = 16,000 target lines; 0 = all)")
        ("apply", po::bool_switch(&apply_only),
         "apply saved argon_spot_check.json to translated_text.json and cache without calling API")
        ("out", po::value(&out_path),
         "override output path for argon_spot_check.json");

    po::variables_map vm;
    try {
        auto parsed = po::command_line_parser(argc, argv).options(desc).run();
        auto stray = po::collect_unrecognized(parsed.options, po::include_positional);
        if (!stray.empty()) {
            std::cerr << "[ERROR] unexpected argument '" << stray.front() << "'\n\n"
                      << desc << std::endl;
            return 1;
        }
        po::store(parsed, vm);
        if (vm.count("help")) {
            std::cout << desc << std::endl;
            return 0;
        }
        po::notify(vm);
    } catch (const po::error& e) {
        std::cerr << "[ERROR] " << e.what() << "\n\n" << desc << std::endl;
        return 1;
    }

    const std::string resolved_dir = resolve_game_dir(game_dir_arg, game_name_arg);
    if (resolved_dir.empty()) {
        std::cerr << "[ERROR] either --game-dir <path> or --game <name> is required\n\n"
                  << desc << std::endl;
        return 1;
    }
    if (offset < 0 || window_size < 1 || context_before < 0 || context_after < 0 ||
        max_windows < 0 || radar_calls < 0) {
        std::cerr << "[ERROR] numeric window/offset/radar options must be non-negative "
                     "(and --window >= 1)\n";
        return 1;
    }

    spot_checker::SpotCheckConfig cfg;
    cfg.game_dir = resolved_dir;
    if (mode_arg == "linear" ||
        (!vm["offset"].defaulted() && vm["mode"].defaulted())) {
        cfg.mode = spot_checker::SpotCheckMode::Linear;
    } else {
        cfg.mode = spot_checker::SpotCheckMode::Hotspot;
    }
    cfg.z_threshold = z_threshold;
    cfg.min_confidence = min_confidence;
    cfg.review_all_in_window = review_all_in_window;
    cfg.skip_h_scenes = !include_h_scenes;
    cfg.fix_punctuation = fix_punctuation;
    cfg.auto_apply = !dry_run;
    cfg.radar_calls = static_cast<std::size_t>(radar_calls);
    cfg.offset = static_cast<std::size_t>(offset);
    cfg.window_size = static_cast<std::size_t>(window_size);
    cfg.context_before = static_cast<std::size_t>(context_before);
    cfg.context_after = static_cast<std::size_t>(context_after);
    cfg.max_windows = static_cast<std::size_t>(max_windows);
    cfg.out_path = out_path;

    try {
        const auto t0 = std::chrono::steady_clock::now();
        spot_checker::SpotChecker checker(cfg);
        spot_checker::log_info("Game directory:      " + cfg.game_dir);
        spot_checker::log_info("Translatable lines:  " +
                               std::to_string(checker.entries().size()));
        spot_checker::log_info("Spot-check file:     " + checker.output_json_path());

        if (apply_only) {
            const spot_checker::ApplyReport applied = checker.apply_improvements();
            spot_checker::log_info("Improvements loaded: " +
                                   std::to_string(applied.improvements_loaded));
            spot_checker::log_info("Script rows updated: " +
                                   std::to_string(applied.script_entries_updated));
            spot_checker::log_info("Cache rows updated:  " +
                                   std::to_string(applied.cache_entries_updated));
            return 0;
        }

        const spot_checker::SpotCheckReport report = checker.run();
        const double elapsed_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        if (cfg.mode == spot_checker::SpotCheckMode::Hotspot) {
            spot_checker::log_info("Hotspots detected:   " +
                                   std::to_string(report.hotspots_detected));
            for (std::size_t i = 0;
                 i < std::min<std::size_t>(5, report.ranked_hotspots.size()); ++i) {
                const auto& hw = report.ranked_hotspots[i];
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "  Hotspot #%zu: offset=%zu len=%zu Z=%.2f peak_line=%zu "
                              "(score=%.1f, %s)",
                              i + 1, hw.offset, hw.length, hw.z_score, hw.peak_index,
                              hw.peak_line_score, hw.dominant_signal.c_str());
                spot_checker::log_info(buf);
            }
        }
        spot_checker::log_info("Windows checked:     " +
                               std::to_string(report.windows_checked));
        spot_checker::log_info("Lines in windows:    " +
                               std::to_string(report.lines_checked));
        spot_checker::log_info("Suspects unlocked:   " +
                               std::to_string(report.editable_lines_unlocked));
        spot_checker::log_info("Rejected by gate:    " +
                               std::to_string(report.candidates_rejected_by_validator));
        spot_checker::log_info("Improvements saved:  " +
                               std::to_string(report.improvements_found) +
                               " (elapsed " + std::to_string(elapsed_s) + "s)");
        if (cfg.auto_apply) {
            spot_checker::log_info("Script rows updated: " +
                                   std::to_string(report.script_entries_updated));
            spot_checker::log_info("Cache rows updated:  " +
                                   std::to_string(report.cache_entries_updated));
        }
        for (const auto& imp : report.improvements) {
            spot_checker::print_line(
                "  [#" + std::to_string(imp.global_index) + " " + imp.speaker +
                " | " + imp.category + " conf=" + std::to_string(imp.confidence) +
                "] " + imp.reason);
            spot_checker::print_line("    OLD: " + imp.old_en);
            spot_checker::print_line("    NEW: " + imp.improved_en);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return 1;
    }
}
