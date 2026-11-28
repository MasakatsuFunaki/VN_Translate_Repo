// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "vn/llm/gemini_wire_codec.h"

#include "core/ascii_text.h"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <utility>

namespace vn::llm {
namespace {

namespace bj = boost::json;

constexpr int kHttpOkMin = 200;
constexpr int kHttpOkMax = 299;
constexpr int kHttpServerErrorMin = 500;
constexpr int kHttpServerErrorMax = 599;
constexpr std::int64_t kZeroTokens = 0;
constexpr std::int64_t kDisableThinkingBudgetThreshold = 0;
constexpr std::int64_t kAbsoluteMinOutputTokens = 1;
constexpr std::size_t kFirstCandidateIndex = 0;
constexpr std::size_t kConfigurableHarmCategoryCount = 5;
constexpr std::size_t kBlockedFinishReasonCount = 6;

constexpr std::string_view kResourceExhaustedStatus = "RESOURCE_EXHAUSTED";
constexpr const char* kSafetyThresholdBlockNone = "BLOCK_NONE";
constexpr const char* kJsonMimeType = "application/json";

constexpr std::array<const char*, kConfigurableHarmCategoryCount>
    kConfigurableHarmCategories = {
        "HARM_CATEGORY_HARASSMENT",
        "HARM_CATEGORY_HATE_SPEECH",
        "HARM_CATEGORY_SEXUALLY_EXPLICIT",
        "HARM_CATEGORY_DANGEROUS_CONTENT",
        "HARM_CATEGORY_CIVIC_INTEGRITY",
};

constexpr std::array<std::string_view, kBlockedFinishReasonCount>
    kBlockedFinishReasons = {
        "SAFETY",
        "PROHIBITED_CONTENT",
        "BLOCKLIST",
        "RECITATION",
        "SPII",
        "IMAGE_SAFETY",
};

[[nodiscard]] static std::int64_t extract_non_negative_int64(
    const bj::object& obj,
    const char* key) noexcept {
    const auto* val = obj.if_contains(key);
    if (val == nullptr) {
        return kZeroTokens;
    }
    if (val->is_int64()) {
        return std::max(kZeroTokens, val->as_int64());
    }
    if (val->is_uint64()) {
        const std::uint64_t u = val->as_uint64();
        constexpr auto kMaxI64 =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        return static_cast<std::int64_t>(u > kMaxI64 ? kMaxI64 : u);
    }
    return kZeroTokens;
}

static void populate_usage_metadata(const bj::object& root_obj,
                                    LlmTokenUsage& usage) noexcept {
    const auto* usage_val = root_obj.if_contains("usageMetadata");
    if (usage_val == nullptr || !usage_val->is_object()) {
        return;
    }
    const auto& u = usage_val->as_object();
    usage.prompt_tokens = extract_non_negative_int64(u, "promptTokenCount");
    usage.cached_tokens =
        extract_non_negative_int64(u, "cachedContentTokenCount");
    usage.candidate_tokens =
        extract_non_negative_int64(u, "candidatesTokenCount");
    usage.thought_tokens = extract_non_negative_int64(u, "thoughtsTokenCount");
}

[[nodiscard]] static std::string extract_prompt_block_reason(
    const bj::object& root_obj) {
    const auto* feedback = root_obj.if_contains("promptFeedback");
    if (feedback == nullptr || !feedback->is_object()) {
        return {};
    }
    const auto* reason = feedback->as_object().if_contains("blockReason");
    if (reason == nullptr || !reason->is_string()) {
        return {};
    }
    const std::string_view sv(reason->as_string().data(),
                              reason->as_string().size());
    if (detail::trim_ascii_view(sv).empty()) {
        return {};
    }
    return std::string(sv);
}

[[nodiscard]] static bool is_blocked_finish_reason(
    std::string_view reason) noexcept {
    return std::ranges::any_of(kBlockedFinishReasons,
                               [reason](std::string_view candidate) noexcept {
                                   return candidate == reason;
                               });
}

[[nodiscard]] static bj::object build_thinking_config(
    const LlmRequest& request,
    std::string_view normalized_level) {
    if (!normalized_level.empty()) {
        return bj::object{{"thinkingLevel", normalized_level}};
    }
    return bj::object{{"thinkingBudget", request.thinking_budget}};
}

}  // namespace

std::string build_gemini_request_json(const LlmRequest& request) {
    ensure_supported_model(request.model);
    if (!is_valid_thinking_level(request.thinking_level)) {
        throw LlmError("invalid Gemini thinkingLevel: '" +
                       request.thinking_level + "'");
    }

    bj::object root;
    if (!request.system_prompt.empty()) {
        root["systemInstruction"] = bj::object{
            {"parts", bj::array{bj::object{{"text", request.system_prompt}}}},
        };
    }

    root["contents"] = bj::array{
        bj::object{
            {"role", "user"},
            {"parts", bj::array{bj::object{{"text", request.user_prompt}}}},
        },
    };

    if (request.safety_block_none) {
        bj::array safety_settings;
        safety_settings.reserve(kConfigurableHarmCategories.size());
        for (const char* category : kConfigurableHarmCategories) {
            safety_settings.emplace_back(bj::object{
                {"category", category},
                {"threshold", kSafetyThresholdBlockNone},
            });
        }
        root["safetySettings"] = std::move(safety_settings);
    }

    bj::object gen_cfg;
    const std::int64_t lower_bound =
        request.enforce_min_output_floor ? kMinOutputTokensFloor
                                         : kAbsoluteMinOutputTokens;
    const std::int64_t clamped_max = std::clamp<std::int64_t>(
        request.max_output_tokens, lower_bound, kMaxOutputTokensCeiling);
    gen_cfg["maxOutputTokens"] = clamped_max;

    const std::string normalized_level =
        normalize_thinking_level(request.thinking_level);
    if (!normalized_level.empty() ||
        request.thinking_budget >= kDisableThinkingBudgetThreshold) {
        gen_cfg["thinkingConfig"] =
            build_thinking_config(request, normalized_level);
    }

    if (request.json_mode) {
        gen_cfg["responseMimeType"] = kJsonMimeType;
    }

    root["generationConfig"] = std::move(gen_cfg);
    return bj::serialize(root);
}

LlmResponse parse_gemini_response_json(std::string_view raw_json,
                                       int http_status) {
    boost::system::error_code parse_ec;
    bj::value parsed;
    if (!raw_json.empty()) {
        parsed = bj::parse(bj::string_view(raw_json.data(), raw_json.size()),
                           parse_ec);
    }

    std::string api_err_status;
    std::string api_err_message;
    if (!parse_ec && parsed.is_object()) {
        if (const auto* err_val = parsed.as_object().if_contains("error");
            err_val != nullptr && err_val->is_object()) {
            const auto& err_obj = err_val->as_object();
            if (const auto* st = err_obj.if_contains("status");
                st != nullptr && st->is_string()) {
                api_err_status = std::string(st->as_string());
            }
            if (const auto* msg = err_obj.if_contains("message");
                msg != nullptr && msg->is_string()) {
                api_err_message = std::string(msg->as_string());
            }
        }
    }

    if (http_status == kHttpStatusTooManyRequests ||
        api_err_status == kResourceExhaustedStatus) {
        const std::string detail =
            !api_err_message.empty()
                ? api_err_message
                : ("rate limited (HTTP " + std::to_string(http_status) + ")");
        throw LlmRateLimitError(detail, http_status);
    }

    if (http_status >= kHttpServerErrorMin && http_status <= kHttpServerErrorMax) {
        const std::string detail =
            !api_err_message.empty()
                ? ("HTTP " + std::to_string(http_status) + ": " + api_err_message)
                : ("HTTP " + std::to_string(http_status));
        throw LlmTransientError(detail, http_status);
    }

    if (http_status < kHttpOkMin || http_status > kHttpOkMax) {
        const std::string detail =
            !api_err_message.empty()
                ? ("HTTP " + std::to_string(http_status) + ": " + api_err_message)
                : ("HTTP " + std::to_string(http_status));
        throw LlmError(detail, http_status);
    }

    if (raw_json.empty() || parse_ec || !parsed.is_object()) {
        throw LlmError(
            parse_ec ? ("malformed response JSON: " + parse_ec.message())
                     : "response body is empty or not a JSON object",
            http_status);
    }

    const auto& root_obj = parsed.as_object();
    LlmResponse out;
    populate_usage_metadata(root_obj, out.usage);

    const auto* cands = root_obj.if_contains("candidates");
    if (cands == nullptr || !cands->is_array() || cands->as_array().empty() ||
        !cands->as_array()[kFirstCandidateIndex].is_object()) {
        std::string block_reason = extract_prompt_block_reason(root_obj);
        if (!block_reason.empty()) {
            out.finish_reason = std::move(block_reason);
            out.content_blocked = true;
            return out;
        }
        throw LlmError("response JSON missing non-empty candidates array",
                       http_status);
    }

    const auto& cand0 = cands->as_array()[kFirstCandidateIndex].as_object();
    if (const auto* content = cand0.if_contains("content");
        content != nullptr && content->is_object()) {
        if (const auto* parts = content->as_object().if_contains("parts");
            parts != nullptr && parts->is_array()) {
            for (const auto& part_val : parts->as_array()) {
                if (!part_val.is_object()) {
                    continue;
                }
                const auto& part_obj = part_val.as_object();
                if (const auto* thought = part_obj.if_contains("thought");
                    thought != nullptr && thought->is_bool() &&
                    thought->as_bool()) {
                    continue;
                }
                if (const auto* txt = part_obj.if_contains("text");
                    txt != nullptr && txt->is_string()) {
                    out.text.append(txt->as_string().data(),
                                    txt->as_string().size());
                }
            }
        }
    }

    if (const auto* fr = cand0.if_contains("finishReason");
        fr != nullptr && fr->is_string()) {
        out.finish_reason = std::string(fr->as_string());
    }

    if (is_blocked_finish_reason(out.finish_reason)) {
        out.content_blocked = true;
        return out;
    }

    if (detail::trim_ascii_view(out.text).empty()) {
        std::string block_reason = extract_prompt_block_reason(root_obj);
        if (!block_reason.empty()) {
            out.finish_reason = std::move(block_reason);
            out.content_blocked = true;
            return out;
        }
        throw LlmError("response candidate contains no non-thought text output",
                       http_status);
    }

    return out;
}

}  // namespace vn::llm
