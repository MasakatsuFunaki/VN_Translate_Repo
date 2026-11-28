// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "vn/llm/llm_types.h"

#include "core/ascii_text.h"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <string>

namespace vn::llm {
namespace {

constexpr std::int64_t kZeroTokens = 0;
constexpr std::size_t kZeroBatchLines = 0;
constexpr double kZeroRate = 0.0;
constexpr double kTokensPerMillion = 1'000'000.0;

constexpr double kCarbonInputCostPerMillionUsd = 2.00;
constexpr double kCarbonCachedInputCostPerMillionUsd = 0.20;
constexpr double kCarbonOutputCostPerMillionUsd = 10.00;

constexpr double kNeonInputCostPerMillionUsd = 3.00;
constexpr double kNeonCachedInputCostPerMillionUsd = 0.30;
constexpr double kNeonOutputCostPerMillionUsd = 15.00;

constexpr double kHydrogenInputCostPerMillionUsd = 1.50;
constexpr double kHydrogenCachedInputCostPerMillionUsd = 0.15;
constexpr double kHydrogenOutputCostPerMillionUsd = 7.50;

constexpr double kGemini31ProInputCostPerMillionUsd = 2.00;
constexpr double kGemini31ProCachedInputCostPerMillionUsd = 0.20;
constexpr double kGemini31ProOutputCostPerMillionUsd = 12.00;

constexpr std::string_view kDisallowedArgonSubstring = "argon";
constexpr std::string_view kNeonSubstring = "neon";
constexpr std::string_view kHydrogenSubstring = "hydrogen";
constexpr std::string_view kGemini31ProSubstring = "gemini-3.1-pro";
constexpr char kEmbeddedNullChar = '\0';

constexpr std::array<std::string_view, 4> kValidThinkingLevels = {
    "MINIMAL",
    "LOW",
    "MEDIUM",
    "HIGH",
};

struct ModelRateTier {
    double uncached_input_per_million_usd;
    double cached_input_per_million_usd;
    double output_per_million_usd;
};

[[nodiscard]] ModelRateTier select_model_rate_tier(
    std::string_view model) noexcept {
    if (detail::contains_ascii_ci(model, kGemini31ProSubstring)) {
        return {kGemini31ProInputCostPerMillionUsd,
                kGemini31ProCachedInputCostPerMillionUsd,
                kGemini31ProOutputCostPerMillionUsd};
    }
    if (detail::contains_ascii_ci(model, kNeonSubstring)) {
        return {kNeonInputCostPerMillionUsd,
                kNeonCachedInputCostPerMillionUsd,
                kNeonOutputCostPerMillionUsd};
    }
    if (detail::contains_ascii_ci(model, kHydrogenSubstring)) {
        return {kHydrogenInputCostPerMillionUsd,
                kHydrogenCachedInputCostPerMillionUsd,
                kHydrogenOutputCostPerMillionUsd};
    }
    return {kCarbonInputCostPerMillionUsd,
            kCarbonCachedInputCostPerMillionUsd,
            kCarbonOutputCostPerMillionUsd};
}

}  // namespace

std::int64_t LlmTokenUsage::total_output_tokens() const noexcept {
    const std::int64_t cand = std::max(kZeroTokens, candidate_tokens);
    const std::int64_t thought = std::max(kZeroTokens, thought_tokens);
    constexpr std::int64_t kMaxI64 = std::numeric_limits<std::int64_t>::max();
    if (cand > kMaxI64 - thought) {
        return kMaxI64;
    }
    return cand + thought;
}

LlmError::LlmError(const std::string& message, int http_status)
    : std::runtime_error(message), http_status_(http_status) {}
LlmError::~LlmError() = default;

LlmRateLimitError::LlmRateLimitError(const std::string& message, int http_status)
    : LlmError(message, http_status) {}
LlmRateLimitError::~LlmRateLimitError() = default;

LlmTransientError::LlmTransientError(const std::string& message, int http_status)
    : LlmError(message, http_status) {}
LlmTransientError::~LlmTransientError() = default;

LlmZeroThoughtError::LlmZeroThoughtError(const std::string& message)
    : LlmError(message, kHttpStatusUnset) {}
LlmZeroThoughtError::~LlmZeroThoughtError() = default;

std::int64_t compute_batch_max_output_tokens(
    std::size_t batch_lines,
    std::int64_t tokens_per_line) noexcept {
    if (batch_lines == kZeroBatchLines || tokens_per_line <= kZeroTokens) {
        return kMinOutputTokensFloor;
    }
    const auto ceiling_lines =
        static_cast<std::size_t>(kMaxOutputTokensCeiling / tokens_per_line);
    if (batch_lines > ceiling_lines) {
        return kMaxOutputTokensCeiling;
    }
    const std::int64_t scaled =
        static_cast<std::int64_t>(batch_lines) * tokens_per_line;
    return std::clamp(scaled, kMinOutputTokensFloor, kMaxOutputTokensCeiling);
}

bool is_disallowed_model(std::string_view model) noexcept {
    if (model.find(kEmbeddedNullChar) != std::string_view::npos) {
        return true;
    }
    const std::string_view trimmed = detail::trim_ascii_view(model);
    if (trimmed.empty()) {
        return false;
    }
    return detail::contains_ascii_ci(trimmed, kDisallowedArgonSubstring);
}

void ensure_supported_model(std::string_view model) {
    if (model.find(kEmbeddedNullChar) != std::string_view::npos) {
        throw LlmError("disallowed LLM model identifier: contains embedded NUL byte");
    }
    if (is_disallowed_model(model)) {
        throw LlmError(
            "disallowed LLM model '" + std::string(detail::trim_ascii_view(model)) +
            "': argon models are disabled in this repository; use gemini-4-carbon");
    }
}

std::string normalize_thinking_level(std::string_view level) {
    return detail::to_upper_ascii(detail::trim_ascii_view(level));
}

bool is_valid_thinking_level(std::string_view level) noexcept {
    if (level.find(kEmbeddedNullChar) != std::string_view::npos) {
        return false;
    }
    const std::string_view trimmed = detail::trim_ascii_view(level);
    if (trimmed.empty()) {
        return true;
    }
    return std::ranges::any_of(kValidThinkingLevels,
                               [trimmed](std::string_view valid) noexcept {
                                   return detail::equals_ascii_ci(trimmed, valid);
                               });
}

double estimate_cost_usd(const LlmTokenUsage& usage,
                         std::string_view model) noexcept {
    const std::int64_t prompt = std::max(kZeroTokens, usage.prompt_tokens);
    const std::int64_t cached = std::max(kZeroTokens, usage.cached_tokens);
    const std::int64_t uncached_input = std::max(kZeroTokens, prompt - cached);
    const double output_tokens_f =
        static_cast<double>(std::max(kZeroTokens, usage.candidate_tokens)) +
        static_cast<double>(std::max(kZeroTokens, usage.thought_tokens));

    const ModelRateTier rates = select_model_rate_tier(model);

    return (static_cast<double>(uncached_input) / kTokensPerMillion) *
               rates.uncached_input_per_million_usd +
           (static_cast<double>(cached) / kTokensPerMillion) *
               rates.cached_input_per_million_usd +
           (output_tokens_f / kTokensPerMillion) * rates.output_per_million_usd;
}

double estimate_cost_eur(const LlmTokenUsage& usage,
                         std::string_view model,
                         double usd_to_eur_rate) noexcept {
    return estimate_cost_usd(usage, model) * std::max(kZeroRate, usd_to_eur_rate);
}

}  // namespace vn::llm
