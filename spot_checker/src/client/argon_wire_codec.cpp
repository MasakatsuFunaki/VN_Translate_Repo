// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "client/argon_wire_codec.h"

#include "common/utf8_text.h"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace spot_checker {
namespace {

namespace bj = boost::json;

constexpr int kHttpOkMin = 200;
constexpr int kHttpOkMax = 299;
constexpr int kHttpTooManyRequests = 429;
constexpr std::size_t kFirstCandidateIndex = 0;
constexpr std::string_view kResourceExhaustedStatus = "RESOURCE_EXHAUSTED";
constexpr const char* kSafetyThresholdBlockNone = "BLOCK_NONE";
constexpr std::size_t kConfigurableHarmCategoryCount = 5;
constexpr std::array<const char*, kConfigurableHarmCategoryCount>
    kConfigurableHarmCategories = {
        "HARM_CATEGORY_HARASSMENT",
        "HARM_CATEGORY_HATE_SPEECH",
        "HARM_CATEGORY_SEXUALLY_EXPLICIT",
        "HARM_CATEGORY_DANGEROUS_CONTENT",
        "HARM_CATEGORY_CIVIC_INTEGRITY",
};
constexpr std::size_t kBlockedFinishReasonCount = 6;
constexpr std::array<std::string_view, kBlockedFinishReasonCount>
    kBlockedFinishReasons = {
        "SAFETY",
        "PROHIBITED_CONTENT",
        "BLOCKLIST",
        "RECITATION",
        "SPII",
        "IMAGE_SAFETY",
};
constexpr std::string_view kExportPrefix = "export";
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

std::int64_t extract_json_int64(const bj::object& obj, const char* key) noexcept {
    const auto* val = obj.if_contains(key);
    if (val == nullptr) {
        return 0;
    }
    if (val->is_int64()) {
        return val->as_int64();
    }
    if (val->is_uint64()) {
        const std::uint64_t u = val->as_uint64();
        constexpr auto kMaxInt64 =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        return static_cast<std::int64_t>(u > kMaxInt64 ? kMaxInt64 : u);
    }
    return 0;
}

void populate_usage_metadata(const bj::object& root_obj,
                             LlmResponse& out) noexcept {
    const auto* usage = root_obj.if_contains("usageMetadata");
    if (usage == nullptr || !usage->is_object()) {
        return;
    }
    const auto& u = usage->as_object();
    out.prompt_tokens = extract_json_int64(u, "promptTokenCount");
    out.output_tokens = extract_json_int64(u, "candidatesTokenCount");
    out.thought_tokens = extract_json_int64(u, "thoughtsTokenCount");
}

std::string extract_prompt_block_reason(const bj::object& root_obj) {
    const auto* feedback = root_obj.if_contains("promptFeedback");
    if (feedback == nullptr || !feedback->is_object()) {
        return {};
    }
    const auto* reason = feedback->as_object().if_contains("blockReason");
    if (reason == nullptr || !reason->is_string()) {
        return {};
    }
    const std::string_view raw_reason(reason->as_string().data(),
                                      reason->as_string().size());
    if (trim_ascii_view(raw_reason).empty()) {
        return {};
    }
    return std::string(raw_reason);
}

bool is_blocked_finish_reason(std::string_view reason) noexcept {
    return std::any_of(kBlockedFinishReasons.begin(),
                       kBlockedFinishReasons.end(),
                       [reason](std::string_view candidate) noexcept {
                           return candidate == reason;
                       });
}

std::string parse_double_quoted_dotenv(std::string_view rhs) {
    std::string out;
    out.reserve(rhs.size());
    for (std::size_t i = 1; i < rhs.size(); ++i) {
        const char ch = rhs[i];
        if (ch == '"') {
            return out;
        }
        if (ch == '\\' && i + 1 < rhs.size()) {
            const char next = rhs[++i];
            switch (next) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            default:
                out += '\\';
                out += next;
                break;
            }
            continue;
        }
        out += ch;
    }
    return out;
}

std::string parse_dotenv_rhs(std::string_view rhs) {
    rhs = trim_ascii_view(rhs);
    if (rhs.empty()) {
        return {};
    }
    if (rhs.front() == '\'') {
        const std::size_t close_quote = rhs.find('\'', 1);
        if (close_quote != std::string_view::npos) {
            return std::string(rhs.substr(1, close_quote - 1));
        }
        return std::string(rhs.substr(1));
    }
    if (rhs.front() == '"') {
        return parse_double_quoted_dotenv(rhs);
    }

    const std::size_t hash_pos = rhs.find('#');
    if (hash_pos != std::string_view::npos) {
        rhs = rhs.substr(0, hash_pos);
    }
    return trim_ascii(rhs);
}

}  // namespace

std::string build_argon_request_json(const LlmRequest& request) {
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

    bj::array safety_settings;
    safety_settings.reserve(kConfigurableHarmCategories.size());
    for (const char* category : kConfigurableHarmCategories) {
        safety_settings.emplace_back(bj::object{
            {"category", category},
            {"threshold", kSafetyThresholdBlockNone},
        });
    }
    root["safetySettings"] = std::move(safety_settings);

    bj::object gen_cfg;
    gen_cfg["maxOutputTokens"] =
        static_cast<std::int64_t>(request.max_output_tokens);
    gen_cfg["thinkingConfig"] = bj::object{
        {"thinkingBudget", static_cast<std::int64_t>(request.thinking_budget)},
    };
    if (request.json_mode) {
        gen_cfg["responseMimeType"] = "application/json";
    }
    root["generationConfig"] = std::move(gen_cfg);
    return bj::serialize(root);
}

Result<LlmResponse> parse_argon_response_json(std::string_view raw_json,
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

    if (http_status == kHttpTooManyRequests ||
        api_err_status == kResourceExhaustedStatus) {
        const std::string detail = !api_err_message.empty()
                                       ? api_err_message
                                       : "rate limited (HTTP " +
                                             std::to_string(http_status) + ")";
        return Result<LlmResponse>::failure(ErrorCode::RateLimited, detail,
                                            http_status);
    }

    if (http_status < kHttpOkMin || http_status > kHttpOkMax) {
        const std::string detail = !api_err_message.empty()
                                       ? api_err_message
                                       : "HTTP status " +
                                             std::to_string(http_status);
        return Result<LlmResponse>::failure(ErrorCode::HttpError, detail,
                                            http_status);
    }

    if (raw_json.empty() || parse_ec || !parsed.is_object()) {
        return Result<LlmResponse>::failure(
            ErrorCode::CorruptJson,
            parse_ec ? ("malformed response JSON: " + parse_ec.message())
                     : "response body is empty or not a JSON object",
            http_status);
    }

    const auto& root_obj = parsed.as_object();
    LlmResponse out;
    populate_usage_metadata(root_obj, out);

    const auto* cands = root_obj.if_contains("candidates");
    if (cands == nullptr || !cands->is_array() || cands->as_array().empty() ||
        !cands->as_array()[kFirstCandidateIndex].is_object()) {
        std::string block_reason = extract_prompt_block_reason(root_obj);
        if (!block_reason.empty()) {
            out.finish_reason = std::move(block_reason);
            out.content_blocked = true;
            return Result<LlmResponse>::success(std::move(out));
        }
        return Result<LlmResponse>::failure(
            ErrorCode::CorruptJson,
            "response JSON missing non-empty candidates array",
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

    if (trim_ascii_view(out.text).empty()) {
        if (is_blocked_finish_reason(out.finish_reason)) {
            out.text.clear();
            out.content_blocked = true;
            return Result<LlmResponse>::success(std::move(out));
        }
        return Result<LlmResponse>::failure(
            ErrorCode::CorruptJson,
            "response candidate contains no non-thought text output",
            http_status);
    }

    return Result<LlmResponse>::success(std::move(out));
}

std::string parse_dotenv_value(std::string_view dotenv_contents,
                               std::string_view key_name) {
    key_name = trim_ascii_view(key_name);
    if (key_name.empty()) {
        return {};
    }

    if (dotenv_contents.size() >= kUtf8Bom.size() &&
        dotenv_contents.substr(0, kUtf8Bom.size()) == kUtf8Bom) {
        dotenv_contents.remove_prefix(kUtf8Bom.size());
    }

    std::string resolved;
    std::size_t pos = 0;
    while (pos <= dotenv_contents.size()) {
        const std::size_t nl = dotenv_contents.find('\n', pos);
        std::string_view line =
            (nl == std::string_view::npos)
                ? dotenv_contents.substr(pos)
                : dotenv_contents.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? (dotenv_contents.size() + 1)
                                             : (nl + 1);

        line = trim_ascii_view(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.size() > kExportPrefix.size() &&
            line.substr(0, kExportPrefix.size()) == kExportPrefix &&
            (line[kExportPrefix.size()] == ' ' ||
             line[kExportPrefix.size()] == '\t')) {
            line = trim_ascii_view(line.substr(kExportPrefix.size()));
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view lhs = trim_ascii_view(line.substr(0, eq));
        if (lhs != key_name) {
            continue;
        }
        resolved = parse_dotenv_rhs(line.substr(eq + 1));
    }
    return resolved;
}

}  // namespace spot_checker
