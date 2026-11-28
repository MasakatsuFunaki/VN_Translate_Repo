// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "domain/spot_check_types.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace spot_checker {

// Pellegrino information-density weights per character class in `jp_body`:
inline constexpr double kKanjiInfoBits = 2.4;
inline constexpr double kKatakanaInfoBits = 1.15;
inline constexpr double kHiraganaInfoBits = 1.0;
inline constexpr double kAsciiAlnumInfoBits = 0.75;
inline constexpr double kOtherCharInfoBits = 0.25;

// Gale-Church / Pellegrino OLS length regression boundaries:
inline constexpr std::size_t kMinOlsSampleCount = 4;
inline constexpr double kMinOlsVarianceX = 9.0;
inline constexpr std::size_t kMaxOlsCleanEnBytes = 2000;
inline constexpr double kOlsBetaMin = 0.6;
inline constexpr double kOlsBetaMax = 3.0;
inline constexpr double kOlsSigmaMin = 8.0;
inline constexpr double kOlsSigmaMax = 40.0;

// Length residual Z-score bounds & dropped-clause thresholds:
inline constexpr double kLengthZMin = -4.0;
inline constexpr double kLengthZMax = 10.0;
inline constexpr double kLengthZDeadband = 3.0;
inline constexpr double kDroppedClauseRatio = 0.35;
inline constexpr double kDroppedClauseMinJpBits = 18.0;
inline constexpr double kDroppedClausePenaltyZ = 3.5;

// Single-line & multi-turn weakness signal weights:
inline constexpr double kRunawayCharRepeatPenalty = 6.0;
inline constexpr double kRunawayTrigramLoopPenalty = 7.0;
inline constexpr double kLeakedJpInEnPenalty = 8.0;
inline constexpr double kRomajiGlossPenalty = 4.0;
inline constexpr double kQuoteFramingMismatchPenalty = 3.2;
inline constexpr double kBenefactiveInversionPenalty = 4.5;
inline constexpr double kDroppedFirstPersonPenalty = 2.6;
inline constexpr double kBrokenQaEchoPenalty = 2.8;
inline constexpr double kBrokenLexicalCohesionPenalty = 2.0;
inline constexpr double kBrokenCallbackShortLineBonus = 1.2;
inline constexpr double kYakuwarigoLossPenalty = 2.2;
inline constexpr double kSpeakerRegisterDriftPenalty = 2.0;
inline constexpr double kYulesKThreshold = 150.0;
inline constexpr double kLowEntropyThreshold = 2.6;

struct PreparedLine {
    std::string jp_body;
    std::string en_body;
    std::string lower_en_body;
    double jp_info_bits = 0.0;
    bool is_passthrough_or_silence = false;
    bool is_h_scene = false;
    std::vector<std::string> kanji_compounds;
    std::vector<std::string> jp_callback_tokens;
    std::vector<std::string> en_content_stems;
};

// Computes weighted Japanese script information content (in bits) over `jp_body`.
// Kanji (including '々') count as `kKanjiInfoBits`, lexical Katakana as
// `kKatakanaInfoBits`, Hiragana as `kHiraganaInfoBits`, ASCII alphanumeric as
// `kAsciiAlnumInfoBits`, and non-whitespace punctuation/symbols as `kOtherCharInfoBits`.
double compute_script_info_bits(std::string_view jp_body) noexcept;

// Precomputes stripped dialogue bodies, lowercase ASCII projection, information
// bits, H-scene flag, >=2-kanji compounds (including 'X々'), particle-filtered
// verb/adjective callback stems, and contraction-aware English content stems.
PreparedLine prepare_line(const ScriptEntry& entry);

// Fits a single-pass Welford ordinary-least-squares linear model
// `expected_en_len = alpha + beta * jp_info_bits` across clean translated rows:
// - Excludes passthrough (`en == jp`), silence/punctuation-only lines,
//   `jp_info_bits < 1.0`, runaway lines (`en_body.size() > kMaxOlsCleanEnBytes`),
//   and sustained H-scene spans (`compute_h_scene_line_mask(entries)`) when
//   `skip_h_scenes == true` (retaining non-H dirty/crude dialogue outside
//   sustained H-scene spans).
// - Requires `sample_count >= kMinOlsSampleCount` and `sum_xx / n > kMinOlsVarianceX`;
//   otherwise returns default fallback `OlsModel{..., valid = false}`.
// - Uses unbiased `sample_count - 2` degrees of freedom for residual `sigma`,
//   clamping `beta` to `[kOlsBetaMin, kOlsBetaMax]` and `sigma` to
//   `[kOlsSigmaMin, kOlsSigmaMax]`.
OlsModel fit_length_regression(const std::vector<ScriptEntry>& entries,
                               bool skip_h_scenes);

// Computes deterministic single-line weakness signals on `entry`:
// - When `apply_h_skip == true` and `is_h_scene_entry` holds, returns score
//   `0.0` (`dominant_signal = "clean"`). Both `score_corpus_lines` (which gates
//   H-scene skipping strictly via corpus-level `compute_h_scene_line_mask`) and
//   post-edit validation pass `apply_h_skip = false` so non-H dirty banter is
//   scored normally and candidate edits introducing adult terms never bypass
//   defect scoring.
// - Evaluates Gale-Church / Pellegrino length residual (`|length_z| > kLengthZDeadband`
//   plus dropped-clause check), runaway char/trigram repetition, leaked Japanese,
//   benefactive `てくれ` / `てもらう` inversion, dropped explicit 1st-person subject
//   (`俺`/`僕`/`私`/`あたし`/`ウチ`/`ワシ`), boundary-checked single-line Yakuwarigo
//   (`にゃ`/`かにゃ`/`ですわ`/`でござる` excluding particles like `俺にゃ` and mimetics),
//   and (when `fix_punctuation == true`) romaji notes and quote framing mismatches.
LineWeaknessScore score_single_line(const ScriptEntry& entry,
                                    const OlsModel& ols_model,
                                    bool apply_h_skip,
                                    bool fix_punctuation = false);

// Scores an ordered corpus combining `score_single_line` with multi-turn signals:
// - When `cfg.skip_h_scenes == true`, lines inside sustained H-scene spans
//   (`compute_h_scene_line_mask(entries)`) score `0.0` (`"clean"`) and are
//   excluded from multi-turn signals, while short non-H dirty/crude banter
//   outside sustained H-scene spans retains full single-line and multi-turn
//   defect scoring.
// - Same-script Q-A verb echo across immediate turns (`i - 1 -> i`).
// - 5-turn BlonDe kanji-compound lexical cohesion (`[max(0, i - 5), i)` within
//   the same `script_name`).
// - Particle-filtered callback / voice-tic echo across short turns.
// - Sliding-window Yule's K lexical monotony and character-bigram Shannon entropy.
// - Per-speaker EWMA register/length drift tracked per canonical `entry.speaker`.
std::vector<LineWeaknessScore> score_corpus_lines(
    const std::vector<ScriptEntry>& entries,
    const OlsModel& ols_model,
    const HotspotConfig& cfg);

}  // namespace spot_checker
