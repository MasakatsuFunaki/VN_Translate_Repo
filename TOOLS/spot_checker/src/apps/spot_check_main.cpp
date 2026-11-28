// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "checker/spot_checker.h"
#include "common/utf8_text.h"
#include "common/win32_cli_env.h"
#include "ports/clock_and_logger.h"

#include <boost/program_options.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace po = boost::program_options;

namespace {

constexpr int kExitSuccess = 0;
constexpr int kExitFailure = 1;
constexpr int kMinWindowSize = 1;
constexpr double kMinExclusiveConfidence = 0.0;
constexpr double kMaxInclusiveConfidence = 1.0;
constexpr std::size_t kMaxLoggedHotspots = 5;
constexpr std::size_t kHotspotLogBufferSize = 256;

}  // namespace

int main() {
    spot_checker::lower_process_priority();
    spot_checker::setup_console_utf8();

    std::string game_name_arg;
    std::string game_dir_arg;
    std::string mode_arg = "hotspot";
    int offset = 0;
    bool scan_only = false;
    bool dry_run = false;
    bool apply_only = false;
    std::string out_path;
    std::string model_arg{spot_checker::kDefaultModel};
    std::string thinking_level_arg{spot_checker::kDefaultThinkingLevel};
    int radar_calls = static_cast<int>(spot_checker::kDefaultRadarCalls);
    int window_size = static_cast<int>(spot_checker::kDefaultWindowSize);
    int max_windows = static_cast<int>(spot_checker::kDefaultMaxWindows);
    double z_threshold = spot_checker::kDefaultZThreshold;
    double min_confidence = spot_checker::kDefaultMinConfidence;
    bool lock_non_suspects = false;
    bool include_h_scenes = false;
    bool fix_punctuation = false;

    po::options_description desc("01_spot_check options");
    desc.add_options()
        ("help,h", "show this help message and exit")
        ("game", po::value(&game_name_arg),
         "game folder name relative to <repo_root>")
        ("game-dir", po::value(&game_dir_arg),
         "explicit path to target <game_dir>")
        ("mode", po::value(&mode_arg)->default_value("hotspot"),
         "selection mode: 'hotspot' (statistical KDE+CUSUM+NMS) or 'linear'")
        ("offset", po::value(&offset)->default_value(0),
         "starting translatable entry index (0-based; implies linear mode when --mode is defaulted)")
        ("scan-only", po::bool_switch(&scan_only),
         "run offline statistical OLS + hotspot scan with 0 API calls and 0 writes inside <game_dir>")
        ("dry-run", po::bool_switch(&dry_run),
         "write argon_spot_check.json only without modifying translated_text.json, caches, or .enc")
        ("apply", po::bool_switch(&apply_only),
         "apply saved argon_spot_check.json to translated_text.json, caches, and .enc (0 API calls)")
        ("out", po::value(&out_path),
         "override output path for argon_spot_check.json or --scan-only JSON report")
        ("model",
         po::value(&model_arg)->default_value(
             std::string(spot_checker::kDefaultModel)),
         "Gemini model identifier ('argon' models disallowed; changing --model re-queues previously blocked spans)")
        ("thinking-level",
         po::value(&thinking_level_arg)->default_value(
             std::string(spot_checker::kDefaultThinkingLevel)),
         "Gemini thinking level: MINIMAL, LOW, MEDIUM, HIGH (or empty for budget fallback)")
        ("radar-calls",
         po::value(&radar_calls)->default_value(
             static_cast<int>(spot_checker::kDefaultRadarCalls)),
         "Stage 1 packed Radar calls (6 candidate passages per call)")
        ("window",
         po::value(&window_size)->default_value(
             static_cast<int>(spot_checker::kDefaultWindowSize)),
         "target lines per window (>= 1)")
        ("max-windows",
         po::value(&max_windows)->default_value(
             static_cast<int>(spot_checker::kDefaultMaxWindows)),
         "maximum Stage 2 windows to check per run (0 = all)")
        ("z-threshold",
         po::value(&z_threshold)->default_value(spot_checker::kDefaultZThreshold),
         "minimum statistical Z-score for hotspot windows")
        ("min-confidence",
         po::value(&min_confidence)->default_value(
             spot_checker::kDefaultMinConfidence),
         "minimum LLM confidence in (0.0, 1.0] required to accept a proposed fix")
        ("lock-non-suspects", po::bool_switch(&lock_non_suspects),
         "lock lines without statistical weakness flags as read-only context")
        ("include-h-scenes", po::bool_switch(&include_h_scenes),
         "include H-scene lines and windows (skipped by default)")
        ("fix-punctuation", po::bool_switch(&fix_punctuation),
         "enable punctuation, romaji-note, and bracket scoring/fixes");

    const std::vector<std::string> utf8_args = spot_checker::process_utf8_args();

    po::variables_map vm;
    try {
        const auto parsed = po::command_line_parser(utf8_args).options(desc).run();
        const auto stray =
            po::collect_unrecognized(parsed.options, po::include_positional);
        if (!stray.empty()) {
            std::cerr << "[ERROR] unexpected argument '" << stray.front()
                      << "'\n\n"
                      << desc << std::endl;
            return kExitFailure;
        }
        po::store(parsed, vm);
        if (vm.count("help") > 0) {
            std::cout << desc << std::endl;
            return kExitSuccess;
        }
        po::notify(vm);
    } catch (const po::error& err) {
        std::cerr << "[ERROR] " << err.what() << "\n\n" << desc << std::endl;
        return kExitFailure;
    }

    if (!game_name_arg.empty() && !game_dir_arg.empty()) {
        std::cerr << "[ERROR] --game and --game-dir are mutually exclusive\n";
        return kExitFailure;
    }

    const std::string resolved_dir = spot_checker::resolve_game_dir(
        game_dir_arg, game_name_arg, spot_checker::find_repo_root());
    if (resolved_dir.empty()) {
        std::cerr << "[ERROR] either --game <name> or --game-dir <path> is required\n\n"
                  << desc << std::endl;
        return kExitFailure;
    }

    const int exclusive_actions =
        (scan_only ? 1 : 0) + (dry_run ? 1 : 0) + (apply_only ? 1 : 0);
    if (exclusive_actions > 1) {
        std::cerr << "[ERROR] --scan-only, --dry-run, and --apply are mutually exclusive\n";
        return kExitFailure;
    }

    if (mode_arg != "hotspot" && mode_arg != "linear") {
        std::cerr << "[ERROR] --mode must be 'hotspot' or 'linear' (got '"
                  << mode_arg << "')\n";
        return kExitFailure;
    }

    if (offset < 0 || window_size < kMinWindowSize || radar_calls < 0 ||
        max_windows < 0) {
        std::cerr << "[ERROR] --offset, --radar-calls, and --max-windows must be >= 0 "
                     "and --window must be >= 1\n";
        return kExitFailure;
    }

    if (!std::isfinite(z_threshold)) {
        std::cerr << "[ERROR] --z-threshold must be a finite number\n";
        return kExitFailure;
    }

    if (!std::isfinite(min_confidence) ||
        min_confidence <= kMinExclusiveConfidence ||
        min_confidence > kMaxInclusiveConfidence) {
        std::cerr << "[ERROR] --min-confidence must be in (0.0, 1.0]\n";
        return kExitFailure;
    }

    const std::string trimmed_model = spot_checker::trim_ascii(model_arg);
    if (trimmed_model.empty()) {
        std::cerr << "[ERROR] --model must not be empty\n";
        return kExitFailure;
    }
    if (vn::llm::is_disallowed_model(model_arg)) {
        try {
            vn::llm::ensure_supported_model(model_arg);
        } catch (const vn::llm::LlmError& err) {
            std::cerr << "[ERROR] " << err.what() << "\n";
            return kExitFailure;
        }
        std::cerr << "[ERROR] disallowed --model '" << trimmed_model
                  << "': argon models are disabled in this repository\n";
        return kExitFailure;
    }

    if (!spot_checker::is_valid_thinking_level(thinking_level_arg)) {
        std::cerr << "[ERROR] --thinking-level must be MINIMAL, LOW, MEDIUM, "
                     "HIGH, or empty (got '"
                  << thinking_level_arg << "')\n";
        return kExitFailure;
    }
    const std::string normalized_thinking_level =
        spot_checker::normalize_thinking_level(thinking_level_arg);

    spot_checker::SpotCheckConfig cfg;
    cfg.game_dir = resolved_dir;
    cfg.mode = spot_checker::SpotCheckMode::Hotspot;
    if (mode_arg == "linear" ||
        (!vm["offset"].defaulted() && vm["mode"].defaulted())) {
        cfg.mode = spot_checker::SpotCheckMode::Linear;
    }
    cfg.z_threshold = z_threshold;
    cfg.min_confidence = min_confidence;
    cfg.review_all_in_window = !lock_non_suspects;
    cfg.skip_h_scenes = !include_h_scenes;
    cfg.fix_punctuation = fix_punctuation;
    cfg.auto_apply = !dry_run && !scan_only;
    cfg.scan_only = scan_only;
    cfg.radar_calls = static_cast<std::size_t>(radar_calls);
    cfg.offset = static_cast<std::size_t>(offset);
    cfg.window_size = static_cast<std::size_t>(window_size);
    cfg.max_windows = static_cast<std::size_t>(max_windows);
    cfg.out_path = out_path;
    cfg.model = trimmed_model;
    cfg.thinking_level = normalized_thinking_level;

    spot_checker::ConsoleLogSink log;
    const auto start_time = std::chrono::steady_clock::now();

    auto checker_res = spot_checker::SpotChecker::create(cfg);
    if (!checker_res.is_ok()) {
        std::cerr << "[ERROR] " << checker_res.error_message() << std::endl;
        return kExitFailure;
    }
    spot_checker::SpotChecker& checker = checker_res.value();

    log.info("Game directory:      " + checker.config().game_dir);
    log.info("Translatable lines:  " +
             std::to_string(checker.entries().size()));
    log.info("Spot-check file:     " +
             (checker.output_json_path().empty()
                  ? std::string("(none -- in-memory scan)")
                  : checker.output_json_path()));

    if (apply_only) {
        const auto applied_res = checker.apply_improvements();
        if (!applied_res.is_ok()) {
            std::cerr << "[ERROR] " << applied_res.error_message() << std::endl;
            return kExitFailure;
        }
        const auto& applied = applied_res.value();
        log.info("Improvements loaded: " +
                 std::to_string(applied.improvements_loaded));
        log.info("Script rows updated: " +
                 std::to_string(applied.script_entries_updated));
        log.info("Cache rows updated:  " +
                 std::to_string(applied.cache_entries_updated));
        return kExitSuccess;
    }

    const auto report_res = checker.run();
    if (!report_res.is_ok()) {
        std::cerr << "[ERROR] " << report_res.error_message() << std::endl;
        return kExitFailure;
    }
    const auto& report = report_res.value();
    const double elapsed_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      start_time)
            .count();

    if (cfg.mode == spot_checker::SpotCheckMode::Hotspot || cfg.scan_only) {
        char ols_buf[kHotspotLogBufferSize];
        std::snprintf(
            ols_buf, sizeof(ols_buf),
            "OLS regression:      slope=%.3f intercept=%.3f sigma=%.3f (%s)",
            report.ols_slope, report.ols_intercept, report.ols_sigma,
            report.ols_valid ? "valid" : "fallback");
        log.info(ols_buf);
        log.info("Hotspots detected:   " +
                 std::to_string(report.hotspots_detected));
        const std::size_t limit = std::min<std::size_t>(
            kMaxLoggedHotspots, report.ranked_hotspots.size());
        for (std::size_t i = 0; i < limit; ++i) {
            const auto& hw = report.ranked_hotspots[i];
            char buf[kHotspotLogBufferSize];
            std::snprintf(
                buf, sizeof(buf),
                "  Hotspot #%zu: offset=%zu len=%zu Z=%.2f peak_line=%zu "
                "(score=%.1f, %s)",
                i + 1, hw.offset, hw.length, hw.z_score, hw.peak_index,
                hw.peak_line_score, hw.dominant_signal.c_str());
            log.info(buf);
        }
    }

    log.info("Windows checked:     " + std::to_string(report.windows_checked));
    log.info("Lines in windows:    " + std::to_string(report.lines_checked));
    if (report.windows_blocked > 0) {
        std::string spans_desc;
        for (std::size_t i = 0; i < report.blocked_spans.size(); ++i) {
            if (i > 0) {
                spans_desc += ", ";
            }
            const auto& [span_off, span_len] = report.blocked_spans[i];
            spans_desc += "[" + std::to_string(span_off) + ".." +
                          std::to_string(span_off + span_len) + ")";
        }
        log.warn("Windows blocked:     " +
                 std::to_string(report.windows_blocked) + " (" +
                 std::to_string(report.lines_blocked) + " lines): " +
                 spans_desc);
    }
    log.info("Suspects unlocked:   " +
             std::to_string(report.editable_lines_unlocked));
    log.info("Rejected by gate:    " +
             std::to_string(report.candidates_rejected_by_validator));
    log.info("API calls made:      " +
             std::to_string(report.token_usage.api_calls) +
             " (prompt=" + std::to_string(report.token_usage.prompt_tokens) +
             ", output=" + std::to_string(report.token_usage.output_tokens) +
             ", thought=" + std::to_string(report.token_usage.thought_tokens) +
             ")");
    log.info("Improvements saved:  " +
             std::to_string(report.improvements_found) + " (elapsed " +
             std::to_string(elapsed_s) + "s)");
    if (cfg.auto_apply) {
        log.info("Script rows updated: " +
                 std::to_string(report.script_entries_updated));
        log.info("Cache rows updated:  " +
                 std::to_string(report.cache_entries_updated));
    }
    for (const auto& imp : report.improvements) {
        log.print("  [#" + std::to_string(imp.global_index) + " " +
                  imp.speaker + " | " + imp.category + " (" + imp.severity +
                  ") conf=" + std::to_string(imp.confidence) + "] " +
                  imp.reason);
        log.print("    OLD: " + imp.old_en);
        log.print("    NEW: " + imp.improved_en);
    }
    return kExitSuccess;
}
