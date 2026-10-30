// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// PIMPL-insulated sliding-window spot-checker and statistical hotspot runner
// for visual novel translations, featuring:
//   - Full-novel statistical hotspot detection + adaptive contagion
//   - Pinpointed suspect-only locking inside each context window (editable_ids)
//   - Strict defect taxonomy & confidence threshold (confidence >= 0.85)
//   - Deterministic C++ "Do-No-Harm" post-edit validator
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "checker/hotspot_detector.h"

namespace spot_checker {

struct ScriptEntry {
    std::size_t global_index = 0;
    std::string script_name;
    std::string array_key;      // "strings" or "lines"
    std::size_t entry_index = 0;
    std::string text_key;       // "text" or "content"
    std::string speaker;        // speaker name or "NARRATION"
    std::string jp_text;
    std::string current_en;
};

struct WindowSlice {
    std::size_t target_start = 0;
    std::vector<ScriptEntry> context_before;
    std::vector<ScriptEntry> target;
    std::vector<ScriptEntry> context_after;
    // Trick 1: 1-based target IDs (1..target.size()) unlocked for editing.
    // When empty, all target lines 1..target.size() are editable.
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
    std::string severity = "major";  // GEMBA-MQM severity: "critical" | "major" | "minor"
    std::string error_span;          // Optional GEMBA-MQM offending span in old_en
    double confidence = 1.0;
    std::string reason;
};

struct RadarPassageResult {
    int passage_id = 0;        // 1-based index inside the packed Radar batch
    std::size_t offset = 0;
    std::size_t length = 0;
    double mqm_score = 0.0;    // GEMBA-MQM weighted score (critical=25, major=5, minor=1)
    std::set<int> suspect_ids; // 1-based target line IDs flagged by Stage 1 Radar
};

struct ValidationResult {
    bool accepted = false;
    std::string rejection_reason;
};

enum class SpotCheckMode {
    Hotspot,  // Full-novel statistical hotspot detection + adaptive contagion
    Linear,   // Sequential stepping from offset
};

struct SpotCheckConfig {
    std::string game_dir;
    SpotCheckMode mode = SpotCheckMode::Hotspot;
    double z_threshold = 1.5;
    double min_confidence = 0.85;
    bool review_all_in_window = false;  // false = lock non-suspect lines as read-only
    bool skip_h_scenes = true;          // Skip explicit H-scene lines and windows by default
    bool fix_punctuation = false;       // Do not fix punctuation/brackets/parentheses by default
    bool auto_apply = true;             // Checkpoint-apply fixes to translated_text.json & caches
    std::size_t radar_calls = 0;        // Stage 1 Packed LLM Radar calls (6 passages/call)
    std::size_t offset = 0;
    std::size_t window_size = 40;
    std::size_t context_before = 40;
    std::size_t context_after = 20;
    std::size_t max_windows = 1;  // 0 = all windows (or all hotspots >= z_threshold)
    std::string out_path;         // default: <game_dir>/script_output/argon_spot_check.json
};

struct SpotCheckReport {
    std::size_t total_entries = 0;
    std::size_t hotspots_detected = 0;
    std::size_t h_windows_skipped = 0;
    std::size_t radar_calls_made = 0;
    std::size_t radar_passages_triaged = 0;
    std::size_t adaptive_expansions = 0;
    std::size_t windows_checked = 0;
    std::size_t lines_checked = 0;
    std::size_t editable_lines_unlocked = 0;
    std::size_t candidates_rejected_by_validator = 0;
    std::size_t improvements_found = 0;
    std::size_t script_entries_updated = 0;
    std::size_t cache_entries_updated = 0;
    std::vector<HotspotWindow> ranked_hotspots;
    std::vector<Improvement> improvements;
};

struct ApplyReport {
    std::size_t improvements_loaded = 0;
    std::size_t script_entries_updated = 0;
    std::size_t cache_entries_updated = 0;
};

// Callback signature for injecting either ArgonClient or an offline test mock.
using ModelCallback = std::function<std::string(const std::string& system_prompt,
                                                const std::string& user_prompt)>;

// ---- Pure helper functions (exposed for direct unit testing) ---------------

// Built-in editorial rules enforcing professional literary prose, author intent,
// non-Westernization, zero censorship, and the Do-No-Harm JSON schema.
std::string default_editorial_rules_prompt(bool fix_punctuation = false);

// Combines default_editorial_rules_prompt() with <game_dir>/TONE_OF_NOVEL.md.
std::string build_system_prompt(const std::string& tone_markdown,
                                bool fix_punctuation = false);

// Parses JP -> EN glossary pairs from TONE_OF_NOVEL.md (both `<name_reference>`
// `JP=EN` items and markdown table `| \`JP\` | \`EN\` |` rows).
std::vector<std::pair<std::string, std::string>> parse_glossary_from_tone_md(
    const std::string& tone_markdown);

// Extracts all translatable dialogue/narration entries from a parsed
// translated_text.json root value (supporting both "strings" and "lines").
std::vector<ScriptEntry> extract_script_entries(const boost::json::value& root);

// Builds a sliding window [offset, offset + window_size) with context_before
// and context_after slices.
WindowSlice build_window_slice(const std::vector<ScriptEntry>& entries,
                               std::size_t offset,
                               std::size_t window_size,
                               std::size_t context_before,
                               std::size_t context_after);

// Trick 1: Computes the 1-based editable_ids allowlist inside `slice`:
// unlocks only lines with s_i > 0 plus their immediate +/- 1 banter partners
// (unless review_all_in_window is true), never unlocking H-scene lines when
// skip_h_scenes is true or passthrough/non-JP lines.
std::set<int> compute_editable_ids(const WindowSlice& slice,
                                   const std::vector<LineWeaknessScore>& novel_scores,
                                   bool review_all_in_window = false,
                                   bool skip_h_scenes = true);

// Formats a WindowSlice into the structured prompt sent to gemini-4-argon,
// annotating each target line as [EDITABLE SUSPECT] or [READ-ONLY CONTEXT].
std::string build_spot_check_user_prompt(const WindowSlice& slice,
                                         bool fix_punctuation = false);

// Stage 1 Packed LLM Radar: formats up to 6 candidate passages into a single
// fast GEMBA-MQM triage prompt (Kocmi & Federmann WMT 2023).
std::string build_radar_pack_prompt(const std::vector<WindowSlice>& passages);

// Stage 1 Packed LLM Radar: parses the JSON array response from the Radar pass
// (`[{"passage_id": 1, "mqm_score": 25, "suspect_ids": [2, 3]}]`).
std::vector<RadarPassageResult> parse_radar_pack_response(
    const std::string& raw_response,
    const std::vector<WindowSlice>& passages);

// Trick 2: Validates whether `category` is one of the 6 allowed defect types.
bool is_valid_defect_category(const std::string& category);

// Trick 3: Deterministic C++ "Do-No-Harm" Post-Edit Validator (plus Stage 2
// GEMBA-MQM `error_span` surgical removal verification).
ValidationResult validate_proposed_fix(
    const ScriptEntry& entry,
    const std::string& candidate_en,
    const std::vector<std::pair<std::string, std::string>>& glossary = {},
    const OlsModel& ols = {},
    const std::string& error_span = {},
    bool fix_punctuation = false);

// Parses and validates the JSON array response from gemini-4-argon:
//   [{"id": 1..N, "new_en": "...", "category": "...", "confidence": 0.92, "reason": "..."}]
// (Also accepts "improved_en" for backward compatibility.)
// Enforces:
//   - id bounds and Trick 1 `slice.editable_ids` allowlist
//   - Trick 2 defect category taxonomy and `confidence >= min_confidence`
//   - Speaker\r\n prefix restoration and sanitize_ascii
//   - Trick 3 `validate_proposed_fix` (quote envelope, zero CJK/romaji notes,
//     placeholders, glossary invariance, and mathematical non-regression)
std::vector<Improvement> parse_spot_check_response(
    const std::string& raw_response,
    const WindowSlice& slice,
    double min_confidence = 0.85,
    const std::vector<std::pair<std::string, std::string>>& glossary = {},
    const OlsModel& ols = {},
    std::size_t* out_rejected_count = nullptr,
    bool fix_punctuation = false);

// Loads/saves translation_cache_anthropic.json preserving insertion order.
std::vector<std::pair<std::string, std::string>> load_ordered_cache(
    const std::string& cache_path);
void save_ordered_cache(
    const std::vector<std::pair<std::string, std::string>>& items,
    const std::string& cache_path);

// ---- PIMPL-insulated SpotChecker class -------------------------------------

class SpotChecker {
public:
    explicit SpotChecker(SpotCheckConfig config, ModelCallback callback = {});
    ~SpotChecker();

    SpotChecker(SpotChecker&& other) noexcept;
    SpotChecker& operator=(SpotChecker&& other) noexcept;

    SpotChecker(const SpotChecker&) = delete;
    SpotChecker& operator=(const SpotChecker&) = delete;

    // Runs the spot-check pass (Hotspot mode or Linear mode) and writes/updates
    // argon_spot_check.json.
    SpotCheckReport run();

    // Applies the saved improvements in argon_spot_check.json back into
    // translated_text.json and translation_cache_anthropic.json (preserving
    // insertion order in the cache). Makes zero API calls.
    ApplyReport apply_improvements();

    const std::vector<ScriptEntry>& entries() const;
    const std::string& system_prompt() const;
    std::string output_json_path() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spot_checker
