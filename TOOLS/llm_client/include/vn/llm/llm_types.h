// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Canonical model/thinking/token/timeout defaults, value DTOs, exception
// hierarchy, and pure calculation helpers for `vn::llm_client`.
// Standard STL only -- zero Boost, OpenSSL, or OS headers.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vn::llm {

inline constexpr std::string_view kDefaultModel = "gemini-4-carbon";
inline constexpr std::string_view kDefaultThinkingLevel = "MEDIUM";
inline constexpr int kDefaultBatchSize = 40;
inline constexpr std::int64_t kDefaultThinkingBudget = 2048;
inline constexpr std::int64_t kMinOutputTokensFloor = 16384;
inline constexpr std::int64_t kMaxOutputTokensCeiling = 65536;
inline constexpr int kDefaultConnectTimeoutSeconds = 60;
inline constexpr int kDefaultReadTimeoutSeconds = 600;
inline constexpr int kDefaultMaxRetries = 4;
inline constexpr int kAutoZeroThoughtRetries = -1;
inline constexpr int kDefaultZeroThoughtRetries = 3;
inline constexpr std::chrono::milliseconds kDefaultInitialBackoff{2000};
inline constexpr std::int64_t kDefaultTokensPerBatchLine = 300;
inline constexpr double kDefaultUsdToEurRate = 0.95;

inline constexpr int kHttpStatusUnset = 0;
inline constexpr int kHttpStatusOk = 200;
inline constexpr int kHttpStatusTooManyRequests = 429;

enum class LlmProvider {
    Gemini,
};

struct LlmRequest {
    // Optional per-request model override; when empty, falls back to
    // `LlmClientOptions::default_model` (`kDefaultModel`).
    std::string model;
    std::string system_prompt;
    std::string user_prompt;
    std::int64_t max_output_tokens = kMinOutputTokensFloor;
    // Discrete Gemini thinking level ("MINIMAL", "LOW", "MEDIUM", "HIGH").
    // Defaults to "MEDIUM"; explicitly clear ("") to fall back to `thinking_budget`.
    std::string thinking_level{kDefaultThinkingLevel};
    std::int64_t thinking_budget = kDefaultThinkingBudget;
    // When true (default), clamps `max_output_tokens` to `[16384, 65536]` so
    // small batches never starve HIGH reasoning; when false, clamps to `[1, 65536]`.
    bool enforce_min_output_floor = true;
    bool safety_block_none = true;
    // Optional structured JSON response mode (`responseMimeType: "application/json"`).
    bool json_mode = false;
    // When true (default), guarded Gemini 4 thinking calls (`MEDIUM`/`HIGH`)
    // that return an unblocked 200 OK response with `thought_tokens <= 0`
    // retry up to `LlmClientOptions::zero_thought_max_retries` times at the
    // requested `thinkingLevel` with an internal-reasoning nudge. Set false on
    // tiny bisected sub-slices (`< 4` lines) where `0` thoughts is acceptable.
    bool retry_on_zero_thoughts = true;
};

struct LlmTokenUsage {
    std::int64_t prompt_tokens = 0;
    std::int64_t cached_tokens = 0;
    std::int64_t candidate_tokens = 0;
    std::int64_t thought_tokens = 0;

    [[nodiscard]] std::int64_t total_output_tokens() const noexcept;
};

struct LlmResponse {
    std::string text;
    std::string finish_reason;
    bool content_blocked = false;
    LlmTokenUsage usage;
};

// Base exception for non-retryable or retry-exhausted LLM failures.
class LlmError : public std::runtime_error {
public:
    explicit LlmError(const std::string& message,
                      int http_status = kHttpStatusUnset);
    ~LlmError() override;

    LlmError(const LlmError&) = default;
    LlmError(LlmError&&) noexcept = default;
    LlmError& operator=(const LlmError&) = default;
    LlmError& operator=(LlmError&&) noexcept = default;

    [[nodiscard]] int http_status() const noexcept { return http_status_; }

private:
    int http_status_;
};

// Retryable rate-limit or quota-throttling error (HTTP 429 / RESOURCE_EXHAUSTED).
class LlmRateLimitError : public LlmError {
public:
    explicit LlmRateLimitError(const std::string& message,
                               int http_status = kHttpStatusTooManyRequests);
    ~LlmRateLimitError() override;

    LlmRateLimitError(const LlmRateLimitError&) = default;
    LlmRateLimitError(LlmRateLimitError&&) noexcept = default;
    LlmRateLimitError& operator=(const LlmRateLimitError&) = default;
    LlmRateLimitError& operator=(LlmRateLimitError&&) noexcept = default;
};

// Retryable transient network/TLS or HTTP 5xx server failure.
class LlmTransientError : public LlmError {
public:
    explicit LlmTransientError(const std::string& message,
                               int http_status = kHttpStatusUnset);
    ~LlmTransientError() override;

    LlmTransientError(const LlmTransientError&) = default;
    LlmTransientError(LlmTransientError&&) noexcept = default;
    LlmTransientError& operator=(const LlmTransientError&) = default;
    LlmTransientError& operator=(LlmTransientError&&) noexcept = default;
};

// Raised by `ZeroThoughtCircuitBreaker` when a guarded Gemini 4 model configured
// with `thinkingLevel` `"MEDIUM"` or `"HIGH"` returns zero thought tokens on
// qualifying top-level batches.
class LlmZeroThoughtError : public LlmError {
public:
    explicit LlmZeroThoughtError(const std::string& message);
    ~LlmZeroThoughtError() override;

    LlmZeroThoughtError(const LlmZeroThoughtError&) = default;
    LlmZeroThoughtError(LlmZeroThoughtError&&) noexcept = default;
    LlmZeroThoughtError& operator=(const LlmZeroThoughtError&) = default;
    LlmZeroThoughtError& operator=(LlmZeroThoughtError&&) noexcept = default;
};

// Overflow-safely computes `batch_lines * tokens_per_line` clamped into
// `[kMinOutputTokensFloor, kMaxOutputTokensCeiling]` (`[16384, 65536]`).
[[nodiscard]] std::int64_t compute_batch_max_output_tokens(
    std::size_t batch_lines,
    std::int64_t tokens_per_line = kDefaultTokensPerBatchLine) noexcept;

// Returns true iff `model` violates repository model policy:
// rejects embedded NUL bytes (`'\0'`) and any model identifier containing
// `"argon"` (ASCII case-insensitive, e.g. `"gemini-4-argon"`).
// Empty/whitespace-only strings return false (callers resolve to `kDefaultModel`).
[[nodiscard]] bool is_disallowed_model(std::string_view model) noexcept;

// Throws `LlmError` if `is_disallowed_model(model)` is true.
void ensure_supported_model(std::string_view model);

// Trims surrounding ASCII whitespace and uppercases ASCII letters.
[[nodiscard]] std::string normalize_thinking_level(std::string_view level);

// Returns true iff `normalize_thinking_level(level)` is `""` (explicit fallback
// to `thinking_budget`) or one of `"MINIMAL"`, `"LOW"`, `"MEDIUM"`, `"HIGH"`.
[[nodiscard]] bool is_valid_thinking_level(std::string_view level) noexcept;

// Estimates spend in USD / EUR:
// - `gemini-4-carbon` (and default): $2.00/1M uncached input, $0.20/1M cached
//   input, $10.00/1M output+thought.
// - `gemini-4-neon`: $3.00/1M uncached input, $0.30/1M cached input,
//   $15.00/1M output+thought.
// - `gemini-4-hydrogen`: $1.50/1M uncached input, $0.15/1M cached input,
//   $7.50/1M output+thought.
// - `gemini-3.1-pro*`: $2.00/1M uncached input, $0.20/1M cached input,
//   $12.00/1M output+thought.
[[nodiscard]] double estimate_cost_usd(
    const LlmTokenUsage& usage,
    std::string_view model = kDefaultModel) noexcept;

[[nodiscard]] double estimate_cost_eur(
    const LlmTokenUsage& usage,
    std::string_view model = kDefaultModel,
    double usd_to_eur_rate = kDefaultUsdToEurRate) noexcept;

}  // namespace vn::llm
