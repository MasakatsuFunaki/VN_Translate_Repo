// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Private C++20 concepts-constrained exponential-backoff retry executor.
// Blends zero-overhead static polymorphism on the inner retry policy loop with
// runtime `ILlmClient` Strategy polymorphism at the component boundary.
#pragma once

#include "vn/llm/llm_client.h"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <string>
#include <utility>

namespace vn::llm::detail {

inline constexpr int kMinRetryCount = 0;
inline constexpr int kMinBackoffMultiplier = 1;
inline constexpr int kExponentialBackoffFactor = 2;
inline constexpr std::chrono::milliseconds kZeroBackoff{0};

template <typename F>
concept RetryableAttemptCallable = std::invocable<F&>;

struct ExponentialBackoffPolicy {
    int max_retries = kDefaultMaxRetries;
    std::chrono::milliseconds initial_backoff = kDefaultInitialBackoff;
    int multiplier = kExponentialBackoffFactor;
};

template <RetryableAttemptCallable AttemptFn,
          BackoffSleepCallable SleepFn,
          DiagnosticLogCallable LogFn>
[[nodiscard]] decltype(auto) execute_with_exponential_backoff(
    const ExponentialBackoffPolicy& policy,
    AttemptFn&& attempt_fn,
    SleepFn&& sleep_fn,
    LogFn&& log_fn) {
    const int effective_max_retries = std::max(kMinRetryCount, policy.max_retries);
    const int effective_multiplier =
        std::max(kMinBackoffMultiplier, policy.multiplier);
    std::chrono::milliseconds backoff =
        std::max(kZeroBackoff, policy.initial_backoff);

    for (int attempt = 0;; ++attempt) {
        try {
            return attempt_fn();
        } catch (const LlmRateLimitError& ex) {
            if (attempt >= effective_max_retries) {
                throw;
            }
            log_fn("Rate limited (" + std::string(ex.what()) +
                   "), retrying in " + std::to_string(backoff.count()) +
                   "ms...");
            sleep_fn(backoff);
            backoff *= effective_multiplier;
        } catch (const LlmTransientError& ex) {
            if (attempt >= effective_max_retries) {
                throw;
            }
            log_fn("Transient LLM transport/server error (" +
                   std::string(ex.what()) + "), retrying in " +
                   std::to_string(backoff.count()) + "ms...");
            sleep_fn(backoff);
            backoff *= effective_multiplier;
        }
    }
}

}  // namespace vn::llm::detail
