// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "checker/fix_validator.h"

#include "common/utf8_text.h"
#include "detector/weakness_signals.h"
#include "domain/vn_line_syntax.h"

#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spot_checker {

namespace {

constexpr std::string_view kLiteralNewlineEscape = "\\n";
constexpr std::string_view kLiteralWaitEscape = "\\@";
constexpr std::string_view kCornerSingleOpen = "「";
constexpr std::string_view kCornerSingleClose = "」";
constexpr std::string_view kCornerDoubleOpen = "『";
constexpr std::string_view kCornerDoubleClose = "』";
constexpr std::size_t kCornerBracketPairBytes =
    kCornerSingleOpen.size() + kCornerSingleClose.size();
constexpr double kMaxValidConfidence = 1.0;
constexpr std::size_t kHalfLengthDivisor = 2;

bool starts_with_sv(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with_sv(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

ValidationResult make_rejection(ValidationRejection code, std::string reason) {
    return ValidationResult{false, code, std::move(reason)};
}

std::string strip_escapes_and_whitespace(std::string_view body) {
    std::string cur = trim_unicode(body);
    while (starts_with_sv(cur, kLiteralNewlineEscape)) {
        cur = trim_unicode(
            std::string_view(cur).substr(kLiteralNewlineEscape.size()));
    }
    while (ends_with_sv(cur, kLiteralWaitEscape)) {
        cur = trim_unicode(std::string_view(cur).substr(
            0, cur.size() - kLiteralWaitEscape.size()));
    }
    return cur;
}

bool is_japanese_script_cp(char32_t cp) noexcept {
    return is_hiragana(cp) || is_lexical_katakana(cp) || is_kanji(cp) ||
           cp == U'ー' || cp == U'ｰ' || cp == U'ゝ' || cp == U'ゞ' ||
           cp == U'ヽ' || cp == U'ヾ';
}

bool is_word_token_cp(char32_t cp) noexcept {
    return (cp >= U'a' && cp <= U'z') || (cp >= U'0' && cp <= U'9') ||
           is_japanese_script_cp(cp);
}

std::vector<std::string> extract_lower_word_tokens(std::string_view text) {
    const std::string lower = to_lower_ascii(text);
    const std::string_view view(lower);
    std::vector<std::string> tokens;
    std::size_t idx = 0;
    while (idx < view.size()) {
        while (idx < view.size()) {
            const std::size_t probe = idx;
            const char32_t cp = utf8_next(view, idx);
            if (is_word_token_cp(cp)) {
                idx = probe;
                break;
            }
        }
        const std::size_t start = idx;
        while (idx < view.size()) {
            const std::size_t probe = idx;
            const char32_t cp = utf8_next(view, idx);
            if (!is_word_token_cp(cp)) {
                idx = probe;
                break;
            }
        }
        if (idx > start) {
            tokens.emplace_back(view.substr(start, idx - start));
        }
    }
    return tokens;
}

std::size_t count_non_overlapping(std::string_view haystack,
                                  std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > haystack.size()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

bool contains_japanese_script_chars(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        if (is_japanese_script_cp(utf8_next(text, index))) {
            return true;
        }
    }
    return false;
}

std::string strip_outer_corner_brackets_once(std::string_view body) {
    std::string core = strip_escapes_and_whitespace(body);
    if (core.size() >= kCornerBracketPairBytes &&
        ((starts_with_sv(core, kCornerSingleOpen) &&
          ends_with_sv(core, kCornerSingleClose)) ||
         (starts_with_sv(core, kCornerDoubleOpen) &&
          ends_with_sv(core, kCornerDoubleClose)))) {
        return core.substr(kCornerSingleOpen.size(),
                           core.size() - kCornerBracketPairBytes);
    }
    return core;
}

}  // namespace

ValidationResult validate_proposed_fix(const ScriptEntry& original,
                                       const Improvement& candidate,
                                       const ToneGlossary& glossary,
                                       const OlsModel& ols_model,
                                       const ValidatorOptions& options) {
    const SpeakerSplit old_split = split_speaker_prefix(original.current_en);
    const std::string& old_body = old_split.body;

    std::string new_body;
    if (!old_split.prefix.empty() &&
        starts_with_sv(candidate.improved_en, old_split.prefix)) {
        new_body = candidate.improved_en.substr(old_split.prefix.size());
    } else {
        new_body = strip_speaker_prefix_body(candidate.improved_en);
    }

    const std::string stripped_new_core = strip_escapes_and_whitespace(new_body);
    if (stripped_new_core.empty()) {
        return make_rejection(ValidationRejection::EmptyTranslation,
                              "Candidate translation body is empty.");
    }

    const std::string jp_body =
        strip_inline_control_wrappers(strip_speaker_prefix_body(original.jp_text));
    if (is_engine_asset_or_command_token(original.jp_text, original.current_en) ||
        is_pure_silence_or_punctuation(jp_body) ||
        original.current_en == original.jp_text ||
        (!jp_body.empty() && old_body == jp_body) ||
        candidate.improved_en == original.current_en || new_body == old_body ||
        candidate.improved_en == original.jp_text || new_body == jp_body) {
        return make_rejection(
            ValidationRejection::UntranslatedPassthrough,
            "Entry is non-translatable passthrough or candidate is unchanged.");
    }

    if (!is_valid_defect_category(candidate.category)) {
        return make_rejection(ValidationRejection::InvalidDefectCategory,
                              "Unrecognized defect category: " + candidate.category);
    }

    if (!std::isfinite(candidate.confidence) ||
        candidate.confidence < options.min_confidence ||
        candidate.confidence > kMaxValidConfidence) {
        return make_rejection(ValidationRejection::InvalidConfidence,
                              "Candidate confidence is outside [min_confidence, 1.0].");
    }

    const bool compressing_runaway =
        old_body.size() > kRunawayCompressionMinOldBytes &&
        new_body.size() < old_body.size();
    const bool removing_leaked_jp =
        contains_japanese_script_chars(old_body) &&
        !contains_japanese_script_chars(new_body);

    if (!options.fix_punctuation && !compressing_runaway && !removing_leaked_jp) {
        if (extract_lower_word_tokens(old_body) ==
            extract_lower_word_tokens(new_body)) {
            return make_rejection(
                ValidationRejection::PunctuationOnlyDisabled,
                "Punctuation-only edit rejected when fix_punctuation is false.");
        }
    }

    if (!candidate.error_span.empty() && !compressing_runaway) {
        const std::size_t old_span_count =
            count_non_overlapping(old_body, candidate.error_span);
        if (old_span_count > 0U) {
            const std::size_t new_span_count =
                count_non_overlapping(new_body, candidate.error_span);
            if (new_span_count >= old_span_count) {
                return make_rejection(
                    ValidationRejection::ErrorSpanNotChanged,
                    "Identified error_span remains unchanged in candidate.");
            }
        }
    }

    if (contains_japanese_script_chars(new_body)) {
        return make_rejection(
            ValidationRejection::LeakedJapaneseCharacters,
            "Candidate translation contains leaked Japanese characters.");
    }

    const QuoteEnvelope old_env = analyze_quote_envelope(old_body);
    const QuoteEnvelope new_env = analyze_quote_envelope(new_body);
    const bool shared_corner_envelope =
        (old_env.outer_kind == OuterWrapKind::CornerQuoteSingle &&
         new_env.outer_kind == OuterWrapKind::CornerQuoteSingle) ||
        (old_env.outer_kind == OuterWrapKind::CornerQuoteDouble &&
         new_env.outer_kind == OuterWrapKind::CornerQuoteDouble);
    if (shared_corner_envelope) {
        const std::string inner_new = strip_outer_corner_brackets_once(new_body);
        if (has_cjk_or_fullwidth_punctuation(inner_new)) {
            return make_rejection(
                ValidationRejection::LeakedFullwidthOrCjkPunctuation,
                "Candidate contains leaked CJK or fullwidth punctuation.");
        }
    } else if (has_cjk_or_fullwidth_punctuation(new_body)) {
        return make_rejection(
            ValidationRejection::LeakedFullwidthOrCjkPunctuation,
            "Candidate contains leaked CJK or fullwidth punctuation.");
    }

    if (!contains_translator_romaji_note(old_body) &&
        contains_translator_romaji_note(new_body)) {
        return make_rejection(
            ValidationRejection::IntroducedTranslatorNote,
            "Candidate introduces a translator or romaji gloss note.");
    }

    const QuoteEnvelope jp_env = analyze_quote_envelope(jp_body);
    if (!quote_envelopes_compatible(jp_env, old_env, new_env)) {
        return make_rejection(
            ValidationRejection::QuoteEnvelopeMismatch,
            "Candidate violates dialogue/thought quote framing.");
    }

    if (extract_engine_control_tokens(old_body) !=
        extract_engine_control_tokens(new_body)) {
        return make_rejection(
            ValidationRejection::ControlTokenMismatch,
            "Candidate alters inline engine placeholders or control tokens.");
    }

    std::string violated_term;
    if (!glossary.check_glossary_preservation(
            jp_body, old_body, new_body, &violated_term)) {
        return make_rejection(
            ValidationRejection::GlossaryTermDropped,
            "Candidate drops canonical glossary term: " + violated_term);
    }

    ScriptEntry candidate_entry = original;
    candidate_entry.current_en = candidate.improved_en;
    const LineWeaknessScore before_score =
        score_single_line(original, ols_model, false, options.fix_punctuation);
    const LineWeaknessScore after_score = score_single_line(
        candidate_entry, ols_model, false, options.fix_punctuation);

    if (after_score.total_score >
            before_score.total_score + kScoreRegressionTolerance ||
        (before_score.total_score == 0.0 && after_score.total_score > 0.0)) {
        return make_rejection(
            ValidationRejection::WeaknessScoreRegression,
            "Candidate increases deterministic line weakness score.");
    }

    const bool extreme_expansion =
        new_body.size() > kMaxExpansionMinNewBytes &&
        old_body.size() <=
            (std::numeric_limits<std::size_t>::max() / kMaxExpansionFactor) &&
        new_body.size() > old_body.size() * kMaxExpansionFactor;
    const bool extreme_truncation =
        old_body.size() >= kMinOldBytesForTruncationCheck &&
        before_score.length_z <= kLengthZDeadband &&
        new_body.size() * kHalfLengthDivisor < old_body.size();
    const bool newly_dropped_clause =
        before_score.length_z > -kDroppedClausePenaltyZ &&
        after_score.length_z <= -kDroppedClausePenaltyZ;

    if (extreme_expansion || extreme_truncation || newly_dropped_clause) {
        return make_rejection(
            ValidationRejection::ExtremeLengthDistortion,
            "Candidate exhibits extreme expansion or truncation.");
    }

    return ValidationResult{true, ValidationRejection::None, {}};
}

}  // namespace spot_checker
