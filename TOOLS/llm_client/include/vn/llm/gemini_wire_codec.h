// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pure Gemini v1beta `generateContent` JSON wire serializer and parser.
// Standard STL only -- zero Boost, OpenSSL, or OS headers.
#pragma once

#include "vn/llm/llm_types.h"

#include <string>
#include <string_view>

namespace vn::llm {

// Serializes `request` into a Gemini `generateContent` JSON request body:
// - Throws `LlmError` if `is_disallowed_model(request.model)` is true (for
//   example any `"argon"` model identifier or embedded NUL byte).
// - Includes `systemInstruction.parts[0].text` iff `!request.system_prompt.empty()`.
// - Includes `contents[0]` with `role: "user"` and `parts[0].text = request.user_prompt`.
// - Includes `safetySettings` setting `"threshold": "BLOCK_NONE"` across all 5
//   configurable harm categories (`HARM_CATEGORY_HARASSMENT`,
//   `HARM_CATEGORY_HATE_SPEECH`, `HARM_CATEGORY_SEXUALLY_EXPLICIT`,
//   `HARM_CATEGORY_DANGEROUS_CONTENT`, `HARM_CATEGORY_CIVIC_INTEGRITY`) when
//   `request.safety_block_none == true`.
// - Clamps `generationConfig.maxOutputTokens` to `[16384, 65536]` when
//   `request.enforce_min_output_floor == true`, or `[1, 65536]` when `false`.
// - Emits `generationConfig.thinkingConfig.thinkingLevel` (normalized uppercase,
//   defaulting to `"MEDIUM"`, with NO `thinkingBudget` field) when
//   `request.thinking_level` is non-empty after ASCII trim; falls back to
//   `thinkingBudget` only when `thinking_level` is empty and
//   `request.thinking_budget >= 0`. Throws `LlmError` if a non-empty
//   `thinking_level` is not a valid Gemini thinking level.
// - Emits `generationConfig.responseMimeType = "application/json"` when
//   `request.json_mode == true`.
[[nodiscard]] std::string build_gemini_request_json(const LlmRequest& request);

// Parses a Gemini `generateContent` HTTP response (`http_status` + `raw_json`):
// - Throws `LlmRateLimitError` on HTTP 429 or JSON `error.status == "RESOURCE_EXHAUSTED"`.
// - Throws `LlmTransientError` on HTTP 5xx (`[500, 599]`).
// - Throws `LlmError` on any other non-2xx HTTP status or malformed JSON.
// - On HTTP 2xx (`[200, 299]`):
//   - Extracts `usageMetadata` (`promptTokenCount`, `cachedContentTokenCount`,
//     `candidatesTokenCount`, `thoughtsTokenCount`).
//   - Concatenates `candidates[0].content.parts[*].text`, stripping parts with
//     `"thought": true`.
//   - Sets `content_blocked = true` (without throwing) when
//     `promptFeedback.blockReason` is present with missing/empty candidates or
//     empty non-thought text, OR when `finishReason` is one of `SAFETY`,
//     `PROHIBITED_CONTENT`, `BLOCKLIST`, `RECITATION`, `SPII`, `IMAGE_SAFETY`.
//   - Throws `LlmError` when candidates are missing/empty or yield empty
//     non-thought text without any safety/policy block reason.
[[nodiscard]] LlmResponse parse_gemini_response_json(
    std::string_view raw_json,
    int http_status = kHttpStatusOk);

}  // namespace vn::llm
