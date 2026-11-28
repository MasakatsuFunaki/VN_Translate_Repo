// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Test-only 8-line real `CROSS_CHANNEL` prose Model-Degradation Baseline Gauntlet
// enforcing reasoning-depth floor (`thought_tokens >= 150`) and 5 literary/wordplay
// regression gates against silent model quantization or reasoning throttling.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vn::llm::testing {

inline constexpr std::int64_t kBaselineMinThoughtTokens = 150;
inline constexpr std::size_t kGauntletLineCount = 8;
inline constexpr std::size_t kGauntletLookaheadLineCount = 4;

struct GauntletLine {
    std::string speaker;
    std::string jp;
    std::int64_t offset = 0;
};

struct GauntletVerdict {
    bool passed = false;
    std::vector<std::string> failures;
};

// Returns the canonical 8-line Romeo Tanaka `CROSS_CHANNEL` trap batch.
[[nodiscard]] const std::vector<GauntletLine>& cross_channel_gauntlet_batch();

// Returns the 4 upcoming `<context_after>` lookahead lines revealing `巨大なサメ`.
[[nodiscard]] const std::vector<GauntletLine>&
cross_channel_gauntlet_context_after();

// Builds the numbered `<lines_to_translate>` + `<context_after>` user prompt.
[[nodiscard]] std::string build_gauntlet_user_prompt();

// Returns the `CROSS_CHANNEL` literary translation system prompt used by the gauntlet.
[[nodiscard]] std::string gauntlet_system_prompt();

// Strictly parses `expected_count` sequential numbered lines (`1. ...` .. `N. ...`)
// from `response_text`, stripping optional outer markdown fences and accidental
// `[Speaker]` tags. Returns an empty vector if numbering is incomplete, out of
// order, or malformed.
[[nodiscard]] std::vector<std::string> parse_gauntlet_numbered_lines(
    std::string_view response_text,
    std::size_t expected_count = kGauntletLineCount);

// Evaluates all 6 baseline model-degradation gates (plus structural/charset sanity):
//   1. `thought_tokens >= kBaselineMinThoughtTokens` (150)
//   2. `欧羅巴(ヨーロッパ)` + `天蓋つきの寝台` -> `"europ"` + `"canop"`/`"four-poster"`, no `"yooroppa"`
//   3. `先輩がた` -> `「がたって」` -> plural grouping + plural retort, no `"gata"`/`"rattle"`/`"clatter"`
//   4. `みゆきがエクスタシーに達した（嘘）。` -> preserves deadpan `"lie"`/`"not"`/`"kidding"`/`"false"`/`"fake"`/`"as if"` aside
//   5. `友好 -> 有効` homophone typo in conquest monologue -> strategic effectiveness, no `"friend"`/`"amicab"`
//   6. `ひどいサメ肌` + `<context_after>` `巨大なサメ` -> preserves `"shark"` pun (no `"sandpaper"` substitution)
//   (Plus exact 8 non-empty lines, zero leaked Japanese kana/kanji, and `%N` preservation on line 6.)
[[nodiscard]] GauntletVerdict evaluate_gauntlet_outputs(
    std::span<const std::string> eight_lines,
    std::int64_t thought_tokens);

}  // namespace vn::llm::testing
