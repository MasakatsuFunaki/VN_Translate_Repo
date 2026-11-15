// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "domain/spot_check_types.h"
#include "domain/tone_glossary.h"

namespace spot_checker {

inline constexpr double kScoreRegressionTolerance = 0.25;
inline constexpr std::size_t kRunawayCompressionMinOldBytes = 500;
inline constexpr std::size_t kMaxExpansionMinNewBytes = 140;
inline constexpr std::size_t kMaxExpansionFactor = 3;
inline constexpr std::size_t kMinOldBytesForTruncationCheck = 30;

struct ValidatorOptions {
    double min_confidence = kDefaultMinConfidence;
    bool fix_punctuation = false;
};

// Deterministic C++ post-edit non-regression gate evaluated in strict order:
// 1. `EmptyTranslation`: stripped `new_body` is empty/whitespace.
// 2. `UntranslatedPassthrough`: `entry` is passthrough/non-JP, or
//    `candidate.improved_en == entry.current_en`, or `== entry.jp_text`.
// 3. `InvalidDefectCategory`: `!is_valid_defect_category(candidate.category)`.
// 4. `InvalidConfidence`: `!std::isfinite(candidate.confidence)` or outside
//    `[options.min_confidence, 1.0]`.
// 5. `PunctuationOnlyDisabled`: `!options.fix_punctuation` and alphanumeric
//    lowercase token sequence of `old_body` equals `new_body` (unless
//    compressing a runaway `> kRunawayCompressionMinOldBytes` loop or removing
//    leaked Japanese).
// 6. `ErrorSpanNotChanged`: non-empty `candidate.error_span` occurs in
//    `old_body` and still occurs at `>=` the same count in `new_body` (unless
//    compressing a runaway loop).
// 7. `LeakedJapaneseCharacters`: `new_body` contains Japanese characters (when
//    `old_body` uses `「...」`, checks `has_real_japanese`; otherwise checks
//    `has_japanese`).
// 8. `LeakedFullwidthOrCjkPunctuation`: `has_cjk_or_fullwidth_punctuation(new_body)`
//    (allowing outer `「...」` only when `old_body` used outer `「...」`).
// 9. `IntroducedTranslatorNote`: `!contains_translator_romaji_note(old_body)`
//    and `contains_translator_romaji_note(new_body)`.
// 10. `QuoteEnvelopeMismatch`: `!quote_envelopes_compatible(...)` across
//     `jp_body`, `old_body`, and `new_body`.
// 11. `ControlTokenMismatch`: `extract_engine_control_tokens(old_body) !=
//     extract_engine_control_tokens(new_body)` (exact sequence & count equality,
//     rejecting both dropped and hallucinated `%s`/`%d`/`$strNN`/`\n`/`\@`/tags).
// 12. `GlossaryTermDropped`: `!glossary.check_glossary_preservation(jp_body,
//     old_body, new_body)` (enforcing 3rd-person-pronoun-delta rule).
// 13. `WeaknessScoreRegression`: `score_single_line(..., apply_h_skip = false)`
//     yields `after.total_score > before.total_score + kScoreRegressionTolerance`
//     or (`before.total_score == 0.0 && after.total_score > 0.0`).
// 14. `ExtremeLengthDistortion`: `new_body.size() > kMaxExpansionMinNewBytes &&
//     new_body.size() > old_body.size() * kMaxExpansionFactor`, or `>50%`
//     truncation when `old_body.size() >= kMinOldBytesForTruncationCheck` and
//     `before.length_z <= 3.0`, or newly triggered dropped-clause `length_z`.
ValidationResult validate_proposed_fix(
    const ScriptEntry& original,
    const Improvement& candidate,
    const ToneGlossary& glossary,
    const OlsModel& ols_model,
    const ValidatorOptions& options = {});

}  // namespace spot_checker
