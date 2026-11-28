// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "common/status.h"
#include "ports/llm_transport.h"

#include <string>
#include <string_view>

namespace spot_checker {

inline constexpr int kHttpStatusOk = 200;

// Thin adapter around `vn::llm::build_gemini_request_json` serializing `request`
// into the Gemini `generateContent` JSON request body (`safety_block_none = true`,
// `json_mode = request.json_mode`, `enforce_min_output_floor = false` to preserve
// `[1, 65536]` `maxOutputTokens` clamping, and `thinkingLevel` defaulting to
// `"MEDIUM"` with fallback to `thinkingBudget` only when `thinking_level` is empty).
// Throws `vn::llm::LlmError` if a non-empty `request.thinking_level` is not one
// of `"MINIMAL"`, `"LOW"`, `"MEDIUM"`, or `"HIGH"`.
std::string build_argon_request_json(const LlmRequest& request);

// Thin `Result<LlmResponse>` (`StatusOr`) adapter around
// `vn::llm::parse_gemini_response_json`:
// - Maps `vn::llm::LlmRateLimitError` (HTTP 429 or `RESOURCE_EXHAUSTED`) to
//   `ErrorCode::RateLimited`.
// - Maps `vn::llm::LlmTransientError` (HTTP 5xx) and non-2xx `vn::llm::LlmError`
//   to `ErrorCode::HttpError` (preserving `http_status`).
// - Maps 2xx malformed/empty-output `vn::llm::LlmError` to `ErrorCode::CorruptJson`.
// - Preserves `content_blocked == true` safety/policy refusals and token usage.
Result<LlmResponse> parse_argon_response_json(std::string_view raw_json,
                                              int http_status = kHttpStatusOk);

// Delegates directly to `vn::llm::parse_dotenv_value` without mutating process
// environment variables.
std::string parse_dotenv_value(std::string_view dotenv_contents,
                               std::string_view key_name);

}  // namespace spot_checker
