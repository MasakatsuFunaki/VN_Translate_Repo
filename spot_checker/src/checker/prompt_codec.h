// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "common/status.h"
#include "domain/spot_check_types.h"
#include "domain/tone_glossary.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace spot_checker {

struct ParsedSpotCheckBatch {
    std::vector<Improvement> candidates;
    std::size_t rejected_in_parse = 0;
};

// Built-in editorial rules enforcing licensed commercial fictional/psychological
// visual novel localization QA framing, literary prose, fidelity to Japanese
// nuance, zero censorship/Westernization, and the 6-category JSON schema.
std::string default_editorial_rules_prompt(bool fix_punctuation = false);

// Combines `default_editorial_rules_prompt(fix_punctuation)` with
// `glossary.sanitized_guide_markdown()`.
std::string build_system_prompt(const ToneGlossary& glossary,
                                bool fix_punctuation = false);

// Redacts entries in `slice` (`context_before`, `target`, `context_after`) whose
// `global_index` is marked `true` in `h_scene_mask` (sustained multi-line
// H-scene spans) with `[REDACTED EXPLICIT SCENE LINE]` placeholders and removes
// redacted target IDs from `editable_ids`. Never redacts or alters unmasked
// lines (`h_scene_mask.empty()` or `h_scene_mask[global_index] == false`), so
// isolated dirty/crude banter in normal dialogue is preserved verbatim.
WindowSlice redact_h_lines_for_prompt(
    const WindowSlice& slice,
    const std::vector<bool>& h_scene_mask = {});

// Formats `slice` into the Stage 2 user prompt, labeling target lines 1..N as
// `[EDITABLE SUSPECT]` (when `id` is in `slice.editable_ids`) or
// `[READ-ONLY CONTEXT]`.
std::string build_spot_check_user_prompt(const WindowSlice& slice,
                                         bool fix_punctuation = false);

// Stage 1 Packed LLM Radar: packs up to `kDefaultRadarPackSize` (6) candidate
// `WindowSlice` passages into a single GEMBA-MQM severity-triage prompt framed
// as licensed commercial fictional/psychological visual novel localization QA.
std::string build_radar_pack_prompt(const std::vector<WindowSlice>& passages,
                                    const ToneGlossary& glossary = {});

// Parses Stage 1 Radar JSON array response:
// - Accepts either explicit `"mqm_score"` or `{critical_count, major_count,
//   minor_count}` (`25 * critical + 5 * major + 1 * minor` fallback when
//   `mqm_score` is absent or negative).
// - Filters out non-object items, duplicate `passage_id`s, and out-of-bounds
//   `passage_id`s outside `1..passages.size()`, populating `offset` and `length`
//   from `passages[passage_id - 1]`.
// - Returns `ErrorCode::CorruptJson` if `raw_response` has no parseable JSON array.
Result<std::vector<RadarPassageResult>> parse_radar_pack_response(
    std::string_view raw_response,
    const std::vector<WindowSlice>& passages);

// Parses Stage 2 JSON array response into candidate `Improvement` structs
// prior to deterministic `validate_proposed_fix` gating:
// - Accepts `"new_en"` (preferred) or `"improved_en"`.
// - Enforces `1 <= id <= slice.target.size()`, `slice.editable_ids` membership,
//   no duplicate `id` within the same batch, valid 6-value `category`, valid
//   `severity` (defaulting omitted severity to `"major"`), and **strictly
//   present finite numeric `confidence` in `[min_confidence, 1.0]`** (missing,
//   non-numeric, `NaN`, `< min_confidence`, or `> 1.0` increments
//   `rejected_in_parse` and discards the item).
// - Restores speaker prefix, newline escaping, and ASCII punctuation via
//   `normalize_restored_translation` + `sanitize_ascii_punctuation`.
// - Returns `ErrorCode::CorruptJson` if `raw_response` contains no valid JSON array.
Result<ParsedSpotCheckBatch> parse_spot_check_response(
    std::string_view raw_response,
    const WindowSlice& slice,
    double min_confidence = kDefaultMinConfidence);

}  // namespace spot_checker
