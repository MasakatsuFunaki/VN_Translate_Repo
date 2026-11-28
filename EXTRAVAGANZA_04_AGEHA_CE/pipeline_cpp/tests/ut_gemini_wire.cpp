// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for Gemini generateContent wire payloads, response parsing,
// safety settings, thought-part filtering, and zero-thought telemetry guards.
#include "cache/cache_core.h"

#include "cache/gemini_client.h"
#include "cache/glossary.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tr = exm::translate;
namespace bj = boost::json;

namespace {

constexpr std::size_t kFirstIndex = 0;
constexpr std::size_t kZeroCount = 0;
constexpr std::size_t kSingleElementCount = 1;
constexpr std::size_t kExpectedSafetyCategoryCount = 5;
constexpr std::size_t kOutputTokenCaseCount = 7;
constexpr std::size_t kMalformedResponseCaseCount = 5;
constexpr std::size_t kBlockedCaseCount = 11;
constexpr std::size_t kThoughtGuardCaseCount = 10;
constexpr std::size_t kDisallowedModelCaseCount = 4;

constexpr std::size_t kZeroBatchLines = 0;
constexpr std::size_t kSingleBatchLine = 1;
constexpr std::size_t kBelowFloorBatchLines = 54;
constexpr std::size_t kAtFloorInsideRangeBatchLines = 55;
constexpr std::size_t kAtCeilingInsideRangeBatchLines = 218;
constexpr std::size_t kAboveCeilingBatchLines = 219;
constexpr std::size_t kMaxSizeTBatchLines = std::numeric_limits<std::size_t>::max();

constexpr std::int64_t kPinnedMinOutputTokens = 16384;
constexpr std::int64_t kPinnedOutputTokens55Lines = 16500;
constexpr std::int64_t kPinnedOutputTokens218Lines = 65400;
constexpr std::int64_t kPinnedMaxOutputTokens = 65536;

constexpr std::int64_t kZeroTokens = 0;
constexpr std::int64_t kNegativeOneThoughtTokens = -1;
constexpr std::int64_t kMinimumPositiveThoughtTokens = 1;
constexpr std::int64_t kSamplePromptTokens = 1420;
constexpr std::int64_t kSampleCandidatesTokens = 385;
constexpr std::int64_t kSampleThoughtTokens = 912;
constexpr std::int64_t kSampleCachedTokens = 1100;
constexpr std::int64_t kFollowupThoughtTokens = 640;

constexpr std::size_t kBelowGuardBatchLines = 3;
constexpr std::size_t kAtGuardBatchLines = 4;

constexpr std::size_t kCallCountAfterFirstBatch = 1;
constexpr std::size_t kCallCountAfterZeroThoughtFailure = 2;
constexpr std::size_t kCallCountAfterBlockedBatch = 3;
constexpr std::size_t kCallCountAfterRecoveryBatch = 4;

constexpr int kLineNumber1 = 1;
constexpr int kLineNumber2 = 2;
constexpr int kLineNumber3 = 3;
constexpr int kLineNumber4 = 4;

constexpr std::array<const char*, kExpectedSafetyCategoryCount> kRequiredHarmCategories = {
    "HARM_CATEGORY_HARASSMENT",
    "HARM_CATEGORY_HATE_SPEECH",
    "HARM_CATEGORY_SEXUALLY_EXPLICIT",
    "HARM_CATEGORY_DANGEROUS_CONTENT",
    "HARM_CATEGORY_CIVIC_INTEGRITY",
};

struct OutputTokenCase {
    const char* label;
    std::size_t batch_line_count;
    std::int64_t expected_max_output_tokens;
};

struct BlockedResponseCase {
    const char* label;
    const char* block_reason;
    const char* finish_reason;
    bool include_block_reason;
    bool include_candidate;
    bool expected_blocked;
};

enum class GuardOutcome {
    kAccepted,
    kRejectedWithZeroThought,
    kRejectedWithEmptyMessage,
};

struct ThoughtGuardCase {
    const char* label;
    std::string_view model;
    std::string_view thinking_level;
    bool blocked;
    std::size_t batch_line_count;
    std::int64_t thought_tokens;
    GuardOutcome expected_outcome;
};

std::string json_string_to_std(const bj::value& val) {
    const bj::string& str = val.as_string();
    return std::string(str.data(), str.size());
}

bj::object make_status_response(const BlockedResponseCase& tc) {
    bj::object root;
    if (tc.include_block_reason) {
        root["promptFeedback"] = bj::object{{"blockReason", tc.block_reason}};
    }
    if (tc.include_candidate) {
        bj::object candidate{
            {"content", bj::object{{"parts", bj::array{bj::object{{"text", "1. Line\n"}}}}}},
            {"finishReason", tc.finish_reason},
        };
        root["candidates"] = bj::array{std::move(candidate)};
    }
    return root;
}

GuardOutcome probe_thought_guard(std::string_view model,
                                 std::string_view thinking_level,
                                 bool blocked,
                                 std::size_t batch_line_count,
                                 std::int64_t thought_tokens) {
    try {
        tr::validate_thought_telemetry(
            model, thinking_level, blocked, batch_line_count, thought_tokens);
        return GuardOutcome::kAccepted;
    } catch (const tr::ZeroThoughtError& err) {
        if (std::string_view(err.what()).empty()) {
            return GuardOutcome::kRejectedWithEmptyMessage;
        }
        return GuardOutcome::kRejectedWithZeroThought;
    }
}

bj::object make_detailed_response(bj::array parts,
                                  std::int64_t thought_tokens,
                                  const char* finish_reason = "STOP") {
    bj::object candidate{
        {"content", bj::object{{"parts", std::move(parts)}}},
        {"finishReason", finish_reason},
    };
    bj::object usage{
        {"promptTokenCount", kSamplePromptTokens},
        {"candidatesTokenCount", kSampleCandidatesTokens},
        {"thoughtsTokenCount", thought_tokens},
        {"cachedContentTokenCount", kSampleCachedTokens},
    };
    return bj::object{
        {"candidates", bj::array{std::move(candidate)}},
        {"usageMetadata", std::move(usage)},
    };
}

class ScriptedGeminiClient final : public exm::gemini::Client {
public:
    void enqueue_response(bj::value response) {
        queued_responses_.push_back(std::move(response));
    }

    bj::value generate_content(const std::string& model, const bj::value& body) override {
        last_model_ = model;
        last_body_ = body;
        ++call_count_;
        if (next_index_ >= queued_responses_.size()) {
            throw std::logic_error("ScriptedGeminiClient exhausted queued responses");
        }
        bj::value out = queued_responses_[next_index_];
        ++next_index_;
        return out;
    }

    const std::string& last_model() const noexcept { return last_model_; }
    const bj::value& last_body() const noexcept { return last_body_; }
    std::size_t call_count() const noexcept { return call_count_; }

private:
    std::vector<bj::value> queued_responses_;
    std::size_t next_index_ = kFirstIndex;
    std::size_t call_count_ = kZeroCount;
    std::string last_model_;
    bj::value last_body_;
};

}  // namespace

TEST(GeminiWire, ComputeMaxOutputTokensClampsAndPreventsOverflow) {
    constexpr std::array<OutputTokenCase, kOutputTokenCaseCount> kCases = {{
        {"zero lines clamps to 16384 floor", kZeroBatchLines, kPinnedMinOutputTokens},
        {"single line clamps to 16384 floor", kSingleBatchLine, kPinnedMinOutputTokens},
        {"54 lines (16200) clamps to 16384 floor", kBelowFloorBatchLines, kPinnedMinOutputTokens},
        {"55 lines (16500) enters linear interior", kAtFloorInsideRangeBatchLines,
         kPinnedOutputTokens55Lines},
        {"218 lines (65400) stays below ceiling", kAtCeilingInsideRangeBatchLines,
         kPinnedOutputTokens218Lines},
        {"219 lines (65700) clamps to 65536 ceiling", kAboveCeilingBatchLines,
         kPinnedMaxOutputTokens},
        {"size_t::max clamps to 65536 without overflow", kMaxSizeTBatchLines,
         kPinnedMaxOutputTokens},
    }};

    ASSERT_EQ(kCases.size(), kOutputTokenCaseCount);
    for (const OutputTokenCase& tc : kCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(tr::compute_max_output_tokens(tc.batch_line_count),
                  tc.expected_max_output_tokens);
    }
}

TEST(GeminiWire, BuildGeminiRequestBodyConfiguresThinkingSafetyAndContextTags) {
    const std::vector<std::pair<std::string, std::string>> previous_context = {
        {"Ageha", "Ageha\r\n\"Earlier line.\""},
    };
    const std::vector<std::pair<std::string, std::string>> batch_lines = {
        {"Shumi", "シュミ\r\n「今の台詞」"},
    };
    const std::vector<std::pair<std::string, std::string>> context_after = {
        {"NARRATION", "直後の地の文。"},
    };

    const bj::object body =
        tr::build_gemini_request_body(batch_lines, previous_context, context_after);

    const bj::value* sys_val = body.if_contains("systemInstruction");
    ASSERT_NE(sys_val, nullptr);
    ASSERT_TRUE(sys_val->is_object());
    const bj::value* sys_parts_val = sys_val->as_object().if_contains("parts");
    ASSERT_NE(sys_parts_val, nullptr);
    ASSERT_TRUE(sys_parts_val->is_array());
    ASSERT_EQ(sys_parts_val->as_array().size(), kSingleElementCount);
    const bj::value& sys_part = sys_parts_val->as_array()[kFirstIndex];
    ASSERT_TRUE(sys_part.is_object());
    const bj::value* sys_text_val = sys_part.as_object().if_contains("text");
    ASSERT_NE(sys_text_val, nullptr);
    ASSERT_TRUE(sys_text_val->is_string());
    EXPECT_EQ(json_string_to_std(*sys_text_val), std::string(tr::SYSTEM_PROMPT));

    const bj::value* gen_val = body.if_contains("generationConfig");
    ASSERT_NE(gen_val, nullptr);
    ASSERT_TRUE(gen_val->is_object());
    const bj::object& gen_cfg = gen_val->as_object();

    const bj::value* max_tokens_val = gen_cfg.if_contains("maxOutputTokens");
    ASSERT_NE(max_tokens_val, nullptr);
    ASSERT_TRUE(max_tokens_val->is_int64() || max_tokens_val->is_uint64());
    EXPECT_EQ(max_tokens_val->to_number<std::int64_t>(), kPinnedMinOutputTokens);

    const bj::value* thinking_val = gen_cfg.if_contains("thinkingConfig");
    ASSERT_NE(thinking_val, nullptr);
    ASSERT_TRUE(thinking_val->is_object());
    const bj::object& thinking_cfg = thinking_val->as_object();
    EXPECT_EQ(thinking_cfg.size(), kSingleElementCount);
    EXPECT_EQ(thinking_cfg.if_contains("thinkingBudget"), nullptr);
    const bj::value* level_val = thinking_cfg.if_contains("thinkingLevel");
    ASSERT_NE(level_val, nullptr);
    ASSERT_TRUE(level_val->is_string());
    EXPECT_EQ(json_string_to_std(*level_val), "MEDIUM");

    const bj::value* safety_val = body.if_contains("safetySettings");
    ASSERT_NE(safety_val, nullptr);
    ASSERT_TRUE(safety_val->is_array());
    const bj::array& safety_settings = safety_val->as_array();
    ASSERT_EQ(safety_settings.size(), kExpectedSafetyCategoryCount);

    std::map<std::string, std::string> thresholds_by_category;
    for (const bj::value& item : safety_settings) {
        ASSERT_TRUE(item.is_object());
        const bj::object& entry = item.as_object();
        const bj::value* cat_val = entry.if_contains("category");
        const bj::value* thr_val = entry.if_contains("threshold");
        ASSERT_NE(cat_val, nullptr);
        ASSERT_NE(thr_val, nullptr);
        ASSERT_TRUE(cat_val->is_string());
        ASSERT_TRUE(thr_val->is_string());
        thresholds_by_category.emplace(json_string_to_std(*cat_val),
                                       json_string_to_std(*thr_val));
    }
    ASSERT_EQ(thresholds_by_category.size(), kExpectedSafetyCategoryCount);
    ASSERT_EQ(kRequiredHarmCategories.size(), kExpectedSafetyCategoryCount);
    for (const char* required_category : kRequiredHarmCategories) {
        SCOPED_TRACE(required_category);
        const auto it = thresholds_by_category.find(std::string(required_category));
        ASSERT_NE(it, thresholds_by_category.end());
        EXPECT_EQ(it->second, "BLOCK_NONE");
    }

    const bj::object low_body = tr::build_gemini_request_body(batch_lines, {}, {}, "LOW");
    const bj::value* low_gen_val = low_body.if_contains("generationConfig");
    ASSERT_NE(low_gen_val, nullptr);
    ASSERT_TRUE(low_gen_val->is_object());
    const bj::value* low_thinking_val = low_gen_val->as_object().if_contains("thinkingConfig");
    ASSERT_NE(low_thinking_val, nullptr);
    ASSERT_TRUE(low_thinking_val->is_object());
    const bj::object& low_thinking_cfg = low_thinking_val->as_object();
    EXPECT_EQ(low_thinking_cfg.size(), kSingleElementCount);
    EXPECT_EQ(low_thinking_cfg.if_contains("thinkingBudget"), nullptr);
    const bj::value* low_level_val = low_thinking_cfg.if_contains("thinkingLevel");
    ASSERT_NE(low_level_val, nullptr);
    ASSERT_TRUE(low_level_val->is_string());
    EXPECT_EQ(json_string_to_std(*low_level_val), "LOW");

    const bj::value* contents_val = body.if_contains("contents");
    ASSERT_NE(contents_val, nullptr);
    ASSERT_TRUE(contents_val->is_array());
    ASSERT_EQ(contents_val->as_array().size(), kSingleElementCount);
    const bj::value& user_turn = contents_val->as_array()[kFirstIndex];
    ASSERT_TRUE(user_turn.is_object());
    const bj::value* role_val = user_turn.as_object().if_contains("role");
    ASSERT_NE(role_val, nullptr);
    ASSERT_TRUE(role_val->is_string());
    EXPECT_EQ(json_string_to_std(*role_val), "user");

    const bj::value* parts_val = user_turn.as_object().if_contains("parts");
    ASSERT_NE(parts_val, nullptr);
    ASSERT_TRUE(parts_val->is_array());
    ASSERT_EQ(parts_val->as_array().size(), kSingleElementCount);
    const bj::value& first_part = parts_val->as_array()[kFirstIndex];
    ASSERT_TRUE(first_part.is_object());
    const bj::value* prompt_val = first_part.as_object().if_contains("text");
    ASSERT_NE(prompt_val, nullptr);
    ASSERT_TRUE(prompt_val->is_string());
    const std::string prompt_text = json_string_to_std(*prompt_val);
    EXPECT_NE(prompt_text.find("<previous_context>"), std::string::npos);
    EXPECT_NE(prompt_text.find("Earlier line."), std::string::npos);
    EXPECT_NE(prompt_text.find("<lines_to_translate>"), std::string::npos);
    EXPECT_NE(prompt_text.find("1. [Shumi] シュミ\\r\\n「今の台詞」"), std::string::npos);
    EXPECT_NE(prompt_text.find("<context_after>"), std::string::npos);
    EXPECT_NE(prompt_text.find("直後の地の文。"), std::string::npos);
}

TEST(GeminiWire, ParseGeminiResponseExtractsUsageFiltersThoughtPartsAndDefaultsMalformed) {
    const bj::array parts = {
        bj::object{{"thought", true}, {"text", "1. INTERNAL_REASONING\n"}},
        bj::object{{"text", "1. First translated\n"}},
        bj::object{{"thought", true}, {"text", "2. SECOND_SECRET_THOUGHT\n"}},
        bj::object{{"thought", false}, {"text", "2. Second translated\n"}},
    };
    const bj::object valid_response =
        make_detailed_response(parts, kSampleThoughtTokens, "STOP");

    const tr::ParsedGeminiResponse parsed = tr::parse_gemini_response(valid_response);
    EXPECT_EQ(parsed.candidate_text, "1. First translated\n2. Second translated\n");
    EXPECT_FALSE(parsed.blocked);
    EXPECT_EQ(parsed.finish_reason, "STOP");
    EXPECT_EQ(parsed.usage.input_tokens, kSamplePromptTokens);
    EXPECT_EQ(parsed.usage.output_tokens, kSampleCandidatesTokens);
    EXPECT_EQ(parsed.usage.thought_tokens, kSampleThoughtTokens);
    EXPECT_EQ(parsed.usage.cached_tokens, kSampleCachedTokens);

    const std::array<std::pair<const char*, bj::value>, kMalformedResponseCaseCount>
        malformed_cases = {{
            {"null root", bj::value(nullptr)},
            {"array root", bj::array{}},
            {"empty object", bj::object{}},
            {"empty candidates and non-object usageMetadata",
             bj::object{{"candidates", bj::array{}}, {"usageMetadata", "invalid"}}},
            {"non-integer usageMetadata fields and non-array parts",
             bj::object{
                 {"candidates", bj::array{bj::object{{"content", bj::object{{"parts", "bad"}}}}}},
                 {"usageMetadata",
                  bj::object{
                      {"promptTokenCount", "nan"},
                      {"candidatesTokenCount", nullptr},
                      {"thoughtsTokenCount", false},
                      {"cachedContentTokenCount", bj::array{}},
                  }},
             }},
        }};

    ASSERT_EQ(malformed_cases.size(), kMalformedResponseCaseCount);
    for (const auto& [label, payload] : malformed_cases) {
        SCOPED_TRACE(label);
        const tr::ParsedGeminiResponse fallback = tr::parse_gemini_response(payload);
        EXPECT_EQ(fallback.candidate_text, "");
        EXPECT_FALSE(fallback.blocked);
        EXPECT_EQ(fallback.usage.input_tokens, kZeroTokens);
        EXPECT_EQ(fallback.usage.output_tokens, kZeroTokens);
        EXPECT_EQ(fallback.usage.thought_tokens, kZeroTokens);
        EXPECT_EQ(fallback.usage.cached_tokens, kZeroTokens);
    }
}

TEST(GeminiWire, ParseGeminiResponseClassifiesBlockedReasons) {
    constexpr std::array<BlockedResponseCase, kBlockedCaseCount> kCases = {{
        {"STOP finishReason without promptFeedback", "", "STOP", false, true, false},
        {"MAX_TOKENS finishReason is not blocked", "", "MAX_TOKENS", false, true, false},
        {"BLOCK_REASON_UNSPECIFIED is not blocked", "BLOCK_REASON_UNSPECIFIED", "STOP", true,
         true, false},
        {"empty blockReason and finishReason strings are not blocked", "", "", true, true, false},
        {"promptFeedback PROHIBITED_CONTENT blocks even without candidates", "PROHIBITED_CONTENT",
         "", true, false, true},
        {"finishReason SAFETY is blocked", "", "SAFETY", false, true, true},
        {"finishReason PROHIBITED_CONTENT is blocked", "", "PROHIBITED_CONTENT", false, true, true},
        {"finishReason BLOCKLIST is blocked", "", "BLOCKLIST", false, true, true},
        {"finishReason SPII is blocked", "", "SPII", false, true, true},
        {"finishReason RECITATION is blocked", "", "RECITATION", false, true, true},
        {"finishReason IMAGE_SAFETY is blocked", "", "IMAGE_SAFETY", false, true, true},
    }};

    ASSERT_EQ(kCases.size(), kBlockedCaseCount);
    for (const BlockedResponseCase& tc : kCases) {
        SCOPED_TRACE(tc.label);
        const tr::ParsedGeminiResponse parsed =
            tr::parse_gemini_response(make_status_response(tc));
        EXPECT_EQ(parsed.blocked, tc.expected_blocked);
    }
}

TEST(GeminiWire, ValidateThoughtTelemetryEnforcesGuardedCarbonMediumHighSeams) {
    constexpr std::array<ThoughtGuardCase, kThoughtGuardCaseCount> kCases = {{
        {"carbon MEDIUM unblocked 4 lines with 0 thoughts throws", "gemini-4-carbon", "MEDIUM",
         false, kAtGuardBatchLines, kZeroTokens, GuardOutcome::kRejectedWithZeroThought},
        {"uppercase CARBON HIGH unblocked 4 lines with -1 thoughts throws", "GEMINI-4-CARBON",
         "HIGH", false, kAtGuardBatchLines, kNegativeOneThoughtTokens,
         GuardOutcome::kRejectedWithZeroThought},
        {"mixed-case carbon substring at medium throws", "models/Gemini-4-Carbon-preview",
         "medium", false, kAtGuardBatchLines, kZeroTokens, GuardOutcome::kRejectedWithZeroThought},
        {"neon MEDIUM unblocked 4 lines with 0 thoughts throws", "gemini-4-neon", "MEDIUM", false,
         kAtGuardBatchLines, kZeroTokens, GuardOutcome::kRejectedWithZeroThought},
        {"hydrogen HIGH unblocked 4 lines with 0 thoughts throws", "gemini-4-hydrogen", "HIGH",
         false, kAtGuardBatchLines, kZeroTokens, GuardOutcome::kRejectedWithZeroThought},
        {"3-line batch below 4-line threshold is accepted", "gemini-4-carbon", "MEDIUM", false,
         kBelowGuardBatchLines, kZeroTokens, GuardOutcome::kAccepted},
        {"4-line batch with 1 thought token is accepted", "gemini-4-carbon", "MEDIUM", false,
         kAtGuardBatchLines, kMinimumPositiveThoughtTokens, GuardOutcome::kAccepted},
        {"blocked response with 0 thought tokens is accepted", "gemini-4-carbon", "MEDIUM", true,
         kAtGuardBatchLines, kZeroTokens, GuardOutcome::kAccepted},
        {"LOW thinking level with 0 thought tokens is accepted", "gemini-4-carbon", "LOW", false,
         kAtGuardBatchLines, kZeroTokens, GuardOutcome::kAccepted},
        {"non-gemini4 model at MEDIUM with 0 thought tokens is accepted", "gemini-2.5-pro",
         "MEDIUM", false, kAtGuardBatchLines, kZeroTokens, GuardOutcome::kAccepted},
    }};

    ASSERT_EQ(kCases.size(), kThoughtGuardCaseCount);
    for (const ThoughtGuardCase& tc : kCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(probe_thought_guard(tc.model,
                                      tc.thinking_level,
                                      tc.blocked,
                                      tc.batch_line_count,
                                      tc.thought_tokens),
                  tc.expected_outcome);
    }
}

TEST(GeminiWire, CallGeminiDetailedFiltersThoughtsPropagatesUsageAndEnforcesZeroThoughtGuard) {
    const std::vector<std::pair<std::string, std::string>> four_line_batch = {
        {"NARRATION", "一つ目の文。"},
        {"NARRATION", "二つ目の文。"},
        {"NARRATION", "三つ目の文。"},
        {"NARRATION", "四つ目の文。"},
    };
    const bj::array valid_parts = {
        bj::object{{"thought", true}, {"text", "1. WRONG"}},
        bj::object{{"text", "1. Right one\n2. Right two\n3. Right three\n4. Right four\n"}},
    };

    ScriptedGeminiClient client;
    client.enqueue_response(make_detailed_response(valid_parts, kSampleThoughtTokens, "STOP"));
    client.enqueue_response(make_detailed_response(valid_parts, kZeroTokens, "STOP"));
    client.enqueue_response(
        make_detailed_response(bj::array{}, kZeroTokens, "PROHIBITED_CONTENT"));
    client.enqueue_response(make_detailed_response(valid_parts, kFollowupThoughtTokens, "STOP"));

    const tr::GeminiBatchResult first =
        tr::call_gemini_detailed(client, four_line_batch, {}, {}, "gemini-4-carbon", "MEDIUM");
    EXPECT_EQ(client.call_count(), kCallCountAfterFirstBatch);
    EXPECT_EQ(client.last_model(), "gemini-4-carbon");
    EXPECT_FALSE(first.blocked);
    EXPECT_EQ(first.usage.input_tokens, kSamplePromptTokens);
    EXPECT_EQ(first.usage.output_tokens, kSampleCandidatesTokens);
    EXPECT_EQ(first.usage.thought_tokens, kSampleThoughtTokens);
    EXPECT_EQ(first.usage.cached_tokens, kSampleCachedTokens);
    ASSERT_EQ(first.translations.size(), kAtGuardBatchLines);
    EXPECT_EQ(first.translations.at(kLineNumber1), "Right one");
    EXPECT_EQ(first.translations.at(kLineNumber2), "Right two");
    EXPECT_EQ(first.translations.at(kLineNumber3), "Right three");
    EXPECT_EQ(first.translations.at(kLineNumber4), "Right four");

    EXPECT_THROW(
        tr::call_gemini_detailed(client, four_line_batch, {}, {}, "gemini-4-carbon", "MEDIUM"),
        tr::ZeroThoughtError);
    EXPECT_EQ(client.call_count(), kCallCountAfterZeroThoughtFailure);

    const tr::GeminiBatchResult blocked_batch =
        tr::call_gemini_detailed(client, four_line_batch, {}, {}, "gemini-4-carbon", "MEDIUM");
    EXPECT_EQ(client.call_count(), kCallCountAfterBlockedBatch);
    EXPECT_TRUE(blocked_batch.blocked);
    EXPECT_EQ(blocked_batch.translations.size(), kZeroCount);
    EXPECT_EQ(blocked_batch.usage.thought_tokens, kZeroTokens);

    const tr::GeminiBatchResult recovered =
        tr::call_gemini_detailed(client, four_line_batch, {}, {}, "gemini-4-carbon", "MEDIUM");
    EXPECT_EQ(client.call_count(), kCallCountAfterRecoveryBatch);
    EXPECT_FALSE(recovered.blocked);
    EXPECT_EQ(recovered.usage.thought_tokens, kFollowupThoughtTokens);
    ASSERT_EQ(recovered.translations.size(), kAtGuardBatchLines);
    EXPECT_EQ(recovered.translations.at(kLineNumber1), "Right one");
    EXPECT_EQ(recovered.translations.at(kLineNumber4), "Right four");
}

TEST(GeminiWire, RejectsDisallowedArgonAndEmbeddedNulModelsBeforeTransport) {
    constexpr std::string_view kEmbeddedNulModel("gemini-4-carbon\0bad", 19);
    constexpr std::array<std::pair<const char*, std::string_view>, kDisallowedModelCaseCount>
        kDisallowedCases = {{
            {"canonical argon model", "gemini-4-argon"},
            {"uppercase ARGON model", "GEMINI-4-ARGON"},
            {"prefixed mixed-case argon model", "models/Gemini-4-Argon-preview"},
            {"embedded NUL model identifier", kEmbeddedNulModel},
        }};

    const std::vector<std::pair<std::string, std::string>> single_line_batch = {
        {"NARRATION", "一つ目の文。"},
    };
    const bj::array valid_parts = {
        bj::object{{"text", "1. Valid after rejection\n"}},
    };

    ScriptedGeminiClient scripted;
    scripted.enqueue_response(make_detailed_response(valid_parts, kSampleThoughtTokens, "STOP"));
    auto https_client = exm::gemini::make_https_client("offline-dummy-key");

    ASSERT_EQ(kDisallowedCases.size(), kDisallowedModelCaseCount);
    for (const auto& [label, disallowed_model] : kDisallowedCases) {
        SCOPED_TRACE(label);
        try {
            (void)tr::call_gemini_detailed(
                scripted, single_line_batch, {}, {}, disallowed_model, "MEDIUM");
            FAIL() << "Expected vn::llm::LlmError for disallowed model in call_gemini_detailed";
        } catch (const tr::ZeroThoughtError&) {
            FAIL() << "Disallowed model must fail model policy, not ZeroThoughtError";
        } catch (const vn::llm::LlmError& err) {
            EXPECT_NE(std::string_view(err.what()).find("disallowed"), std::string_view::npos);
        }
        EXPECT_EQ(scripted.call_count(), kZeroCount);

        try {
            (void)https_client->generate_content(std::string(disallowed_model), bj::object{});
            FAIL() << "Expected vn::llm::LlmError for disallowed model in HttpsClient";
        } catch (const vn::llm::LlmError& err) {
            EXPECT_NE(std::string_view(err.what()).find("disallowed"), std::string_view::npos);
        }
    }

    const tr::GeminiBatchResult valid_after_reject = tr::call_gemini_detailed(
        scripted, single_line_batch, {}, {}, "gemini-4-carbon", "MEDIUM");
    EXPECT_EQ(scripted.call_count(), kCallCountAfterFirstBatch);
    ASSERT_EQ(valid_after_reject.translations.size(), kSingleElementCount);
    EXPECT_EQ(valid_after_reject.translations.at(kLineNumber1), "Valid after rejection");
}
