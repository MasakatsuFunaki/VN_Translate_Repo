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

// Serializes `request` into the Gemini `generateContent` JSON request body:
// - Includes `systemInstruction.parts[0].text` iff `!request.system_prompt.empty()`.
// - Includes `contents[0]{role:"user", parts:[{text: request.user_prompt}]}`.
// - Includes `safetySettings` setting `"threshold": "BLOCK_NONE"` across
//   `HARM_CATEGORY_HARASSMENT`, `HARM_CATEGORY_HATE_SPEECH`,
//   `HARM_CATEGORY_SEXUALLY_EXPLICIT`, `HARM_CATEGORY_DANGEROUS_CONTENT`, and
//   `HARM_CATEGORY_CIVIC_INTEGRITY`.
// - Includes `generationConfig.maxOutputTokens = request.max_output_tokens`,
//   `generationConfig.thinkingConfig.thinkingBudget = request.thinking_budget`,
//   and `generationConfig.responseMimeType = "application/json"` when
//   `request.json_mode == true`.
std::string build_argon_request_json(const LlmRequest& request);

// Parses a `generateContent` HTTP response (`http_status` + `raw_json`):
// - Returns `ErrorCode::RateLimited` if `http_status == 429` or JSON
//   `error.status == "RESOURCE_EXHAUSTED"`.
// - Returns `ErrorCode::HttpError` (preserving `http_status`) on any other HTTP
//   status outside `[200, 299]`.
// - On HTTP `[200, 299]`, extracts `usageMetadata` (`promptTokenCount`,
//   `candidatesTokenCount`, `thoughtsTokenCount`, handling signed and unsigned
//   JSON integers) up front:
//   - When `candidates` is missing or empty and `promptFeedback.blockReason`
//     is a non-empty string, returns `Result<LlmResponse>::success` with
//     `content_blocked = true`, `finish_reason = <blockReason>`, `text = ""`,
//     and extracted `usageMetadata` token counts.
//   - When `candidates[0]` yields empty non-thought `text` and `finishReason`
//     is one of `"SAFETY"`, `"PROHIBITED_CONTENT"`, `"BLOCKLIST"`,
//     `"RECITATION"`, `"SPII"`, or `"IMAGE_SAFETY"`, returns
//     `Result<LlmResponse>::success` with `content_blocked = true`, `text = ""`,
//     `finish_reason`, and extracted `usageMetadata` token counts.
// - Returns `ErrorCode::CorruptJson` on malformed JSON, missing/empty
//   `candidates` without a non-empty `promptFeedback.blockReason`, or empty
//   non-thought text output without a safety/policy refusal `finishReason`.
// - Concatenates `candidates[0].content.parts[*].text` skipping parts with
//   `"thought": true`.
Result<LlmResponse> parse_argon_response_json(std::string_view raw_json,
                                              int http_status = 200);

// Pure, side-effect-free `.env` parser: extracts the value assigned to
// `key_name` from `dotenv_contents` (supporting `KEY=VAL`, `export KEY=VAL`,
// `#` full-line and unquoted inline comments, surrounding whitespace, and
// single- or double-quoted values). Never mutates process environment variables.
std::string parse_dotenv_value(std::string_view dotenv_contents,
                               std::string_view key_name);

}  // namespace spot_checker
