// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "client/argon_wire_codec.h"

#include "client/argon_client.h"

#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace spot_checker {
namespace {

constexpr int kHttpOkLower = 200;
constexpr int kHttpOkUpper = 299;
constexpr int kHttpBelowOk = 199;
constexpr int kHttpAboveOk = 300;
constexpr int kHttpTooManyRequests = 429;
constexpr int kHttpServiceUnavailable = 503;
constexpr int kHttpInternalError = 500;

constexpr int kCustomThinkingBudget = 4096;
constexpr int kCustomMaxOutputTokens = 8192;
constexpr int kZeroThinkingBudget = 0;
constexpr int kCompactMaxOutputTokens = 1024;
constexpr std::size_t kExpectedSafetyCategoryCount = 5U;
constexpr std::int64_t kDecimalRadix = 10;
constexpr std::int64_t kLargeTokenCount = 3000000000LL;
constexpr std::int64_t kExpectedOutputTokens = 345;
constexpr std::int64_t kExpectedThoughtTokens = 890;
constexpr std::int64_t kBlockedPromptTokens = 420;
constexpr std::int64_t kBlockedThoughtTokens = 15;

// Minimal RFC 8259 JSON value reader used strictly to inspect emitted request
// payloads without assuming root object key ordering.
struct JsonVal {
    enum class Kind { Null, Bool, Int, Str, Arr, Obj };
    Kind kind = Kind::Null;
    bool b = false;
    std::int64_t i = 0;
    std::string s;
    std::vector<JsonVal> arr;
    std::map<std::string, JsonVal> obj;

    bool contains(const std::string& key) const {
        return kind == Kind::Obj && obj.find(key) != obj.end();
    }

    const JsonVal& at(const std::string& key) const {
        const auto it = obj.find(key);
        if (kind != Kind::Obj || it == obj.end()) {
            throw std::runtime_error("missing JSON key: " + key);
        }
        return it->second;
    }
};

class MiniJsonParser {
public:
    explicit MiniJsonParser(std::string_view src) noexcept : src_(src) {}

    JsonVal parse_document() {
        skip_ws();
        JsonVal root = parse_value();
        skip_ws();
        if (pos_ != src_.size()) {
            throw std::runtime_error("trailing bytes after JSON root");
        }
        return root;
    }

private:
    void skip_ws() noexcept {
        while (pos_ < src_.size() &&
               (src_[pos_] == ' ' || src_[pos_] == '\t' ||
                src_[pos_] == '\r' || src_[pos_] == '\n')) {
            ++pos_;
        }
    }

    char consume() {
        if (pos_ >= src_.size()) {
            throw std::runtime_error("unexpected end of JSON");
        }
        return src_[pos_++];
    }

    JsonVal parse_value() {
        skip_ws();
        if (pos_ >= src_.size()) {
            throw std::runtime_error("empty JSON value");
        }
        const char ch = src_[pos_];
        if (ch == '{') {
            return parse_object();
        }
        if (ch == '[') {
            return parse_array();
        }
        if (ch == '"') {
            JsonVal v;
            v.kind = JsonVal::Kind::Str;
            v.s = parse_string();
            return v;
        }
        if (ch == 't' || ch == 'f') {
            return parse_bool();
        }
        if (ch == 'n') {
            return parse_null();
        }
        return parse_int();
    }

    std::string parse_string() {
        if (consume() != '"') {
            throw std::runtime_error("expected '\"'");
        }
        std::string out;
        while (pos_ < src_.size()) {
            const char c = consume();
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            const char esc = consume();
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default:
                    throw std::runtime_error("unsupported escape in test JSON");
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }

    JsonVal parse_object() {
        consume();
        JsonVal v;
        v.kind = JsonVal::Kind::Obj;
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return v;
        }
        for (;;) {
            skip_ws();
            const std::string key = parse_string();
            skip_ws();
            if (consume() != ':') {
                throw std::runtime_error("expected ':'");
            }
            v.obj[key] = parse_value();
            skip_ws();
            const char sep = consume();
            if (sep == '}') {
                return v;
            }
            if (sep != ',') {
                throw std::runtime_error("expected ',' or '}'");
            }
        }
    }

    JsonVal parse_array() {
        consume();
        JsonVal v;
        v.kind = JsonVal::Kind::Arr;
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            return v;
        }
        for (;;) {
            v.arr.push_back(parse_value());
            skip_ws();
            const char sep = consume();
            if (sep == ']') {
                return v;
            }
            if (sep != ',') {
                throw std::runtime_error("expected ',' or ']'");
            }
        }
    }

    JsonVal parse_bool() {
        constexpr std::string_view kTrueLiteral = "true";
        constexpr std::string_view kFalseLiteral = "false";
        JsonVal v;
        v.kind = JsonVal::Kind::Bool;
        if (src_.substr(pos_, kTrueLiteral.size()) == kTrueLiteral) {
            pos_ += kTrueLiteral.size();
            v.b = true;
            return v;
        }
        if (src_.substr(pos_, kFalseLiteral.size()) == kFalseLiteral) {
            pos_ += kFalseLiteral.size();
            v.b = false;
            return v;
        }
        throw std::runtime_error("invalid boolean literal");
    }

    JsonVal parse_null() {
        constexpr std::string_view kNullLiteral = "null";
        if (src_.substr(pos_, kNullLiteral.size()) != kNullLiteral) {
            throw std::runtime_error("invalid null literal");
        }
        pos_ += kNullLiteral.size();
        return JsonVal{};
    }

    JsonVal parse_int() {
        bool neg = false;
        if (src_[pos_] == '-') {
            neg = true;
            ++pos_;
        }
        if (pos_ >= src_.size() ||
            !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
            throw std::runtime_error("invalid integer literal");
        }
        std::int64_t val = 0;
        while (pos_ < src_.size() &&
               std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
            val = val * kDecimalRadix + (src_[pos_] - '0');
            ++pos_;
        }
        JsonVal v;
        v.kind = JsonVal::Kind::Int;
        v.i = neg ? -val : val;
        return v;
    }

    std::string_view src_;
    std::size_t pos_ = 0U;
};

void verify_block_none_safety_settings(const JsonVal& doc) {
    ASSERT_TRUE(doc.contains("safetySettings"));
    const JsonVal& safety = doc.at("safetySettings");
    ASSERT_EQ(safety.kind, JsonVal::Kind::Arr);
    ASSERT_EQ(safety.arr.size(), kExpectedSafetyCategoryCount);

    std::map<std::string, std::string> thresholds_by_category;
    for (const JsonVal& entry : safety.arr) {
        ASSERT_EQ(entry.kind, JsonVal::Kind::Obj);
        ASSERT_TRUE(entry.contains("category"));
        ASSERT_TRUE(entry.contains("threshold"));
        thresholds_by_category[entry.at("category").s] =
            entry.at("threshold").s;
    }
    ASSERT_EQ(thresholds_by_category.size(), kExpectedSafetyCategoryCount);

    constexpr std::string_view kExpectedCategories[kExpectedSafetyCategoryCount] = {
        "HARM_CATEGORY_HARASSMENT",
        "HARM_CATEGORY_HATE_SPEECH",
        "HARM_CATEGORY_SEXUALLY_EXPLICIT",
        "HARM_CATEGORY_DANGEROUS_CONTENT",
        "HARM_CATEGORY_CIVIC_INTEGRITY",
    };
    for (const std::string_view cat : kExpectedCategories) {
        SCOPED_TRACE(cat);
        const auto it = thresholds_by_category.find(std::string(cat));
        ASSERT_NE(it, thresholds_by_category.end());
        EXPECT_EQ(it->second, "BLOCK_NONE");
    }
}

TEST(ArgonWireCodec, BuildsGenerateContentRequestWithSafetySettingsAndOptionalSystemInstruction) {
    LlmRequest full_req;
    full_req.system_prompt = "You are a literary JP-EN VN editor.";
    full_req.user_prompt = "Check line:\n「おはよう」 -> \"Morning.\"";
    full_req.thinking_budget = kCustomThinkingBudget;
    full_req.max_output_tokens = kCustomMaxOutputTokens;
    full_req.json_mode = true;

    const std::string full_raw = build_argon_request_json(full_req);
    const JsonVal full_doc = MiniJsonParser(full_raw).parse_document();
    ASSERT_EQ(full_doc.kind, JsonVal::Kind::Obj);
    verify_block_none_safety_settings(full_doc);

    ASSERT_TRUE(full_doc.contains("systemInstruction"));
    const JsonVal& sys_parts = full_doc.at("systemInstruction").at("parts");
    ASSERT_EQ(sys_parts.kind, JsonVal::Kind::Arr);
    ASSERT_EQ(sys_parts.arr.size(), 1U);
    EXPECT_EQ(sys_parts.arr[0].at("text").s, full_req.system_prompt);

    const JsonVal& contents = full_doc.at("contents");
    ASSERT_EQ(contents.kind, JsonVal::Kind::Arr);
    ASSERT_EQ(contents.arr.size(), 1U);
    EXPECT_EQ(contents.arr[0].at("role").s, "user");
    const JsonVal& user_parts = contents.arr[0].at("parts");
    ASSERT_EQ(user_parts.kind, JsonVal::Kind::Arr);
    ASSERT_EQ(user_parts.arr.size(), 1U);
    EXPECT_EQ(user_parts.arr[0].at("text").s, full_req.user_prompt);

    const JsonVal& gen_cfg = full_doc.at("generationConfig");
    EXPECT_EQ(gen_cfg.at("maxOutputTokens").i, kCustomMaxOutputTokens);
    const JsonVal& full_thinking_cfg = gen_cfg.at("thinkingConfig");
    EXPECT_EQ(full_thinking_cfg.at("thinkingLevel").s, "MEDIUM");
    EXPECT_FALSE(full_thinking_cfg.contains("thinkingBudget"));
    EXPECT_EQ(gen_cfg.at("responseMimeType").s, "application/json");

    LlmRequest custom_level_req = full_req;
    custom_level_req.thinking_level = "  LOW\t";
    const JsonVal custom_level_doc =
        MiniJsonParser(build_argon_request_json(custom_level_req))
            .parse_document();
    const JsonVal& custom_thinking_cfg =
        custom_level_doc.at("generationConfig").at("thinkingConfig");
    EXPECT_EQ(custom_thinking_cfg.at("thinkingLevel").s, "LOW");
    EXPECT_FALSE(custom_thinking_cfg.contains("thinkingBudget"));

    LlmRequest minimal_req;
    minimal_req.system_prompt.clear();
    minimal_req.user_prompt = "Ping";
    minimal_req.thinking_level.clear();
    minimal_req.thinking_budget = kZeroThinkingBudget;
    minimal_req.max_output_tokens = kCompactMaxOutputTokens;
    minimal_req.json_mode = false;

    const JsonVal min_doc =
        MiniJsonParser(build_argon_request_json(minimal_req)).parse_document();
    verify_block_none_safety_settings(min_doc);
    EXPECT_FALSE(min_doc.contains("systemInstruction"));
    const JsonVal& min_cfg = min_doc.at("generationConfig");
    EXPECT_EQ(min_cfg.at("maxOutputTokens").i, kCompactMaxOutputTokens);
    const JsonVal& min_thinking_cfg = min_cfg.at("thinkingConfig");
    EXPECT_EQ(min_thinking_cfg.at("thinkingBudget").i, kZeroThinkingBudget);
    EXPECT_FALSE(min_thinking_cfg.contains("thinkingLevel"));
    EXPECT_FALSE(min_cfg.contains("responseMimeType"));
}

TEST(ArgonWireCodec, ParsesMultiPartResponsesSafetyRefusalsAndErrorBoundaries) {
    constexpr std::string_view kSuccessPayload = R"({
      "candidates": [
        {
          "content": {
            "role": "model",
            "parts": [
              {"thought": true, "text": "Internal reasoning that MUST NOT leak."},
              {"text": "{\"improvements\": ["},
              {"thought": false, "text": "{\"id\": 1}]}"}
            ]
          },
          "finishReason": "STOP"
        }
      ],
      "usageMetadata": {
        "promptTokenCount": 3000000000,
        "candidatesTokenCount": 345,
        "thoughtsTokenCount": 890
      }
    })";

    const Result<LlmResponse> ok_res =
        parse_argon_response_json(kSuccessPayload, kHttpOkUpper);
    ASSERT_TRUE(ok_res.is_ok()) << ok_res.error_message();
    EXPECT_FALSE(ok_res.value().content_blocked);
    EXPECT_EQ(ok_res.value().text, "{\"improvements\": [{\"id\": 1}]}");
    EXPECT_EQ(ok_res.value().finish_reason, "STOP");
    EXPECT_EQ(ok_res.value().prompt_tokens, kLargeTokenCount);
    EXPECT_EQ(ok_res.value().output_tokens, kExpectedOutputTokens);
    EXPECT_EQ(ok_res.value().thought_tokens, kExpectedThoughtTokens);

    struct BlockedCase {
        const char* label;
        std::string_view body;
        std::string_view expected_reason;
        std::int64_t expected_prompt_tokens;
        std::int64_t expected_thought_tokens;
    };
    constexpr BlockedCase kBlockedCases[] = {
        {"missing_candidates_with_prompt_feedback_block_reason",
         R"({
           "promptFeedback": {"blockReason": "PROHIBITED_CONTENT"},
           "usageMetadata": {"promptTokenCount": 420, "candidatesTokenCount": 0, "thoughtsTokenCount": 15}
         })",
         "PROHIBITED_CONTENT",
         kBlockedPromptTokens,
         kBlockedThoughtTokens},
        {"empty_candidates_with_prompt_feedback_block_reason",
         R"({
           "candidates": [],
           "promptFeedback": {"blockReason": "SAFETY"},
           "usageMetadata": {"promptTokenCount": 420}
         })",
         "SAFETY",
         kBlockedPromptTokens,
         0},
        {"candidate_finish_reason_safety_without_content",
         R"({
           "candidates": [{"finishReason": "SAFETY"}],
           "usageMetadata": {"promptTokenCount": 420, "thoughtsTokenCount": 15}
         })",
         "SAFETY",
         kBlockedPromptTokens,
         kBlockedThoughtTokens},
        {"candidate_finish_reason_prohibited_content_thought_only",
         R"({
           "candidates": [{
             "content": {"parts": [{"thought": true, "text": "refused"}]},
             "finishReason": "PROHIBITED_CONTENT"
           }],
           "usageMetadata": {"promptTokenCount": 420, "thoughtsTokenCount": 15}
         })",
         "PROHIBITED_CONTENT",
         kBlockedPromptTokens,
         kBlockedThoughtTokens},
        {"candidate_finish_reason_blocklist_empty_parts",
         R"({
           "candidates": [{"content": {"parts": []}, "finishReason": "BLOCKLIST"}],
           "usageMetadata": {"promptTokenCount": 420}
         })",
         "BLOCKLIST",
         kBlockedPromptTokens,
         0},
        {"candidate_finish_reason_recitation_empty_text",
         R"({
           "candidates": [{"content": {"parts": [{"text": ""}]}, "finishReason": "RECITATION"}],
           "usageMetadata": {"promptTokenCount": 420}
         })",
         "RECITATION",
         kBlockedPromptTokens,
         0},
        {"candidate_finish_reason_spii",
         R"({
           "candidates": [{"finishReason": "SPII"}],
           "usageMetadata": {"promptTokenCount": 420}
         })",
         "SPII",
         kBlockedPromptTokens,
         0},
        {"candidate_finish_reason_image_safety",
         R"({
           "candidates": [{"finishReason": "IMAGE_SAFETY"}],
           "usageMetadata": {"promptTokenCount": 420}
         })",
         "IMAGE_SAFETY",
         kBlockedPromptTokens,
         0},
    };

    for (const auto& tc : kBlockedCases) {
        SCOPED_TRACE(tc.label);
        const Result<LlmResponse> blocked =
            parse_argon_response_json(tc.body, kHttpOkLower);
        ASSERT_TRUE(blocked.is_ok()) << blocked.error_message();
        EXPECT_TRUE(blocked.value().content_blocked);
        EXPECT_EQ(blocked.value().text, "");
        EXPECT_EQ(blocked.value().finish_reason, tc.expected_reason);
        EXPECT_EQ(blocked.value().prompt_tokens, tc.expected_prompt_tokens);
        EXPECT_EQ(blocked.value().output_tokens, 0);
        EXPECT_EQ(blocked.value().thought_tokens, tc.expected_thought_tokens);
    }

    struct ErrorCase {
        const char* label;
        int http_status;
        std::string_view body;
        ErrorCode expected_code;
    };
    constexpr ErrorCase kErrors[] = {
        {"http_429_rate_limited",
         kHttpTooManyRequests,
         R"({"error":{"message":"Quota exceeded"}})",
         ErrorCode::RateLimited},
        {"json_resource_exhausted_status",
         kHttpServiceUnavailable,
         R"({"error":{"code":503,"status":"RESOURCE_EXHAUSTED","message":"Busy"}})",
         ErrorCode::RateLimited},
        {"http_199_below_ok_range",
         kHttpBelowOk,
         R"({"candidates":[{"content":{"parts":[{"text":"ok"}]}}]})",
         ErrorCode::HttpError},
        {"http_300_above_ok_range",
         kHttpAboveOk,
         R"({"candidates":[{"content":{"parts":[{"text":"ok"}]}}]})",
         ErrorCode::HttpError},
        {"http_500_internal_error",
         kHttpInternalError,
         R"({"error":{"code":500,"status":"INTERNAL","message":"Boom"}})",
         ErrorCode::HttpError},
        {"malformed_json_200",
         kHttpOkLower,
         "{not valid json",
         ErrorCode::CorruptJson},
        {"missing_candidates_200",
         kHttpOkLower,
         R"({"usageMetadata":{"promptTokenCount":10}})",
         ErrorCode::CorruptJson},
        {"empty_block_reason_without_candidates_still_corrupt",
         kHttpOkLower,
         R"({"promptFeedback":{"blockReason":""}})",
         ErrorCode::CorruptJson},
        {"empty_candidates_array_200",
         kHttpOkLower,
         R"({"candidates":[]})",
         ErrorCode::CorruptJson},
        {"thought_only_parts_with_stop_finish_reason_corrupt",
         kHttpOkLower,
         R"({"candidates":[{"content":{"parts":[{"thought":true,"text":"only thinking"}]},"finishReason":"STOP"}]})",
         ErrorCode::CorruptJson},
        {"empty_text_with_max_tokens_finish_reason_corrupt",
         kHttpOkLower,
         R"({"candidates":[{"content":{"parts":[{"text":""}]},"finishReason":"MAX_TOKENS"}]})",
         ErrorCode::CorruptJson},
    };

    for (const auto& tc : kErrors) {
        SCOPED_TRACE(tc.label);
        const Result<LlmResponse> err =
            parse_argon_response_json(tc.body, tc.http_status);
        EXPECT_FALSE(err.is_ok());
        EXPECT_EQ(err.error_code(), tc.expected_code);
        if (tc.expected_code == ErrorCode::HttpError ||
            tc.expected_code == ErrorCode::RateLimited) {
            EXPECT_EQ(err.status().http_status, tc.http_status);
        }
    }
}

bool is_env_var_present(const wchar_t* name) {
    wchar_t* buffer = nullptr;
    std::size_t count = 0U;
    if (_wdupenv_s(&buffer, &count, name) != 0 || buffer == nullptr) {
        std::free(buffer);
        return false;
    }
    std::free(buffer);
    return true;
}

TEST(ArgonWireCodec, ParsesDotenvValuesHermeticallyWithoutMutatingEnvironment) {
    constexpr std::string_view kDotenv =
        "# Leading full-line comment\r\n"
        "# PLAIN_KEY=commented_out\r\n"
        "MY_PLAIN_KEY=wrong_prefix\r\n"
        "PLAIN_KEY_EXTRA=wrong_suffix\r\n"
        "PLAIN_KEY = alpha_123 # trailing inline comment\r\n"
        "export DOUBLE_QUOTED = \"val#with#hashes and spaces\" # outer comment\n"
        "SINGLE_QUOTED='single # preserved'\n"
        "EMPTY_VAL=\r\n"
        "SPOT_CHECKER_UT_HERMETIC_SENTINEL=do_not_leak_to_env\n";

    ASSERT_FALSE(is_env_var_present(L"SPOT_CHECKER_UT_HERMETIC_SENTINEL"));

    EXPECT_EQ(parse_dotenv_value(kDotenv, "PLAIN_KEY"), "alpha_123");
    EXPECT_EQ(parse_dotenv_value(kDotenv, "DOUBLE_QUOTED"),
              "val#with#hashes and spaces");
    EXPECT_EQ(parse_dotenv_value(kDotenv, "SINGLE_QUOTED"),
              "single # preserved");
    EXPECT_EQ(parse_dotenv_value(kDotenv, "EMPTY_VAL"), "");
    EXPECT_EQ(parse_dotenv_value(kDotenv, "MISSING_KEY"), "");
    EXPECT_EQ(parse_dotenv_value(kDotenv, ""), "");
    EXPECT_EQ(parse_dotenv_value(kDotenv, "SPOT_CHECKER_UT_HERMETIC_SENTINEL"),
              "do_not_leak_to_env");

    EXPECT_FALSE(is_env_var_present(L"SPOT_CHECKER_UT_HERMETIC_SENTINEL"));
}

TEST(ArgonWireCodec,
     RejectsDisallowedArgonModelsAndInvalidThinkingLevelsWithoutNetworkIo) {
    LlmRequest valid_req;
    valid_req.system_prompt = "Editor";
    valid_req.user_prompt = "「おはよう」 -> \"Morning.\"";

    constexpr char kEmbeddedNulModel[] = "gemini-4-carbon\0tail";
    constexpr std::size_t kEmbeddedNulModelLen = sizeof(kEmbeddedNulModel) - 1U;

    struct DisallowedModelCase {
        const char* label;
        std::string_view api_key;
        std::string model;
        bool expect_argon_in_message;
    };
    const DisallowedModelCase kCases[] = {
        {"lowercase_gemini_4_argon_with_key",
         "unused_sentinel_api_key",
         "gemini-4-argon",
         true},
        {"uppercase_padded_argon_variant_without_key",
         "",
         "  GEMINI-4-ARGON-FALLBACK\t",
         true},
        {"embedded_nul_byte_seam_without_key",
         "",
         std::string(kEmbeddedNulModel, kEmbeddedNulModelLen),
         false},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        ArgonClient client(std::string(tc.api_key), tc.model);
        const Result<LlmResponse> res = client.generate(valid_req);
        ASSERT_FALSE(res.is_ok());
        EXPECT_EQ(res.error_code(), ErrorCode::InvalidArgument)
            << res.error_message();
        EXPECT_FALSE(res.error_message().empty());
        if (tc.expect_argon_in_message) {
            EXPECT_NE(res.error_message().find("argon"), std::string::npos)
                << res.error_message();
        }
    }

    LlmRequest invalid_level_req = valid_req;
    invalid_level_req.thinking_level = "ULTRA";
    EXPECT_THROW((void)build_argon_request_json(invalid_level_req),
                 vn::llm::LlmError);

    ArgonClient carbon_client("unused_sentinel_api_key",
                              std::string(kDefaultModel));
    const Result<LlmResponse> bad_level_res =
        carbon_client.generate(invalid_level_req);
    ASSERT_FALSE(bad_level_res.is_ok());
    EXPECT_EQ(bad_level_res.error_code(), ErrorCode::InvalidArgument)
        << bad_level_res.error_message();
}

}  // namespace
}  // namespace spot_checker
