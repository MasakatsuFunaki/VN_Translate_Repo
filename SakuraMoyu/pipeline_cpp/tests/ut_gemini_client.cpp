// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-room unit tests for Gemini v1beta generateContent wire adaptation,
// thought-part filtering, usage telemetry extraction, and the zero-thought
// fail-fast circuit breaker.
#include "translate/gemini_client.h"
#include "translate/translate_core.h"

#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace gm = fav::gemini;
namespace tr = fav::translate;

namespace {

constexpr std::int64_t kClampedMinOutputTokens = 16384;
constexpr std::int64_t kBelowMinOutputTokens = 16383;
constexpr std::int64_t kMidRangeOutputTokens = 32768;
constexpr std::int64_t kClampedMaxOutputTokens = 65536;
constexpr std::int64_t kAboveMaxOutputTokens = 65537;
constexpr std::int64_t kNegativeMaxTokens = -1;
constexpr std::int64_t kZeroTokens = 0;
constexpr std::uint64_t kZeroUint64Tokens = 0u;
constexpr std::uint64_t kMaxUint64Tokens = std::numeric_limits<std::uint64_t>::max();

constexpr double kSampleTemperature = 0.3;
constexpr int kCustomTimeoutSec = 120;

constexpr std::size_t kExpectedSafetyCategoryCount = 5u;
constexpr std::size_t kExpectedSingleElementCount = 1u;
constexpr std::size_t kFirstIndex = 0u;

constexpr std::int64_t kSamplePromptTokens = 1250;
constexpr std::int64_t kSampleCandidatesTokens = 80;
constexpr std::int64_t kSampleThoughtsTokens = 620;
constexpr std::int64_t kExpectedCombinedOutputTokens = 700;
constexpr std::int64_t kSampleCachedTokens = 900;

constexpr std::int64_t kBatchPromptTokens = 1100;
constexpr std::int64_t kBatchCandidatesTokens = 48;
constexpr std::int64_t kBatchValidThoughtsTokens = 512;
constexpr std::int64_t kExpectedBatchOutputTokens = 560;
constexpr std::int64_t kBatchCachedTokens = 700;

constexpr std::size_t kBelowZeroThoughtBatchSize = 3u;
constexpr std::size_t kMinZeroThoughtBatchSize = 4u;
constexpr std::int64_t kNegativeThoughtTokens = -1;
constexpr std::int64_t kSingleThoughtToken = 1;

constexpr int kLineOne = 1;
constexpr int kLineTwo = 2;
constexpr int kLineThree = 3;
constexpr int kLineFour = 4;

constexpr int kCircuitBreakerBatchSize = 4;
constexpr int kFatalRuntimeExitCode = 1;
constexpr std::size_t kExpectedSingleApiCall = 1u;
constexpr std::size_t kExpectedTwoApiCalls = 2u;
constexpr int kBaseScriptOffset = 100;
constexpr int kScriptOffsetStride = 40;
constexpr int kInitialLineIndex = 0;

constexpr std::uint32_t kByteMask = 0xFFu;
constexpr unsigned int kOneByteShift = 8u;
constexpr unsigned int kTwoBytesShift = 16u;
constexpr unsigned int kThreeBytesShift = 24u;
constexpr std::size_t kScriptHeaderByteCount = 4u;
constexpr std::size_t kNulTerminatorByteCount = 1u;
constexpr std::uint8_t kNulByte = 0x00u;
constexpr std::uint8_t kPushStringOpcode = 0x0Eu;
constexpr std::uint8_t kFaceGraphicOpcode = 0x10u;
constexpr std::uint8_t kSampleByteOperand = 0x01u;

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::string json_str(const boost::json::value& v) {
    const auto& s = v.as_string();
    return std::string(s.data(), s.size());
}

boost::json::object make_base_messages_body() {
    boost::json::object user_msg;
    user_msg["role"] = "user";
    user_msg["content"] = "1. 朝が来た。";

    boost::json::array messages;
    messages.emplace_back(std::move(user_msg));

    boost::json::object body;
    body["system"] = "Translate Japanese visual novel dialogue.";
    body["messages"] = std::move(messages);
    body["temperature"] = kSampleTemperature;
    return body;
}

boost::json::object make_wire_response(std::string_view finish_reason,
                                       std::int64_t prompt_tokens,
                                       std::int64_t candidates_tokens,
                                       std::int64_t thoughts_tokens,
                                       std::int64_t cached_tokens,
                                       std::string_view visible_text,
                                       std::optional<std::string_view> thought_text = std::nullopt) {
    boost::json::array parts;
    if (thought_text.has_value()) {
        boost::json::object thought_part;
        thought_part["thought"] = true;
        thought_part["text"] = *thought_text;
        parts.emplace_back(std::move(thought_part));
    }
    boost::json::object visible_part;
    visible_part["text"] = visible_text;
    parts.emplace_back(std::move(visible_part));

    boost::json::object content;
    content["role"] = "model";
    content["parts"] = std::move(parts);

    boost::json::object candidate;
    candidate["content"] = std::move(content);
    if (!finish_reason.empty()) {
        candidate["finishReason"] = finish_reason;
    }

    boost::json::array candidates;
    candidates.emplace_back(std::move(candidate));

    boost::json::object usage;
    usage["promptTokenCount"] = prompt_tokens;
    usage["candidatesTokenCount"] = candidates_tokens;
    usage["thoughtsTokenCount"] = thoughts_tokens;
    usage["cachedContentTokenCount"] = cached_tokens;

    boost::json::object response;
    response["candidates"] = std::move(candidates);
    response["usageMetadata"] = std::move(usage);
    return response;
}

class FakeEndpoint final : public gm::Endpoint {
public:
    void enqueue_response(boost::json::value response) {
        queued_responses_.push_back(std::move(response));
    }

    boost::json::value generate_content(std::string_view model,
                                        const boost::json::object& wire_request,
                                        int timeout_sec) override {
        const std::size_t idx = call_count_++;
        last_model_ = std::string(model);
        last_wire_request_ = wire_request;
        last_timeout_sec_ = timeout_sec;
        if (idx < queued_responses_.size()) {
            return queued_responses_[idx];
        }
        return queued_responses_.back();
    }

    std::size_t call_count() const noexcept { return call_count_; }
    const std::string& last_model() const noexcept { return last_model_; }
    const boost::json::object& last_wire_request() const noexcept { return last_wire_request_; }
    int last_timeout_sec() const noexcept { return last_timeout_sec_; }

private:
    std::vector<boost::json::value> queued_responses_;
    std::size_t call_count_ = 0u;
    std::string last_model_;
    boost::json::object last_wire_request_;
    int last_timeout_sec_ = 0;
};

void append_u32(fav::Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kOneByteShift) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kTwoBytesShift) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kThreeBytesShift) & kByteMask));
}

void append_push_string(fav::Bytes& out, const std::string& utf8) {
    const fav::Bytes cp932 = fav::utf8_to_cp932_replace(utf8);
    out.push_back(kPushStringOpcode);
    out.push_back(static_cast<std::uint8_t>(cp932.size() + kNulTerminatorByteCount));
    out.insert(out.end(), cp932.begin(), cp932.end());
    out.push_back(kNulByte);
}

fav::Bytes build_valid_cast_hcb() {
    fav::Bytes body;
    body.push_back(kFaceGraphicOpcode);
    body.push_back(kSampleByteOperand);
    append_push_string(body, "　クロ　");
    body.push_back(kFaceGraphicOpcode);
    body.push_back(kSampleByteOperand);
    append_push_string(body, "　ハル　");

    fav::Bytes hcb;
    append_u32(hcb, static_cast<std::uint32_t>(kScriptHeaderByteCount + body.size()));
    hcb.insert(hcb.end(), body.begin(), body.end());
    return hcb;
}

}  // namespace

TEST(GeminiClient, ResolvesModelAndThinkingLevelWithFallbacks) {
    struct ResolverCase {
        const char* name;
        bool is_object;
        const char* model_field;
        const char* thinking_field;
        const char* expected_model;
        const char* expected_thinking;
    };

    constexpr ResolverCase kCases[] = {
        {"non_object_null", false, nullptr, nullptr, "gemini-4-carbon", "MEDIUM"},
        {"missing_fields", true, nullptr, nullptr, "gemini-4-carbon", "MEDIUM"},
        {"empty_strings", true, "", "", "gemini-4-carbon", "MEDIUM"},
        {"explicit_overrides", true, "gemini-2.5-pro", "LOW", "gemini-2.5-pro", "LOW"},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.name);
        boost::json::value body;
        if (tc.is_object) {
            boost::json::object obj;
            if (tc.model_field != nullptr) {
                obj["model"] = tc.model_field;
            }
            if (tc.thinking_field != nullptr) {
                obj["thinking_level"] = tc.thinking_field;
            }
            body = std::move(obj);
        }

        EXPECT_EQ(gm::resolve_request_model(body), tc.expected_model);
        EXPECT_EQ(gm::resolve_thinking_level(body), tc.expected_thinking);
    }
}

TEST(GeminiClient, RejectsDisallowedArgonAndEmbeddedNulModelsAcrossResolverWireBuilderAndClient) {
    struct DisallowedModelCase {
        const char* name;
        std::string_view model;
    };

    static constexpr char kEmbeddedNulModel[] = "gemini-4-carbon\0extra";
    constexpr DisallowedModelCase kCases[] = {
        {"canonical_gemini_4_argon", "gemini-4-argon"},
        {"uppercase_argon", "GEMINI-4-ARGON"},
        {"prefixed_argon", "models/gemini-4-argon-preview"},
        {"embedded_nul_byte_seam",
         std::string_view(kEmbeddedNulModel, sizeof(kEmbeddedNulModel) - 1u)},
    };

    FakeEndpoint endpoint;
    endpoint.enqueue_response(make_wire_response(
        "STOP", kSamplePromptTokens, kSampleCandidatesTokens, kSampleThoughtsTokens,
        kZeroTokens, "1. Morning came.\n"));
    gm::Client client(&endpoint);

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.name);
        boost::json::object body = make_base_messages_body();
        body["model"] = boost::json::string(tc.model.data(), tc.model.size());

        EXPECT_THROW(gm::resolve_request_model(body), gm::ApiError);
        EXPECT_THROW(gm::build_generate_content_request(body), gm::ApiError);
        EXPECT_THROW(client.messages(body), gm::ApiError);
        EXPECT_THROW(
            tr::call_gemini_batch(client, {"朝が来た。"}, {}, {},
                                  std::string(tc.model), "MEDIUM"),
            gm::ApiError);
        EXPECT_EQ(endpoint.call_count(), kZeroUint64Tokens);
    }

    // Valid -> Failing -> Valid seam: client succeeds immediately afterward with
    // supported default `gemini-4-carbon`.
    const boost::json::value ok = client.messages(make_base_messages_body());
    EXPECT_EQ(endpoint.call_count(), kExpectedSingleApiCall);
    EXPECT_EQ(endpoint.last_model(), "gemini-4-carbon");
    EXPECT_EQ(gm::extract_usage(ok).thought_tokens, kSampleThoughtsTokens);
}

TEST(GeminiClient, BuildsGenerateContentRequestWithThinkingLevelAndSafetyCategories) {
    const boost::json::object default_body = make_base_messages_body();
    const boost::json::object default_wire = gm::build_generate_content_request(default_body);

    ASSERT_NE(default_wire.if_contains("generationConfig"), nullptr);
    ASSERT_TRUE(default_wire.at("generationConfig").is_object());
    const auto& default_gen = default_wire.at("generationConfig").as_object();
    EXPECT_EQ(default_gen.if_contains("thinkingBudget"), nullptr);

    ASSERT_NE(default_gen.if_contains("thinkingConfig"), nullptr);
    ASSERT_TRUE(default_gen.at("thinkingConfig").is_object());
    const auto& default_thinking = default_gen.at("thinkingConfig").as_object();
    EXPECT_EQ(default_thinking.if_contains("thinkingBudget"), nullptr);
    ASSERT_EQ(default_thinking.size(), kExpectedSingleElementCount);
    ASSERT_NE(default_thinking.if_contains("thinkingLevel"), nullptr);
    EXPECT_EQ(json_str(default_thinking.at("thinkingLevel")), "MEDIUM");

    boost::json::object low_body = make_base_messages_body();
    low_body["thinking_level"] = "LOW";
    const boost::json::object low_wire = gm::build_generate_content_request(low_body);
    ASSERT_NE(low_wire.if_contains("generationConfig"), nullptr);
    const auto& low_gen = low_wire.at("generationConfig").as_object();
    ASSERT_NE(low_gen.if_contains("thinkingConfig"), nullptr);
    const auto& low_thinking = low_gen.at("thinkingConfig").as_object();
    EXPECT_EQ(low_thinking.if_contains("thinkingBudget"), nullptr);
    ASSERT_EQ(low_thinking.size(), kExpectedSingleElementCount);
    ASSERT_NE(low_thinking.if_contains("thinkingLevel"), nullptr);
    EXPECT_EQ(json_str(low_thinking.at("thinkingLevel")), "LOW");

    ASSERT_NE(default_wire.if_contains("safetySettings"), nullptr);
    ASSERT_TRUE(default_wire.at("safetySettings").is_array());
    const auto& safety_settings = default_wire.at("safetySettings").as_array();
    ASSERT_EQ(safety_settings.size(), kExpectedSafetyCategoryCount);

    std::map<std::string, std::string> thresholds_by_category;
    for (const auto& entry : safety_settings) {
        ASSERT_TRUE(entry.is_object());
        const auto& entry_obj = entry.as_object();
        ASSERT_NE(entry_obj.if_contains("category"), nullptr);
        ASSERT_NE(entry_obj.if_contains("threshold"), nullptr);
        ASSERT_TRUE(entry_obj.at("category").is_string());
        ASSERT_TRUE(entry_obj.at("threshold").is_string());
        thresholds_by_category.emplace(json_str(entry_obj.at("category")),
                                       json_str(entry_obj.at("threshold")));
    }

    ASSERT_EQ(thresholds_by_category.size(), kExpectedSafetyCategoryCount);
    constexpr const char* kExpectedCategories[] = {
        "HARM_CATEGORY_HARASSMENT",
        "HARM_CATEGORY_HATE_SPEECH",
        "HARM_CATEGORY_SEXUALLY_EXPLICIT",
        "HARM_CATEGORY_DANGEROUS_CONTENT",
        "HARM_CATEGORY_CIVIC_INTEGRITY",
    };
    for (const char* category : kExpectedCategories) {
        SCOPED_TRACE(category);
        ASSERT_NE(thresholds_by_category.find(category), thresholds_by_category.end());
        EXPECT_EQ(thresholds_by_category.at(category), "BLOCK_NONE");
    }
}

TEST(GeminiClient, ClampsMaxOutputTokensAcrossBoundariesAndUint64Max) {
    enum class TokenInputKind { Omitted, SignedInt64, UnsignedUint64 };

    struct ClampCase {
        const char* name;
        TokenInputKind kind;
        std::int64_t signed_val;
        std::uint64_t unsigned_val;
        std::int64_t expected_max_output_tokens;
    };

    constexpr ClampCase kCases[] = {
        {"omitted_defaults_to_min", TokenInputKind::Omitted, kZeroTokens, kZeroUint64Tokens,
         kClampedMinOutputTokens},
        {"negative_one_clamps_to_min", TokenInputKind::SignedInt64, kNegativeMaxTokens,
         kZeroUint64Tokens, kClampedMinOutputTokens},
        {"zero_clamps_to_min", TokenInputKind::SignedInt64, kZeroTokens, kZeroUint64Tokens,
         kClampedMinOutputTokens},
        {"min_minus_one_clamps_to_min", TokenInputKind::SignedInt64, kBelowMinOutputTokens,
         kZeroUint64Tokens, kClampedMinOutputTokens},
        {"exact_min_preserved", TokenInputKind::SignedInt64, kClampedMinOutputTokens,
         kZeroUint64Tokens, kClampedMinOutputTokens},
        {"interior_value_preserved", TokenInputKind::SignedInt64, kMidRangeOutputTokens,
         kZeroUint64Tokens, kMidRangeOutputTokens},
        {"exact_max_preserved", TokenInputKind::SignedInt64, kClampedMaxOutputTokens,
         kZeroUint64Tokens, kClampedMaxOutputTokens},
        {"max_plus_one_clamps_to_max", TokenInputKind::SignedInt64, kAboveMaxOutputTokens,
         kZeroUint64Tokens, kClampedMaxOutputTokens},
        {"uint64_max_clamps_to_max_without_wraparound", TokenInputKind::UnsignedUint64,
         kZeroTokens, kMaxUint64Tokens, kClampedMaxOutputTokens},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.name);
        boost::json::object body = make_base_messages_body();
        if (tc.kind == TokenInputKind::SignedInt64) {
            body["max_tokens"] = tc.signed_val;
        } else if (tc.kind == TokenInputKind::UnsignedUint64) {
            body["max_tokens"] = tc.unsigned_val;
        }

        const boost::json::object wire = gm::build_generate_content_request(body);
        ASSERT_NE(wire.if_contains("generationConfig"), nullptr);
        const auto& gen_cfg = wire.at("generationConfig").as_object();
        ASSERT_NE(gen_cfg.if_contains("maxOutputTokens"), nullptr);
        const auto& max_out = gen_cfg.at("maxOutputTokens");
        ASSERT_TRUE(max_out.is_int64() || max_out.is_uint64());
        EXPECT_EQ(max_out.to_number<std::int64_t>(), tc.expected_max_output_tokens);
    }
}

TEST(GeminiClient, ParsesResponseFilteringThoughtPartsAndExtractingUsage) {
    boost::json::array parts;
    {
        boost::json::object p0;
        p0["thought"] = true;
        p0["text"] = "Reasoning step 1 that must not appear.\n";
        parts.emplace_back(std::move(p0));
    }
    {
        boost::json::object p1;
        p1["text"] = "1. Good evening.\n";
        parts.emplace_back(std::move(p1));
    }
    {
        boost::json::object p2;
        p2["thought"] = true;
        p2["text"] = "Reasoning step 2 that must not appear.\n";
        parts.emplace_back(std::move(p2));
    }
    {
        boost::json::object p3;
        p3["thought"] = false;
        p3["text"] = "2. Welcome home.";
        parts.emplace_back(std::move(p3));
    }

    boost::json::object content;
    content["role"] = "model";
    content["parts"] = std::move(parts);

    boost::json::object candidate;
    candidate["content"] = std::move(content);
    candidate["finishReason"] = "STOP";

    boost::json::array candidates;
    candidates.emplace_back(std::move(candidate));

    boost::json::object usage_meta;
    usage_meta["promptTokenCount"] = kSamplePromptTokens;
    usage_meta["candidatesTokenCount"] = kSampleCandidatesTokens;
    usage_meta["thoughtsTokenCount"] = kSampleThoughtsTokens;
    usage_meta["cachedContentTokenCount"] = kSampleCachedTokens;

    boost::json::object raw;
    raw["candidates"] = std::move(candidates);
    raw["usageMetadata"] = std::move(usage_meta);

    const boost::json::object parsed = gm::parse_generate_content_response(raw);
    ASSERT_NE(parsed.if_contains("content"), nullptr);
    ASSERT_TRUE(parsed.at("content").is_array());
    const auto& content_blocks = parsed.at("content").as_array();
    ASSERT_EQ(content_blocks.size(), kExpectedSingleElementCount);
    ASSERT_TRUE(content_blocks[kFirstIndex].is_object());
    const auto& text_block = content_blocks[kFirstIndex].as_object();
    EXPECT_EQ(json_str(text_block.at("type")), "text");
    EXPECT_EQ(json_str(text_block.at("text")), "1. Good evening.\n2. Welcome home.");

    ASSERT_NE(parsed.if_contains("stop_reason"), nullptr);
    EXPECT_EQ(json_str(parsed.at("stop_reason")), "end_turn");

    ASSERT_NE(parsed.if_contains("usage"), nullptr);
    const auto& usage_obj = parsed.at("usage").as_object();
    EXPECT_EQ(usage_obj.at("input_tokens").to_number<std::int64_t>(), kSamplePromptTokens);
    EXPECT_EQ(usage_obj.at("thought_tokens").to_number<std::int64_t>(), kSampleThoughtsTokens);
    EXPECT_EQ(usage_obj.at("output_tokens").to_number<std::int64_t>(),
              kExpectedCombinedOutputTokens);
    EXPECT_EQ(usage_obj.at("cache_read_input_tokens").to_number<std::int64_t>(),
              kSampleCachedTokens);
    EXPECT_EQ(usage_obj.at("cache_creation_input_tokens").to_number<std::int64_t>(), kZeroTokens);

    const gm::Usage extracted = gm::extract_usage(parsed);
    EXPECT_EQ(extracted.input_tokens, kSamplePromptTokens);
    EXPECT_EQ(extracted.thought_tokens, kSampleThoughtsTokens);
    EXPECT_EQ(extracted.output_tokens, kExpectedCombinedOutputTokens);
    EXPECT_EQ(extracted.cache_read_input_tokens, kSampleCachedTokens);
    EXPECT_EQ(extracted.cache_creation_input_tokens, kZeroTokens);

    raw.erase("usageMetadata");
    const boost::json::object parsed_no_usage = gm::parse_generate_content_response(raw);
    const gm::Usage zero_usage = gm::extract_usage(parsed_no_usage);
    EXPECT_EQ(zero_usage.input_tokens, kZeroTokens);
    EXPECT_EQ(zero_usage.thought_tokens, kZeroTokens);
    EXPECT_EQ(zero_usage.output_tokens, kZeroTokens);
    EXPECT_EQ(zero_usage.cache_read_input_tokens, kZeroTokens);
    EXPECT_EQ(zero_usage.cache_creation_input_tokens, kZeroTokens);

    const gm::Usage empty_usage = gm::extract_usage(boost::json::object{});
    EXPECT_EQ(empty_usage.input_tokens, kZeroTokens);
    EXPECT_EQ(empty_usage.thought_tokens, kZeroTokens);
    EXPECT_EQ(empty_usage.output_tokens, kZeroTokens);
    EXPECT_EQ(empty_usage.cache_read_input_tokens, kZeroTokens);
    EXPECT_EQ(empty_usage.cache_creation_input_tokens, kZeroTokens);
}

TEST(GeminiClient, MapsFinishReasonsAndPromptFeedbackBlocksToStopReason) {
    enum class CandidatePresence { SingleCandidate, EmptyArray, MissingKey };

    struct StopReasonCase {
        const char* name;
        CandidatePresence candidate_presence;
        const char* finish_reason;
        const char* prompt_block_reason;
        const char* expected_stop_reason;
    };

    constexpr StopReasonCase kCases[] = {
        {"stop_maps_to_end_turn", CandidatePresence::SingleCandidate, "STOP", nullptr, "end_turn"},
        {"missing_finish_reason_defaults_to_end_turn", CandidatePresence::SingleCandidate, nullptr,
         nullptr, "end_turn"},
        {"max_tokens_maps_to_max_tokens", CandidatePresence::SingleCandidate, "MAX_TOKENS",
         nullptr, "max_tokens"},
        {"safety_maps_to_refusal", CandidatePresence::SingleCandidate, "SAFETY", nullptr,
         "refusal"},
        {"prohibited_content_maps_to_refusal", CandidatePresence::SingleCandidate,
         "PROHIBITED_CONTENT", nullptr, "refusal"},
        {"recitation_maps_to_refusal", CandidatePresence::SingleCandidate, "RECITATION", nullptr,
         "refusal"},
        {"blocklist_maps_to_refusal", CandidatePresence::SingleCandidate, "BLOCKLIST", nullptr,
         "refusal"},
        {"spii_maps_to_refusal", CandidatePresence::SingleCandidate, "SPII", nullptr, "refusal"},
        {"unknown_finish_reason_preserved_verbatim", CandidatePresence::SingleCandidate,
         "MALFORMED_FUNCTION_CALL", nullptr, "MALFORMED_FUNCTION_CALL"},
        {"prompt_feedback_block_with_missing_candidates", CandidatePresence::MissingKey, nullptr,
         "PROHIBITED_CONTENT", "refusal"},
        {"prompt_feedback_block_with_empty_candidates", CandidatePresence::EmptyArray, nullptr,
         "SAFETY", "refusal"},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.name);
        boost::json::object raw;
        if (tc.candidate_presence == CandidatePresence::SingleCandidate) {
            raw = make_wire_response(tc.finish_reason != nullptr ? tc.finish_reason : "",
                                     kSamplePromptTokens, kSampleCandidatesTokens,
                                     kSampleThoughtsTokens, kZeroTokens, "1. Translated.");
        } else if (tc.candidate_presence == CandidatePresence::EmptyArray) {
            raw["candidates"] = boost::json::array{};
        }

        if (tc.prompt_block_reason != nullptr) {
            boost::json::object feedback;
            feedback["blockReason"] = tc.prompt_block_reason;
            raw["promptFeedback"] = std::move(feedback);
        }

        const boost::json::object parsed = gm::parse_generate_content_response(raw);
        ASSERT_NE(parsed.if_contains("stop_reason"), nullptr);
        EXPECT_EQ(json_str(parsed.at("stop_reason")), tc.expected_stop_reason);
    }
}

TEST(GeminiClient, ZeroThoughtPredicateEnforcesAllBoundarySeams) {
    struct PredicateCase {
        const char* name;
        const char* model;
        const char* thinking_level;
        const char* stop_reason;
        std::size_t batch_line_count;
        std::int64_t thought_tokens;
        bool expected_violation;
    };

    constexpr PredicateCase kCases[] = {
        {"batch_size_three_below_seam_is_allowed", "gemini-4-carbon", "MEDIUM", "end_turn",
         kBelowZeroThoughtBatchSize, kZeroTokens, false},
        {"batch_size_four_at_seam_with_zero_thoughts_trips_medium", "gemini-4-carbon", "MEDIUM",
         "end_turn", kMinZeroThoughtBatchSize, kZeroTokens, true},
        {"batch_size_four_at_seam_with_zero_thoughts_trips_high", "gemini-4-carbon", "HIGH",
         "end_turn", kMinZeroThoughtBatchSize, kZeroTokens, true},
        {"negative_thought_tokens_trips", "gemini-4-carbon", "MEDIUM", "end_turn",
         kMinZeroThoughtBatchSize, kNegativeThoughtTokens, true},
        {"single_thought_token_above_seam_is_allowed", "gemini-4-carbon", "MEDIUM", "end_turn",
         kMinZeroThoughtBatchSize, kSingleThoughtToken, false},
        {"uppercase_carbon_model_matches_case_insensitively", "GEMINI-4-CARBON", "MEDIUM",
         "end_turn", kMinZeroThoughtBatchSize, kZeroTokens, true},
        {"prefixed_carbon_model_substring_matches", "models/gemini-4-Carbon-preview", "MEDIUM",
         "end_turn", kMinZeroThoughtBatchSize, kZeroTokens, true},
        {"disallowed_argon_model_is_unguarded", "gemini-4-argon", "HIGH", "end_turn",
         kMinZeroThoughtBatchSize, kZeroTokens, false},
        {"non_gemini4_model_is_exempt", "gemini-2.5-pro", "HIGH", "end_turn",
         kMinZeroThoughtBatchSize, kZeroTokens, false},
        {"low_thinking_level_is_exempt", "gemini-4-carbon", "LOW", "end_turn",
         kMinZeroThoughtBatchSize, kZeroTokens, false},
        {"empty_thinking_level_is_exempt", "gemini-4-carbon", "", "end_turn",
         kMinZeroThoughtBatchSize, kZeroTokens, false},
        {"refusal_stop_reason_is_exempt", "gemini-4-carbon", "MEDIUM", "refusal",
         kMinZeroThoughtBatchSize, kZeroTokens, false},
        {"max_tokens_stop_reason_still_trips", "gemini-4-carbon", "MEDIUM", "max_tokens",
         kMinZeroThoughtBatchSize, kZeroTokens, true},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.name);
        EXPECT_EQ(tr::is_zero_thought_violation(tc.model, tc.thinking_level, tc.stop_reason,
                                                tc.batch_line_count, tc.thought_tokens),
                  tc.expected_violation);
    }
}

TEST(GeminiClient, CallGeminiBatchEnforcesZeroThoughtBreakerAndRecoversOnValidCall) {
    EXPECT_THROW(gm::Client(static_cast<gm::Endpoint*>(nullptr)), gm::ApiError);

    const std::vector<std::string> batch_lines = {
        "朝が来た。",
        "夜風が吹いた。",
        "桜が舞った。",
        "夜が戻った。",
    };
    constexpr const char* kVisibleTranslations =
        "1. Morning came.\n"
        "2. The night breeze blew.\n"
        "3. Cherry blossoms danced.\n"
        "4. Night returned.\n";

    FakeEndpoint fake_endpoint;
    fake_endpoint.enqueue_response(make_wire_response(
        "STOP", kBatchPromptTokens, kBatchCandidatesTokens, kZeroTokens, kBatchCachedTokens,
        kVisibleTranslations));
    fake_endpoint.enqueue_response(make_wire_response(
        "STOP", kBatchPromptTokens, kBatchCandidatesTokens, kBatchValidThoughtsTokens,
        kBatchCachedTokens, kVisibleTranslations,
        "1. INTERNAL THOUGHT THAT MUST NOT OVERWRITE LINE ONE\n"));

    gm::Client client(&fake_endpoint);

    EXPECT_THROW(
        tr::call_gemini_batch(client, batch_lines, {"Previous line."}, {"次の行。"},
                              "gemini-4-carbon", "MEDIUM"),
        tr::ZeroThoughtError);

    ASSERT_EQ(fake_endpoint.call_count(), kExpectedSingleApiCall);
    EXPECT_EQ(fake_endpoint.last_model(), "gemini-4-carbon");
    const auto& wire_req = fake_endpoint.last_wire_request();
    ASSERT_NE(wire_req.if_contains("generationConfig"), nullptr);
    const auto& gen_cfg = wire_req.at("generationConfig").as_object();
    EXPECT_EQ(gen_cfg.if_contains("thinkingBudget"), nullptr);
    ASSERT_NE(gen_cfg.if_contains("thinkingConfig"), nullptr);
    const auto& thinking_cfg = gen_cfg.at("thinkingConfig").as_object();
    EXPECT_EQ(thinking_cfg.if_contains("thinkingBudget"), nullptr);
    ASSERT_EQ(thinking_cfg.size(), kExpectedSingleElementCount);
    EXPECT_EQ(json_str(thinking_cfg.at("thinkingLevel")), "MEDIUM");

    const tr::BatchResult ok_result = tr::call_gemini_batch(
        client, batch_lines, {"Previous line."}, {"次の行。"}, "gemini-4-carbon", "MEDIUM");
    ASSERT_EQ(fake_endpoint.call_count(), kExpectedTwoApiCalls);
    EXPECT_EQ(ok_result.stop_reason, "end_turn");
    EXPECT_EQ(ok_result.usage.input_tokens, kBatchPromptTokens);
    EXPECT_EQ(ok_result.usage.thought_tokens, kBatchValidThoughtsTokens);
    EXPECT_EQ(ok_result.usage.output_tokens, kExpectedBatchOutputTokens);
    EXPECT_EQ(ok_result.usage.cache_read_input_tokens, kBatchCachedTokens);
    EXPECT_EQ(ok_result.usage.cache_creation_input_tokens, kZeroTokens);

    ASSERT_EQ(ok_result.translations.size(), kMinZeroThoughtBatchSize);
    EXPECT_EQ(ok_result.translations.at(kLineOne), "Morning came.");
    EXPECT_EQ(ok_result.translations.at(kLineTwo), "The night breeze blew.");
    EXPECT_EQ(ok_result.translations.at(kLineThree), "Cherry blossoms danced.");
    EXPECT_EQ(ok_result.translations.at(kLineFour), "Night returned.");

    FakeEndpoint timeout_endpoint;
    timeout_endpoint.enqueue_response(make_wire_response(
        "STOP", kBatchPromptTokens, kBatchCandidatesTokens, kBatchValidThoughtsTokens,
        kZeroTokens, "1. Morning came.\n"));
    gm::LazyClient lazy_client(&timeout_endpoint);
    const boost::json::value msg_resp =
        lazy_client.get().messages(make_base_messages_body(), kCustomTimeoutSec);
    ASSERT_EQ(timeout_endpoint.call_count(), kExpectedSingleApiCall);
    EXPECT_EQ(timeout_endpoint.last_timeout_sec(), kCustomTimeoutSec);
    EXPECT_EQ(gm::extract_usage(msg_resp).thought_tokens, kBatchValidThoughtsTokens);
}

TEST(GeminiClient, PropagatesRateLimitAndApiErrorsAcrossCustomEndpointAndRecovers) {
    class FaultInjectingEndpoint final : public gm::Endpoint {
    public:
        enum class Action { ThrowRateLimit, ReturnJson429, ThrowApiError, Succeed };

        void enqueue(Action action) { actions_.push_back(action); }

        boost::json::value generate_content(std::string_view /*model*/,
                                            const boost::json::object& /*wire_request*/,
                                            int /*timeout_sec*/) override {
            const Action act = actions_.at(calls_++);
            if (act == Action::ThrowRateLimit) {
                throw gm::RateLimitError("HTTP 429 quota exceeded on custom endpoint");
            }
            if (act == Action::ReturnJson429) {
                boost::json::object err;
                err["code"] = 429;
                err["status"] = "RESOURCE_EXHAUSTED";
                err["message"] = "Per-minute token rate limit reached";
                boost::json::object root;
                root["error"] = std::move(err);
                return root;
            }
            if (act == Action::ThrowApiError) {
                throw gm::ApiError("Upstream gateway error");
            }
            return make_wire_response(
                "STOP", kSamplePromptTokens, kSampleCandidatesTokens,
                kSampleThoughtsTokens, kZeroTokens, "1. Morning came.\n");
        }

        std::size_t calls() const noexcept { return calls_; }

    private:
        std::vector<Action> actions_;
        std::size_t calls_ = 0u;
    };

    constexpr std::size_t kExpectedFiveEndpointCalls = 5u;
    FaultInjectingEndpoint endpoint;
    endpoint.enqueue(FaultInjectingEndpoint::Action::ThrowRateLimit);
    endpoint.enqueue(FaultInjectingEndpoint::Action::ThrowRateLimit);
    endpoint.enqueue(FaultInjectingEndpoint::Action::ReturnJson429);
    endpoint.enqueue(FaultInjectingEndpoint::Action::ThrowApiError);
    endpoint.enqueue(FaultInjectingEndpoint::Action::Succeed);

    gm::Client client(&endpoint);

    try {
        (void)client.messages(make_base_messages_body());
        FAIL() << "Expected RateLimitError when custom Endpoint throws RateLimitError";
    } catch (const gm::RateLimitError& e) {
        EXPECT_NE(std::string(e.what()).find("429 quota exceeded"), std::string::npos)
            << e.what();
    }

    EXPECT_THROW(
        tr::call_gemini_batch(client, {"朝が来た。"}, {}, {}, "gemini-4-carbon", "MEDIUM"),
        gm::RateLimitError);

    EXPECT_THROW(client.messages(make_base_messages_body()), gm::RateLimitError);

    try {
        (void)client.messages(make_base_messages_body());
        FAIL() << "Expected ApiError when custom Endpoint throws ApiError";
    } catch (const gm::RateLimitError&) {
        FAIL() << "ApiError must not be misclassified as RateLimitError";
    } catch (const gm::ApiError& e) {
        EXPECT_NE(std::string(e.what()).find("Upstream gateway error"), std::string::npos)
            << e.what();
    }

    // Valid -> Failing -> Valid recovery on the same Client instance.
    const boost::json::value ok_resp = client.messages(make_base_messages_body());
    EXPECT_EQ(endpoint.calls(), kExpectedFiveEndpointCalls);
    EXPECT_EQ(gm::extract_usage(ok_resp).thought_tokens, kSampleThoughtsTokens);
}

TEST(GeminiClient, TranslateAllAbortsOnZeroThoughtWithoutMutatingCacheOrCallingNextBatch) {
    const ScopedTempDir temp("fav_ut_zero_thought_abort");
    const fs::path& dir = temp.path();

    const std::string hcb_path = (dir / "Sakura.hcb").u8string();
    const std::string input_path = (dir / "extracted_text.json").u8string();
    const std::string cache_path = (dir / "cache.json").u8string();
    const std::string output_path = (dir / "translated_text.json").u8string();
    const std::string tsv_path = (dir / "translation_table.tsv").u8string();

    fav::write_file(hcb_path, build_valid_cast_hcb());

    constexpr const char* kUncachedLines[] = {
        "桜の花びらが夜風に舞う。",
        "遠い駅のホームで鐘が鳴った。",
        "小さな影が街灯の下で揺れる。",
        "冷たい指先をそっと握りしめた。",
        "夜空の向こうに白い月が浮かぶ。",
        "誰もいない教室に時計の音が響く。",
        "約束の言葉はまだ胸に残っている。",
        "夜明け前の空が少しずつ染まっていく。",
    };

    boost::json::array extracted_lines;
    int idx = kInitialLineIndex;
    for (const char* jp_line : kUncachedLines) {
        boost::json::object entry;
        entry["idx"] = idx;
        entry["offset"] = kBaseScriptOffset + (idx * kScriptOffsetStride);
        entry["type"] = "STRING";
        entry["content"] = jp_line;
        extracted_lines.emplace_back(std::move(entry));
        ++idx;
    }

    boost::json::object script_obj;
    script_obj["lines"] = std::move(extracted_lines);
    boost::json::object root_doc;
    root_doc["Sakura.hcb"] = std::move(script_obj);
    fav::write_file(input_path, fav::json_dump(root_doc));

    const std::string initial_cache_bytes = "{\"朝が来た。\": \"Morning came.\"}";
    fav::write_file(cache_path, initial_cache_bytes);

    FakeEndpoint fake_endpoint;
    fake_endpoint.enqueue_response(make_wire_response(
        "STOP", kBatchPromptTokens, kBatchCandidatesTokens, kZeroTokens, kZeroTokens,
        "1. Cherry petals dance in the night breeze.\n"
        "2. A bell rang on the distant station platform.\n"
        "3. A small shadow sways beneath the streetlight.\n"
        "4. I gently squeezed those cold fingertips.\n"));

    tr::TranslateOptions opt;
    opt.batch_size = kCircuitBreakerBatchSize;
    opt.model = "gemini-4-carbon";
    opt.thinking_level = "MEDIUM";
    opt.endpoint = &fake_endpoint;
    opt.hcb_file = hcb_path;
    opt.input_file = input_path;
    opt.cache_file = cache_path;
    opt.output_file = output_path;
    opt.tsv_file = tsv_path;
    opt.last_translate_dir = (dir / "last_gemini_translate").u8string();

    const int exit_code = tr::translate_all(opt);
    EXPECT_EQ(exit_code, kFatalRuntimeExitCode);
    EXPECT_EQ(fake_endpoint.call_count(), kExpectedSingleApiCall);

    ASSERT_TRUE(fs::exists(fs::u8path(cache_path)));
    const fav::Bytes disk_cache = fav::read_file(cache_path);
    const std::string disk_cache_str(disk_cache.begin(), disk_cache.end());
    EXPECT_EQ(disk_cache_str, initial_cache_bytes);

    EXPECT_FALSE(fs::exists(fs::u8path(output_path)));
    EXPECT_FALSE(fs::exists(fs::u8path(tsv_path)));
}

TEST(GeminiClient, TranslateAllRefusesToCacheStrikeOneZeroThoughtAndAbortsOnStrikeTwo) {
    constexpr int kStrikeBatchSize = 16;
    constexpr int kTotalStrikeLines = 64;
    constexpr std::size_t kExpectedThreeCalls = 3u;
    constexpr std::int64_t kBatchOneThoughts = 320;

    const ScopedTempDir temp("fav_ut_zero_thought_strike_one_two");
    const fs::path& dir = temp.path();

    const std::string hcb_path = (dir / "Sakura.hcb").u8string();
    const std::string input_path = (dir / "extracted_text.json").u8string();
    const std::string cache_path = (dir / "cache.json").u8string();
    const std::string output_path = (dir / "translated_text.json").u8string();
    const std::string tsv_path = (dir / "translation_table.tsv").u8string();

    fav::write_file(hcb_path, build_valid_cast_hcb());

    std::vector<std::string> jp_lines;
    jp_lines.reserve(static_cast<std::size_t>(kTotalStrikeLines));
    boost::json::array extracted_lines;
    for (int idx = 0; idx < kTotalStrikeLines; ++idx) {
        const std::string jp = "夜空の星を数える行番号" + std::to_string(idx) + "。";
        jp_lines.push_back(jp);
        boost::json::object entry;
        entry["idx"] = idx;
        entry["offset"] = kBaseScriptOffset + (idx * kScriptOffsetStride);
        entry["type"] = "STRING";
        entry["content"] = jp;
        extracted_lines.emplace_back(std::move(entry));
    }

    boost::json::object script_obj;
    script_obj["lines"] = std::move(extracted_lines);
    boost::json::object root_doc;
    root_doc["Sakura.hcb"] = std::move(script_obj);
    fav::write_file(input_path, fav::json_dump(root_doc));

    auto make_numbered_block = [](const char* prefix, int count) {
        std::string text;
        for (int n = 1; n <= count; ++n) {
            text += std::to_string(n) + ". " + prefix + " line " + std::to_string(n) + ".\n";
        }
        return text;
    };

    FakeEndpoint fake_endpoint;
    fake_endpoint.enqueue_response(make_wire_response(
        "STOP", kBatchPromptTokens, kBatchCandidatesTokens, kBatchOneThoughts,
        kZeroTokens, make_numbered_block("Batch1", kStrikeBatchSize)));
    fake_endpoint.enqueue_response(make_wire_response(
        "STOP", kBatchPromptTokens, kBatchCandidatesTokens, kZeroTokens,
        kZeroTokens, make_numbered_block("Strike1", kStrikeBatchSize)));
    fake_endpoint.enqueue_response(make_wire_response(
        "STOP", kBatchPromptTokens, kBatchCandidatesTokens, kZeroTokens,
        kZeroTokens, make_numbered_block("Strike2", kStrikeBatchSize)));

    tr::TranslateOptions opt;
    opt.batch_size = kStrikeBatchSize;
    opt.model = "gemini-4-carbon";
    opt.thinking_level = "MEDIUM";
    opt.endpoint = &fake_endpoint;
    opt.hcb_file = hcb_path;
    opt.input_file = input_path;
    opt.cache_file = cache_path;
    opt.output_file = output_path;
    opt.tsv_file = tsv_path;
    opt.last_translate_dir = (dir / "last_gemini_translate").u8string();

    EXPECT_EQ(tr::translate_all(opt), kFatalRuntimeExitCode);
    EXPECT_EQ(fake_endpoint.call_count(), kExpectedThreeCalls);

    const tr::Cache saved = tr::load_cache(cache_path);
    for (int idx = 0; idx < kStrikeBatchSize; ++idx) {
        const std::string* en = saved.get(jp_lines[static_cast<std::size_t>(idx)]);
        ASSERT_NE(en, nullptr) << "Batch 1 line " << idx << " must survive on disk";
        EXPECT_EQ(*en, "Batch1 line " + std::to_string(idx + 1) + ".");
    }
    for (int idx = kStrikeBatchSize; idx < kTotalStrikeLines; ++idx) {
        EXPECT_FALSE(saved.contains(jp_lines[static_cast<std::size_t>(idx)]))
            << "0-thought Strike 1/2 line " << idx << " must NEVER enter cache";
    }
}

TEST(GeminiClient, TranslateAllBisectsRefusedBatchAndRescuesSiblingsWithoutTrippingBreaker) {
    constexpr std::size_t kExpectedSixBisectCalls = 6u;
    constexpr int kSuccessExitCode = 0;

    const ScopedTempDir temp("fav_ut_refusal_bisection");
    const fs::path& dir = temp.path();

    const std::string hcb_path = (dir / "Sakura.hcb").u8string();
    const std::string input_path = (dir / "extracted_text.json").u8string();
    const std::string cache_path = (dir / "cache.json").u8string();
    const std::string output_path = (dir / "translated_text.json").u8string();
    const std::string tsv_path = (dir / "translation_table.tsv").u8string();

    fav::write_file(hcb_path, build_valid_cast_hcb());

    constexpr const char* kBatchLines[] = {
        "窓の外で桜が揺れている。",
        "静かな夜の風が頬を撫でた。",
        "二人の影が廊下に重なった。",
        "禁則対象の台詞がここにある。",
    };

    boost::json::array extracted_lines;
    for (int idx = 0; idx < kCircuitBreakerBatchSize; ++idx) {
        boost::json::object entry;
        entry["idx"] = idx;
        entry["offset"] = kBaseScriptOffset + (idx * kScriptOffsetStride);
        entry["type"] = "STRING";
        entry["content"] = kBatchLines[idx];
        extracted_lines.emplace_back(std::move(entry));
    }

    boost::json::object script_obj;
    script_obj["lines"] = std::move(extracted_lines);
    boost::json::object root_doc;
    root_doc["Sakura.hcb"] = std::move(script_obj);
    fav::write_file(input_path, fav::json_dump(root_doc));

    FakeEndpoint fake_endpoint;
    // 1. Top-level 4 lines -> refusal
    fake_endpoint.enqueue_response(make_wire_response(
        "PROHIBITED_CONTENT", kBatchPromptTokens, kZeroTokens, kZeroTokens, kZeroTokens, ""));
    // 2. Neutral 4 lines -> refusal
    fake_endpoint.enqueue_response(make_wire_response(
        "PROHIBITED_CONTENT", kBatchPromptTokens, kZeroTokens, kZeroTokens, kZeroTokens, ""));
    // 3. Left half (2 lines) -> succeeds even with 0 thoughts (!is_top_level)
    fake_endpoint.enqueue_response(make_wire_response(
        "STOP", kBatchPromptTokens, kBatchCandidatesTokens, kZeroTokens, kZeroTokens,
        "1. Cherry blossoms sway outside the window.\n"
        "2. A quiet night breeze brushed my cheek.\n"));
    // 4. Right half (2 lines) -> refusal
    fake_endpoint.enqueue_response(make_wire_response(
        "PROHIBITED_CONTENT", kBatchPromptTokens, kZeroTokens, kZeroTokens, kZeroTokens, ""));
    // 5. Right-left leaf (1 line) -> succeeds
    fake_endpoint.enqueue_response(make_wire_response(
        "STOP", kBatchPromptTokens, kBatchCandidatesTokens, kZeroTokens, kZeroTokens,
        "1. Our shadows overlapped in the hallway.\n"));
    // 6. Right-right leaf (1 line) -> hard refusal at size 1
    fake_endpoint.enqueue_response(make_wire_response(
        "PROHIBITED_CONTENT", kBatchPromptTokens, kZeroTokens, kZeroTokens, kZeroTokens, ""));

    tr::TranslateOptions opt;
    opt.batch_size = kCircuitBreakerBatchSize;
    opt.model = "gemini-4-carbon";
    opt.thinking_level = "MEDIUM";
    opt.endpoint = &fake_endpoint;
    opt.hcb_file = hcb_path;
    opt.input_file = input_path;
    opt.cache_file = cache_path;
    opt.output_file = output_path;
    opt.tsv_file = tsv_path;
    opt.last_translate_dir = (dir / "last_gemini_translate").u8string();

    EXPECT_EQ(tr::translate_all(opt), kSuccessExitCode);
    EXPECT_EQ(fake_endpoint.call_count(), kExpectedSixBisectCalls);

    const tr::Cache saved = tr::load_cache(cache_path);
    ASSERT_NE(saved.get(kBatchLines[0]), nullptr);
    EXPECT_EQ(*saved.get(kBatchLines[0]), "Cherry blossoms sway outside the window.");
    ASSERT_NE(saved.get(kBatchLines[1]), nullptr);
    EXPECT_EQ(*saved.get(kBatchLines[1]), "A quiet night breeze brushed my cheek.");
    ASSERT_NE(saved.get(kBatchLines[2]), nullptr);
    EXPECT_EQ(*saved.get(kBatchLines[2]), "Our shadows overlapped in the hallway.");
    EXPECT_FALSE(saved.contains(kBatchLines[3]));
}

TEST(GeminiClient, TranslateAllStopsCleanlyWhenMaxBudgetEurReached) {
    constexpr int kSuccessExitCode = 0;
    constexpr std::int64_t kHighPromptTokens = 5000;
    constexpr std::int64_t kHighCandidateTokens = 1000;
    constexpr std::int64_t kHighThoughtTokens = 1000;
    constexpr double kTightBudgetEur = 0.01;

    const ScopedTempDir temp("fav_ut_budget_cap");
    const fs::path& dir = temp.path();

    const std::string hcb_path = (dir / "Sakura.hcb").u8string();
    const std::string input_path = (dir / "extracted_text.json").u8string();
    const std::string cache_path = (dir / "cache.json").u8string();
    const std::string output_path = (dir / "translated_text.json").u8string();
    const std::string tsv_path = (dir / "translation_table.tsv").u8string();

    fav::write_file(hcb_path, build_valid_cast_hcb());

    constexpr const char* kEightLines[] = {
        "春の風が吹いている。",
        "駅前の時計が鳴った。",
        "白い雲が空を流れる。",
        "靴音が静かに遠ざかる。",
        "夕暮れの光が窓を染める。",
        "本棚の影が長く伸びた。",
        "鍵を回す音が響いた。",
        "夜空に一番星が見えた。",
    };

    boost::json::array extracted_lines;
    for (int idx = 0; idx < 8; ++idx) {
        boost::json::object entry;
        entry["idx"] = idx;
        entry["offset"] = kBaseScriptOffset + (idx * kScriptOffsetStride);
        entry["type"] = "STRING";
        entry["content"] = kEightLines[idx];
        extracted_lines.emplace_back(std::move(entry));
    }

    boost::json::object script_obj;
    script_obj["lines"] = std::move(extracted_lines);
    boost::json::object root_doc;
    root_doc["Sakura.hcb"] = std::move(script_obj);
    fav::write_file(input_path, fav::json_dump(root_doc));

    FakeEndpoint fake_endpoint;
    fake_endpoint.enqueue_response(make_wire_response(
        "STOP", kHighPromptTokens, kHighCandidateTokens, kHighThoughtTokens, kZeroTokens,
        "1. The spring breeze is blowing.\n"
        "2. The station clock chimed.\n"
        "3. White clouds drift across the sky.\n"
        "4. Footsteps fade quietly away.\n"));

    tr::TranslateOptions opt;
    opt.batch_size = kCircuitBreakerBatchSize;
    opt.max_budget_eur = kTightBudgetEur;
    opt.model = "gemini-4-carbon";
    opt.thinking_level = "MEDIUM";
    opt.endpoint = &fake_endpoint;
    opt.hcb_file = hcb_path;
    opt.input_file = input_path;
    opt.cache_file = cache_path;
    opt.output_file = output_path;
    opt.tsv_file = tsv_path;
    opt.last_translate_dir = (dir / "last_gemini_translate").u8string();

    EXPECT_EQ(tr::translate_all(opt), kSuccessExitCode);
    EXPECT_EQ(fake_endpoint.call_count(), kExpectedSingleApiCall);

    const tr::Cache saved = tr::load_cache(cache_path);
    ASSERT_NE(saved.get(kEightLines[0]), nullptr);
    EXPECT_EQ(*saved.get(kEightLines[0]), "The spring breeze is blowing.");
    EXPECT_FALSE(saved.contains(kEightLines[4]));
}
