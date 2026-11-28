// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "vn/llm/llm_types.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spot_checker {

inline constexpr std::string_view kDefaultModel = vn::llm::kDefaultModel;
inline constexpr std::string_view kDefaultArgonModel = kDefaultModel;
inline constexpr std::string_view kDefaultThinkingLevel =
    vn::llm::kDefaultThinkingLevel;
inline constexpr std::string_view kDefaultArgonHost =
    "generativelanguage.googleapis.com";
inline constexpr std::string_view kDefaultArgonPort = "443";
inline constexpr std::string_view kDefaultArgonBasePath = "/v1beta/models";

inline constexpr double kDefaultZThreshold = 0.0;
inline constexpr double kDefaultMinConfidence = 0.85;
inline constexpr std::size_t kDefaultRadarCalls = 20;
inline constexpr std::size_t kDefaultRadarPackSize = 6;
inline constexpr std::size_t kDefaultWindowSize = 40;
inline constexpr std::size_t kDefaultContextBefore = 40;
inline constexpr std::size_t kDefaultContextAfter = 20;
inline constexpr std::size_t kDefaultMaxWindows = 400;
inline constexpr std::size_t kDefaultPeriodicSyncInterval = 25;
inline constexpr std::size_t kDefaultMinBlockedSplitLines = 5;

inline constexpr std::size_t kDefaultNmsMinSeparation = 30;
inline constexpr double kDefaultGaussianSigma = 10.0;
inline constexpr double kDefaultCusumSlackK = 0.75;
inline constexpr double kDefaultContagionDefectRate = 0.12;
inline constexpr std::size_t kDefaultBoundaryMargin = 5;

inline constexpr double kMqmCriticalWeight = 25.0;
inline constexpr double kMqmMajorWeight = 5.0;
inline constexpr double kMqmMinorWeight = 1.0;

enum class SpotCheckMode {
    Hotspot,
    Linear,
};

enum class ValidationRejection {
    None = 0,
    EmptyTranslation,
    UntranslatedPassthrough,
    InvalidDefectCategory,
    InvalidConfidence,
    PunctuationOnlyDisabled,
    ErrorSpanNotChanged,
    LeakedJapaneseCharacters,
    LeakedFullwidthOrCjkPunctuation,
    IntroducedTranslatorNote,
    QuoteEnvelopeMismatch,
    ControlTokenMismatch,
    GlossaryTermDropped,
    WeaknessScoreRegression,
    ExtremeLengthDistortion,
};

// Returns stable snake_case identifier for `code` (e.g. "quote_envelope_mismatch").
std::string_view to_string(ValidationRejection code) noexcept;

// Returns true iff `category` is one of the 6 shipped `argon_spot_check.json`
// defect categories:
//   - "mistranslation"
//   - "broken_callback"
//   - "artifact_or_note"
//   - "character_voice"
//   - "censored_or_westernized"
//   - "stiff_translationese"
bool is_valid_defect_category(std::string_view category) noexcept;

// Returns true iff `severity` is one of the 3 GEMBA-MQM severity tiers:
// "critical", "major", "minor".
bool is_valid_severity(std::string_view severity) noexcept;

// Trims ASCII whitespace and uppercases ASCII letters for Gemini
// `thinkingConfig.thinkingLevel` values (e.g. " high " -> "HIGH", "" -> "").
std::string normalize_thinking_level(std::string_view level);

// Returns true iff `level` (after `normalize_thinking_level`) is empty
// (explicit fallback to `thinkingBudget`) or one of the Gemini thinking levels:
// "MINIMAL", "LOW", "MEDIUM", "HIGH".
bool is_valid_thinking_level(std::string_view level) noexcept;

struct ScriptEntry {
    std::size_t global_index = 0;
    std::string script_name;
    std::string array_key;  // "strings", "lines", or "" (direct array)
    std::size_t entry_index = 0;
    std::string type;
    std::string text_key;   // "text", "content", or "jp"
    std::string speaker;    // canonical English speaker or "NARRATION"
    std::string jp_text;
    std::string current_en;
};

struct WindowSlice {
    std::size_t target_start = 0;
    std::vector<ScriptEntry> context_before;
    std::vector<ScriptEntry> target;
    std::vector<ScriptEntry> context_after;
    // 1-based target IDs (`1..target.size()`) unlocked for LLM editing.
    std::set<int> editable_ids;
};

struct Improvement {
    std::size_t global_index = 0;
    std::string script_name;
    std::size_t entry_index = 0;
    std::string speaker;
    std::string jp_text;
    std::string old_en;
    std::string improved_en;
    std::string category = "mistranslation";
    std::string severity = "major";  // "critical" | "major" | "minor"
    std::string error_span;          // Optional offending span in `old_en`
    std::string fix_span;            // Optional replacement span in `improved_en`
    double confidence = 1.0;
    std::string reason;
};

struct OlsModel {
    double alpha = 0.0;
    double beta = 1.8;
    double sigma = 15.0;
    std::size_t sample_count = 0;
    bool valid = false;
};

struct LineWeaknessScore {
    std::size_t global_index = 0;
    double total_score = 0.0;
    double length_z = 0.0;
    double discourse_score = 0.0;
    double entropy_score = 0.0;
    double repetition_score = 0.0;
    double cusum_stat = 0.0;
    double smoothed_density = 0.0;
    std::string dominant_signal = "clean";
};

struct HotspotWindow {
    std::size_t offset = 0;
    std::size_t length = 0;
    std::size_t peak_index = 0;
    double energy = 0.0;
    double cusum_peak = 0.0;
    double z_score = 0.0;
    double peak_line_score = 0.0;
    std::string dominant_signal = "clean";
};

struct HotspotConfig {
    std::size_t window_size = kDefaultWindowSize;
    std::size_t nms_min_separation = kDefaultNmsMinSeparation;
    double gaussian_sigma = kDefaultGaussianSigma;
    double cusum_slack_k = kDefaultCusumSlackK;
    double z_threshold = kDefaultZThreshold;
    std::size_t max_windows = 0;
    bool skip_h_scenes = true;
    bool fix_punctuation = false;
    double contagion_defect_rate = kDefaultContagionDefectRate;
    std::size_t boundary_margin = kDefaultBoundaryMargin;
};

struct NeighborExpansion {
    bool expand_left = false;
    std::size_t left_offset = 0;
    std::size_t left_length = 0;
    bool expand_right = false;
    std::size_t right_offset = 0;
    std::size_t right_length = 0;
};

struct RadarPassageResult {
    int passage_id = 0;  // 1-based index inside packed Radar batch (`1..N`)
    std::size_t offset = 0;
    std::size_t length = 0;
    int critical_count = 0;
    int major_count = 0;
    int minor_count = 0;
    double mqm_score = 0.0;      // `25 * critical + 5 * major + 1 * minor`
    std::set<int> suspect_ids;   // 1-based target line IDs flagged by Stage 1 Radar
};

struct ValidationResult {
    bool accepted = false;
    ValidationRejection rejection = ValidationRejection::None;
    std::string rejection_reason;
};

struct TokenUsageSummary {
    std::size_t api_calls = 0;
    std::size_t radar_calls_made = 0;
    std::size_t repair_calls_made = 0;
    std::int64_t prompt_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t thought_tokens = 0;
};

struct SpotCheckConfig {
    std::string game_dir;
    SpotCheckMode mode = SpotCheckMode::Hotspot;
    double z_threshold = kDefaultZThreshold;
    double min_confidence = kDefaultMinConfidence;
    bool review_all_in_window = true;
    bool skip_h_scenes = true;
    bool fix_punctuation = false;
    bool auto_apply = true;
    // Offline 0-API-call mode: runs full statistical OLS + 6-signal + CUSUM/KDE/NMS
    // hotspot scan without requiring `GEMINI_API_KEY`. When `!out_path.empty()`,
    // writes the JSON scan report ONLY to `out_path`; when `out_path.empty()`,
    // performs zero disk writes anywhere. Never mutates any file in `game_dir`.
    bool scan_only = false;
    std::size_t radar_calls = kDefaultRadarCalls;
    std::size_t radar_pack_size = kDefaultRadarPackSize;
    std::size_t offset = 0;
    std::size_t window_size = kDefaultWindowSize;
    std::size_t context_before = kDefaultContextBefore;
    std::size_t context_after = kDefaultContextAfter;
    std::size_t max_windows = kDefaultMaxWindows;
    // Hard ceiling across Stage 1 Radar + Stage 2 repair LLM calls per run
    // (`0` = unlimited). When reached, `SpotChecker::run()` stops cleanly after
    // persisting checkpoint progress.
    std::size_t max_api_calls = 0;
    std::size_t periodic_sync_interval = kDefaultPeriodicSyncInterval;
    std::string out_path;
    std::string model{kDefaultArgonModel};
    std::string thinking_level{kDefaultThinkingLevel};
};

struct SpotCheckReport {
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
    std::size_t improvements_found = 0;
    std::size_t script_entries_updated = 0;
    std::size_t cache_entries_updated = 0;
    TokenUsageSummary token_usage;
    std::vector<std::size_t> checked_offsets;
    std::vector<std::pair<std::size_t, std::size_t>> checked_spans;
    std::vector<std::size_t> blocked_offsets;
    std::vector<std::pair<std::size_t, std::size_t>> blocked_spans;
    std::vector<HotspotWindow> ranked_hotspots;
    std::vector<Improvement> improvements;
};

struct ApplyReport {
    std::size_t improvements_loaded = 0;
    std::size_t script_entries_updated = 0;
    std::size_t cache_entries_updated = 0;
};

}  // namespace spot_checker
