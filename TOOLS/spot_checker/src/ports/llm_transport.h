// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "common/status.h"
#include "domain/spot_check_types.h"

#include <cstdint>
#include <string>

namespace spot_checker {

inline constexpr int kDefaultThinkingBudgetTokens =
    static_cast<int>(vn::llm::kDefaultThinkingBudget);
inline constexpr int kDefaultMaxOutputTokens =
    static_cast<int>(vn::llm::kMinOutputTokensFloor);

struct LlmRequest {
    std::string system_prompt;
    std::string user_prompt;
    std::string thinking_level{kDefaultThinkingLevel};
    int thinking_budget = kDefaultThinkingBudgetTokens;
    int max_output_tokens = kDefaultMaxOutputTokens;
    bool json_mode = true;
};

struct LlmResponse {
    std::string text;
    std::string finish_reason;
    std::int64_t prompt_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t thought_tokens = 0;
    bool content_blocked = false;
};

// Pure protocol interface for synchronous generative model invocations.
// Returns `Result<LlmResponse>::success` with `content_blocked == true` when
// the model refuses a prompt or candidate on safety/policy grounds,
// `ErrorCode::InvalidArgument` when the configured model or `thinking_level`
// violates policy (e.g. any disallowed `"argon"` model identifier),
// `ErrorCode::RateLimited` on HTTP 429 / RESOURCE_EXHAUSTED,
// `ErrorCode::MissingApiKey` when credentials are absent, and
// `ErrorCode::HttpError` / `ErrorCode::NetworkError` / `ErrorCode::CorruptJson`
// on transport or wire failures.
class ILlmTransport {
public:
    virtual ~ILlmTransport();

    ILlmTransport(const ILlmTransport&) = delete;
    ILlmTransport& operator=(const ILlmTransport&) = delete;

    virtual Result<LlmResponse> generate(const LlmRequest& request) = 0;

protected:
    ILlmTransport() = default;
    ILlmTransport(ILlmTransport&&) noexcept = default;
    ILlmTransport& operator=(ILlmTransport&&) noexcept = default;
};

}  // namespace spot_checker
