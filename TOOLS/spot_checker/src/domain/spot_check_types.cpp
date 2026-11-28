// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "domain/spot_check_types.h"

#include <array>

namespace spot_checker {

namespace {

constexpr std::array<std::string_view, 6> kAllowedDefectCategories = {
    "mistranslation",
    "broken_callback",
    "artifact_or_note",
    "character_voice",
    "censored_or_westernized",
    "stiff_translationese",
};

constexpr std::array<std::string_view, 3> kAllowedSeverities = {
    "critical",
    "major",
    "minor",
};

}  // namespace

std::string_view to_string(ValidationRejection code) noexcept {
    switch (code) {
    case ValidationRejection::None:
        return "none";
    case ValidationRejection::EmptyTranslation:
        return "empty_translation";
    case ValidationRejection::UntranslatedPassthrough:
        return "untranslated_passthrough";
    case ValidationRejection::InvalidDefectCategory:
        return "invalid_defect_category";
    case ValidationRejection::InvalidConfidence:
        return "invalid_confidence";
    case ValidationRejection::PunctuationOnlyDisabled:
        return "punctuation_only_disabled";
    case ValidationRejection::ErrorSpanNotChanged:
        return "error_span_not_changed";
    case ValidationRejection::LeakedJapaneseCharacters:
        return "leaked_japanese_characters";
    case ValidationRejection::LeakedFullwidthOrCjkPunctuation:
        return "leaked_fullwidth_or_cjk_punctuation";
    case ValidationRejection::IntroducedTranslatorNote:
        return "introduced_translator_note";
    case ValidationRejection::QuoteEnvelopeMismatch:
        return "quote_envelope_mismatch";
    case ValidationRejection::ControlTokenMismatch:
        return "control_token_mismatch";
    case ValidationRejection::GlossaryTermDropped:
        return "glossary_term_dropped";
    case ValidationRejection::WeaknessScoreRegression:
        return "weakness_score_regression";
    case ValidationRejection::ExtremeLengthDistortion:
        return "extreme_length_distortion";
    }
    return "unknown";
}

bool is_valid_defect_category(std::string_view category) noexcept {
    for (const std::string_view allowed : kAllowedDefectCategories) {
        if (category == allowed) {
            return true;
        }
    }
    return false;
}

bool is_valid_severity(std::string_view severity) noexcept {
    for (const std::string_view allowed : kAllowedSeverities) {
        if (severity == allowed) {
            return true;
        }
    }
    return false;
}

std::string normalize_thinking_level(std::string_view level) {
    return vn::llm::normalize_thinking_level(level);
}

bool is_valid_thinking_level(std::string_view level) noexcept {
    return vn::llm::is_valid_thinking_level(level);
}

}  // namespace spot_checker
