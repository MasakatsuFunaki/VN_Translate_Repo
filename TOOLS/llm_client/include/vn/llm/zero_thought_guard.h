// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Stateful top-level-only zero-thought circuit breaker guarding supported
// Gemini 4 reasoning runs (`carbon`, `neon`, `hydrogen`) configured with
// `thinkingLevel` `"MEDIUM"` or `"HIGH"` against silent non-thinking mode.
// Standard STL only -- zero Boost, OpenSSL, or OS headers.
#pragma once

#include "vn/llm/llm_types.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vn::llm {

inline constexpr std::size_t kMinInitialBatchSliceForThoughtCheck = 4;
inline constexpr std::size_t kMinSubsequentBatchSliceForThoughtCheck = 16;
inline constexpr std::size_t kConsecutiveZeroThoughtBatchLimit = 2;

// Returns true iff `!is_disallowed_model(model)`, `model` names a supported
// Gemini 4 reasoning family (`carbon`, `neon`, or `hydrogen`, case-insensitive),
// and `thinking_level` normalizes to `"MEDIUM"` or `"HIGH"`.
[[nodiscard]] bool is_guarded_thinking_mode(
    std::string_view model,
    std::string_view thinking_level) noexcept;

// Thread-compatible: concurrent const access is safe; mutating calls require
// external synchronization.
class ZeroThoughtCircuitBreaker {
public:
    ZeroThoughtCircuitBreaker() noexcept = default;

    // Evaluates a completed call against the active-thinking invariant.
    // Active ONLY when `is_guarded_thinking_mode(model, thinking_level)`,
    // `is_top_level == true`, and `!content_blocked`:
    //   - On Batch 0 (`top_level_batches_completed_ == 0`): throws
    //     `LlmZeroThoughtError` immediately if `slice_size >= 4` and
    //     `thought_tokens <= 0`.
    //   - On subsequent batches (`top_level_batches_completed_ > 0`): throws
    //     `LlmZeroThoughtError` when 2 consecutive unblocked top-level batches
    //     with `slice_size >= 16` return `thought_tokens <= 0`.
    //   - Never trips or mutates state on non-top-level bisected sub-slices
    //     (`!is_top_level`) or safety-blocked responses (`content_blocked`).
    void check_response(std::string_view model,
                        std::string_view thinking_level,
                        bool is_top_level,
                        std::size_t slice_size,
                        bool content_blocked,
                        std::int64_t thought_tokens);

    void record_top_level_batch_completed() noexcept;

    void reset() noexcept;

    [[nodiscard]] std::size_t top_level_batches_completed() const noexcept {
        return top_level_batches_completed_;
    }

    [[nodiscard]] std::size_t consecutive_top_level_zero_thoughts() const noexcept {
        return consecutive_top_level_zero_thoughts_;
    }

private:
    std::size_t top_level_batches_completed_ = 0;
    std::size_t consecutive_top_level_zero_thoughts_ = 0;
};

}  // namespace vn::llm
