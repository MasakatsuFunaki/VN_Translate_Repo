// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "client/argon_wire_codec.h"

#include "vn/llm/gemini_wire_codec.h"
#include "vn/llm/llm_client.h"

#include <cstdint>
#include <utility>

namespace spot_checker {
namespace {

constexpr int kHttpOkMin = 200;
constexpr int kHttpOkMax = 299;

}  // namespace

std::string build_argon_request_json(const LlmRequest& request) {
    vn::llm::LlmRequest vn_req;
    vn_req.system_prompt = request.system_prompt;
    vn_req.user_prompt = request.user_prompt;
    vn_req.thinking_level = request.thinking_level;
    vn_req.thinking_budget = static_cast<std::int64_t>(request.thinking_budget);
    vn_req.max_output_tokens =
        static_cast<std::int64_t>(request.max_output_tokens);
    vn_req.enforce_min_output_floor = false;
    vn_req.safety_block_none = true;
    vn_req.json_mode = request.json_mode;
    return vn::llm::build_gemini_request_json(vn_req);
}

Result<LlmResponse> parse_argon_response_json(std::string_view raw_json,
                                              int http_status) {
    try {
        vn::llm::LlmResponse parsed =
            vn::llm::parse_gemini_response_json(raw_json, http_status);
        LlmResponse out;
        if (!parsed.content_blocked) {
            out.text = std::move(parsed.text);
        }
        out.finish_reason = std::move(parsed.finish_reason);
        out.prompt_tokens = parsed.usage.prompt_tokens;
        out.output_tokens = parsed.usage.candidate_tokens;
        out.thought_tokens = parsed.usage.thought_tokens;
        out.content_blocked = parsed.content_blocked;
        return Result<LlmResponse>::success(std::move(out));
    } catch (const vn::llm::LlmRateLimitError& ex) {
        return Result<LlmResponse>::failure(
            ErrorCode::RateLimited, ex.what(), ex.http_status());
    } catch (const vn::llm::LlmTransientError& ex) {
        return Result<LlmResponse>::failure(
            ErrorCode::HttpError, ex.what(), ex.http_status());
    } catch (const vn::llm::LlmError& ex) {
        if (http_status < kHttpOkMin || http_status > kHttpOkMax) {
            return Result<LlmResponse>::failure(
                ErrorCode::HttpError, ex.what(), ex.http_status());
        }
        return Result<LlmResponse>::failure(
            ErrorCode::CorruptJson, ex.what(), ex.http_status());
    }
}

std::string parse_dotenv_value(std::string_view dotenv_contents,
                               std::string_view key_name) {
    return vn::llm::parse_dotenv_value(dotenv_contents, key_name);
}

}  // namespace spot_checker
