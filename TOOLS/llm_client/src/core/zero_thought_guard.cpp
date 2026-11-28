// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "vn/llm/zero_thought_guard.h"

#include "core/ascii_text.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <string>

namespace vn::llm {
namespace {

constexpr std::size_t kGuardedModelFamilyCount = 3;
constexpr std::array<std::string_view, kGuardedModelFamilyCount>
    kGuardedGemini4ModelTags = {
        "carbon",
        "neon",
        "hydrogen",
};

constexpr std::size_t kGuardedThinkingLevelCount = 2;
constexpr std::array<std::string_view, kGuardedThinkingLevelCount>
    kGuardedThinkingLevels = {
        "MEDIUM",
        "HIGH",
};

constexpr std::size_t kInitialTopLevelBatchCount = 0;
constexpr std::size_t kClearedZeroThoughtStreak = 0;
constexpr std::int64_t kZeroThoughtThreshold = 0;

}  // namespace

bool is_guarded_thinking_mode(std::string_view model,
                              std::string_view thinking_level) noexcept {
    if (is_disallowed_model(model)) {
        return false;
    }
    const std::string_view trimmed_level =
        detail::trim_ascii_view(thinking_level);
    const bool level_guarded = std::ranges::any_of(
        kGuardedThinkingLevels, [trimmed_level](std::string_view guarded) noexcept {
            return detail::equals_ascii_ci(trimmed_level, guarded);
        });
    if (!level_guarded) {
        return false;
    }
    return std::ranges::any_of(
        kGuardedGemini4ModelTags, [model](std::string_view tag) noexcept {
            return detail::contains_ascii_ci(model, tag);
        });
}

void ZeroThoughtCircuitBreaker::check_response(std::string_view model,
                                               std::string_view thinking_level,
                                               bool is_top_level,
                                               std::size_t slice_size,
                                               bool content_blocked,
                                               std::int64_t thought_tokens) {
    if (!is_top_level || content_blocked) {
        return;
    }
    if (!is_guarded_thinking_mode(model, thinking_level)) {
        return;
    }

    const std::string normalized_level = normalize_thinking_level(thinking_level);
    if (top_level_batches_completed_ == kInitialTopLevelBatchCount) {
        if (slice_size >= kMinInitialBatchSliceForThoughtCheck &&
            thought_tokens <= kZeroThoughtThreshold) {
            throw LlmZeroThoughtError(
                "Gemini 4 model '" + std::string(model) +
                "' returned 0 thought tokens on initial top-level batch "
                "(slice_size=" + std::to_string(slice_size) +
                ") despite thinkingLevel=" + normalized_level);
        }
        if (thought_tokens > kZeroThoughtThreshold) {
            consecutive_top_level_zero_thoughts_ = kClearedZeroThoughtStreak;
        }
        return;
    }

    if (slice_size >= kMinSubsequentBatchSliceForThoughtCheck) {
        if (thought_tokens <= kZeroThoughtThreshold) {
            ++consecutive_top_level_zero_thoughts_;
            if (consecutive_top_level_zero_thoughts_ >=
                kConsecutiveZeroThoughtBatchLimit) {
                throw LlmZeroThoughtError(
                    "Gemini 4 model '" + std::string(model) + "' returned 0 "
                    "thought tokens on " +
                    std::to_string(consecutive_top_level_zero_thoughts_) +
                    " consecutive top-level batches (slice_size=" +
                    std::to_string(slice_size) +
                    ") despite thinkingLevel=" + normalized_level);
            }
            return;
        }
        consecutive_top_level_zero_thoughts_ = kClearedZeroThoughtStreak;
        return;
    }

    if (thought_tokens > kZeroThoughtThreshold) {
        consecutive_top_level_zero_thoughts_ = kClearedZeroThoughtStreak;
    }
}

void ZeroThoughtCircuitBreaker::record_top_level_batch_completed() noexcept {
    ++top_level_batches_completed_;
}

void ZeroThoughtCircuitBreaker::reset() noexcept {
    top_level_batches_completed_ = kInitialTopLevelBatchCount;
    consecutive_top_level_zero_thoughts_ = kClearedZeroThoughtStreak;
}

}  // namespace vn::llm
