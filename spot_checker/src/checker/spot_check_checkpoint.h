// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "common/status.h"
#include "domain/spot_check_types.h"
#include "ports/file_store.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spot_checker {

// Full persisted state of `argon_spot_check.json` (parsed once per run, never
// silently discarding corrupt JSON). Preserves 100% of shipped keys (`game_dir`,
// `model`, `mode`, `total_entries`, `hotspots_detected`, `h_windows_skipped`,
// `radar_calls_made`, `radar_passages_triaged`, `adaptive_expansions`,
// `windows_checked`, `lines_checked`, `editable_lines_unlocked`,
// `candidates_rejected_by_validator`, `improvements_count`,
// `script_entries_updated`, `cache_entries_updated`, `checked_offsets`,
// `ranked_hotspots`, `improvements` with both `"improved_en"` and `"new_en"`,
// `"severity"`, `"error_span"`, `"fix_span"`) plus additive `token_usage`,
// `checked_spans`, `windows_blocked`, `lines_blocked`, `blocked_offsets`,
// `blocked_spans`, and `radar_results`.
struct CheckpointState {
    std::string game_dir;
    std::string model{kDefaultArgonModel};
    std::string mode = "hotspot";
    std::size_t total_entries = 0;
    double ols_slope = 0.0;
    double ols_intercept = 0.0;
    double ols_sigma = 0.0;
    bool ols_valid = false;
    std::size_t hotspots_detected = 0;
    std::size_t h_windows_skipped = 0;
    std::size_t radar_calls_made = 0;
    std::size_t radar_passages_triaged = 0;
    std::size_t adaptive_expansions = 0;
    std::size_t windows_checked = 0;
    std::size_t lines_checked = 0;
    std::size_t windows_blocked = 0;
    std::size_t lines_blocked = 0;
    std::size_t editable_lines_unlocked = 0;
    std::size_t candidates_rejected_by_validator = 0;
    std::size_t script_entries_updated = 0;
    std::size_t cache_entries_updated = 0;
    TokenUsageSummary token_usage;
    std::vector<std::size_t> checked_offsets;
    std::vector<std::pair<std::size_t, std::size_t>> checked_spans;
    std::vector<std::size_t> blocked_offsets;
    std::vector<std::pair<std::size_t, std::size_t>> blocked_spans;
    std::vector<RadarPassageResult> radar_results;
    std::vector<HotspotWindow> ranked_hotspots;
    std::vector<Improvement> improvements;
};

// Parses `argon_spot_check.json` contents into `CheckpointState`.
// - If `"checked_spans"` is absent on legacy checkpoints, synthesizes
//   `[offset, default_window_size)` spans from `"checked_offsets"`.
// - Parses optional `"windows_blocked"`, `"lines_blocked"`, `"blocked_offsets"`,
//   and `"blocked_spans"` (defaulting to `0` / empty when absent on legacy
//   checkpoints, or synthesizing `[offset, default_window_size)` spans from
//   `"blocked_offsets"` when `"blocked_spans"` is omitted).
// - Returns `ErrorCode::CorruptCheckpoint` if `raw_json` is empty, malformed,
//   not a JSON object, or contains malformed field types (never silently
//   returns an empty state).
Result<CheckpointState> parse_checkpoint_json(
    std::string_view raw_json,
    std::size_t default_window_size = kDefaultWindowSize);

// Serializes `state` into pretty-printed (2-space indented) UTF-8 JSON.
std::string serialize_checkpoint_json(const CheckpointState& state);

// Loads `path` if `store.exists(path)` is true; returns `std::nullopt` when
// `path` does not exist, or propagates `ErrorCode::IoError` /
// `ErrorCode::CorruptCheckpoint` on read/parse failure.
Result<std::optional<CheckpointState>> load_checkpoint_if_exists(
    const IFileStore& store,
    const std::filesystem::path& path,
    std::size_t default_window_size = kDefaultWindowSize);

Status save_checkpoint_atomic(IFileStore& store,
                              const std::filesystem::path& path,
                              const CheckpointState& state);

}  // namespace spot_checker
