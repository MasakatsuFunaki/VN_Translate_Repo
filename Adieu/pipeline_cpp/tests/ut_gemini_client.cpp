// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for the Gemini generateContent request builder, response adapter,
// and header insulation contracts declared in translate/gemini_client.h.
#include "translate/gemini_client.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINSOCKAPI_) || \
    defined(OPENSSL_VERSION_NUMBER)
#error "translate/gemini_client.h must stay insulated from Windows, Winsock, and OpenSSL headers"
#endif

#include "vn/llm/llm_client.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace gem = adi::gemini;

static_assert(std::is_abstract_v<gem::IMessageClient>);
static_assert(!std::is_copy_constructible_v<gem::IMessageClient>);
static_assert(!std::is_copy_assignable_v<gem::IMessageClient>);

static_assert(std::is_base_of_v<gem::IMessageClient, gem::Client>);
static_assert(!std::is_copy_constructible_v<gem::Client>);
static_assert(!std::is_copy_assignable_v<gem::Client>);
static_assert(std::is_nothrow_move_constructible_v<gem::Client>);
static_assert(std::is_nothrow_move_assignable_v<gem::Client>);

static_assert(std::is_base_of_v<gem::IMessageClient, gem::LazyClient>);
static_assert(!std::is_copy_constructible_v<gem::LazyClient>);
static_assert(!std::is_copy_assignable_v<gem::LazyClient>);
static_assert(std::is_nothrow_move_constructible_v<gem::LazyClient>);
static_assert(std::is_nothrow_move_assignable_v<gem::LazyClient>);

static_assert(std::is_base_of_v<std::runtime_error, gem::RateLimitError>);
static_assert(std::is_base_of_v<std::runtime_error, gem::ApiError>);
static_assert(!std::is_base_of_v<gem::ApiError, gem::RateLimitError>);
static_assert(!std::is_base_of_v<gem::RateLimitError, gem::ApiError>);

namespace {

constexpr const char* kDefaultModelId = "gemini-4-carbon";

constexpr std::size_t kEmptySize = 0u;
constexpr std::size_t kSingleItemSize = 1u;
constexpr std::size_t kSafetySettingFieldCount = 2u;
constexpr std::size_t kExpectedHarmCategoryCount = 5u;
constexpr std::size_t kExpectedTurnCount = 4u;
constexpr std::size_t kExpectedMultimodalPartCount = 3u;

constexpr std::size_t kFirstIndex = 0u;
constexpr std::size_t kSecondIndex = 1u;
constexpr std::size_t kThirdIndex = 2u;
constexpr std::size_t kFourthIndex = 3u;

constexpr std::int64_t kZeroTokens = 0;
constexpr std::int64_t kNegativeTokens = -1;
constexpr std::int64_t kBelowMinMaxOutputTokens = 16383;
constexpr std::int64_t kMinMaxOutputTokens = 16384;
constexpr std::int64_t kInteriorMaxOutputTokens = 32768;
constexpr std::int64_t kMaxMaxOutputTokens = 65536;
constexpr std::int64_t kAboveMaxMaxOutputTokens = 65537;

constexpr std::uint64_t kUint64BelowMinTokens = 4096u;
constexpr std::uint64_t kUint64InteriorTokens = 49152u;
constexpr std::uint64_t kUint64AboveMaxTokens = 131072u;

constexpr std::int64_t kNonStringModelNumber = 42;
constexpr std::int64_t kLegacyThinkingBudget = 8192;

constexpr double kSampleTemperatureDouble = 0.35;
constexpr std::int64_t kSampleTemperatureInt64 = 0;
constexpr std::uint64_t kSampleTemperatureUint64 = 1u;
constexpr double kExpectedTemperatureInt64AsDouble = 0.0;
constexpr double kExpectedTemperatureUint64AsDouble = 1.0;

constexpr std::int64_t kSamplePromptTokens = 120;
constexpr std::int64_t kSampleCandidatesTokens = 45;
constexpr std::int64_t kSampleThoughtsTokens = 310;
constexpr std::int64_t kExpectedCombinedOutputTokens = 355;
constexpr std::int64_t kSampleTotalTokens = 475;
constexpr std::int64_t kSampleCachedTokens = 80;

constexpr std::int64_t kPartialPromptTokens = 50;
constexpr std::int64_t kPartialCandidatesTokens = 19;

constexpr std::array<const char*, kExpectedHarmCategoryCount> kRequiredHarmCategories = {
    "HARM_CATEGORY_HARASSMENT",
    "HARM_CATEGORY_HATE_SPEECH",
    "HARM_CATEGORY_SEXUALLY_EXPLICIT",
    "HARM_CATEGORY_DANGEROUS_CONTENT",
    "HARM_CATEGORY_CIVIC_INTEGRITY",
};

void verify_thinking_config(const boost::json::object& gen_cfg) {
    ASSERT_TRUE(gen_cfg.contains("thinkingConfig"));
    ASSERT_TRUE(gen_cfg.at("thinkingConfig").is_object());
    const auto& thinking = gen_cfg.at("thinkingConfig").as_object();
    EXPECT_FALSE(thinking.contains("thinkingBudget"));

    boost::json::object expected_thinking;
    expected_thinking["thinkingLevel"] = "MEDIUM";
    EXPECT_EQ(thinking, expected_thinking);
}

void verify_safety_settings(const boost::json::object& payload) {
    ASSERT_TRUE(payload.contains("safetySettings"));
    ASSERT_TRUE(payload.at("safetySettings").is_array());
    const auto& safety = payload.at("safetySettings").as_array();
    ASSERT_EQ(safety.size(), kExpectedHarmCategoryCount);

    std::map<std::string, std::string> thresholds_by_category;
    for (const auto& item : safety) {
        ASSERT_TRUE(item.is_object());
        const auto& obj = item.as_object();
        ASSERT_EQ(obj.size(), kSafetySettingFieldCount);
        ASSERT_TRUE(obj.contains("category"));
        ASSERT_TRUE(obj.at("category").is_string());
        ASSERT_TRUE(obj.contains("threshold"));
        ASSERT_TRUE(obj.at("threshold").is_string());
        thresholds_by_category.emplace(
            std::string(obj.at("category").as_string()),
            std::string(obj.at("threshold").as_string()));
    }

    ASSERT_EQ(thresholds_by_category.size(), kExpectedHarmCategoryCount);
    for (const char* category : kRequiredHarmCategories) {
        SCOPED_TRACE(category);
        const auto it = thresholds_by_category.find(category);
        ASSERT_NE(it, thresholds_by_category.end());
        EXPECT_EQ(it->second, "BLOCK_NONE");
    }
}

void verify_usage_counts(const boost::json::value& adapted,
                         std::int64_t expected_input,
                         std::int64_t expected_output,
                         std::int64_t expected_thought,
                         std::int64_t expected_cache_read,
                         std::int64_t expected_cache_creation) {
    ASSERT_TRUE(adapted.is_object());
    const auto& root = adapted.as_object();
    ASSERT_TRUE(root.contains("usage"));
    ASSERT_TRUE(root.at("usage").is_object());
    const auto& usage = root.at("usage").as_object();

    ASSERT_TRUE(usage.contains("input_tokens"));
    ASSERT_TRUE(usage.at("input_tokens").is_int64());
    EXPECT_EQ(usage.at("input_tokens").as_int64(), expected_input);

    ASSERT_TRUE(usage.contains("output_tokens"));
    ASSERT_TRUE(usage.at("output_tokens").is_int64());
    EXPECT_EQ(usage.at("output_tokens").as_int64(), expected_output);

    ASSERT_TRUE(usage.contains("thought_tokens"));
    ASSERT_TRUE(usage.at("thought_tokens").is_int64());
    EXPECT_EQ(usage.at("thought_tokens").as_int64(), expected_thought);

    ASSERT_TRUE(usage.contains("cache_read_input_tokens"));
    ASSERT_TRUE(usage.at("cache_read_input_tokens").is_int64());
    EXPECT_EQ(usage.at("cache_read_input_tokens").as_int64(), expected_cache_read);

    ASSERT_TRUE(usage.contains("cache_creation_input_tokens"));
    ASSERT_TRUE(usage.at("cache_creation_input_tokens").is_int64());
    EXPECT_EQ(usage.at("cache_creation_input_tokens").as_int64(),
              expected_cache_creation);
}

enum class ModelFieldKind {
    Omitted,
    StringValue,
    IntegerValue,
};

struct ModelRoutingCase {
    const char* label;
    ModelFieldKind kind;
    const char* model_str;
    const char* expected_model_id;
};

boost::json::object make_model_routing_body(const ModelRoutingCase& tc) {
    boost::json::object body;
    if (tc.kind == ModelFieldKind::StringValue) {
        body["model"] = tc.model_str;
        return body;
    }
    if (tc.kind == ModelFieldKind::IntegerValue) {
        body["model"] = kNonStringModelNumber;
        return body;
    }
    return body;
}

enum class MaxTokensKind {
    Omitted,
    SignedInt,
    UnsignedInt,
    NonNumericString,
};

struct MaxTokensCase {
    const char* label;
    MaxTokensKind kind;
    std::int64_t signed_val;
    std::uint64_t unsigned_val;
    std::int64_t expected_max_output_tokens;
};

boost::json::object make_max_tokens_body(const MaxTokensCase& tc) {
    boost::json::object body;
    if (tc.kind == MaxTokensKind::SignedInt) {
        body["max_tokens"] = tc.signed_val;
        return body;
    }
    if (tc.kind == MaxTokensKind::UnsignedInt) {
        body["max_tokens"] = tc.unsigned_val;
        return body;
    }
    if (tc.kind == MaxTokensKind::NonNumericString) {
        body["max_tokens"] = "32768";
        return body;
    }
    return body;
}

enum class CandidatesPresence {
    Absent,
    EmptyArray,
    SingleCandidate,
};

struct StopReasonCase {
    const char* label;
    CandidatesPresence candidates_presence;
    const char* finish_reason;
    const char* prompt_block_reason;
    const char* expected_stop_reason;
};

boost::json::object make_stop_reason_response(const StopReasonCase& tc) {
    boost::json::object raw;
    if (tc.candidates_presence == CandidatesPresence::EmptyArray) {
        raw["candidates"] = boost::json::array{};
    }
    if (tc.candidates_presence == CandidatesPresence::SingleCandidate) {
        boost::json::object text_part;
        text_part["text"] = "Translated line.";
        boost::json::array parts;
        parts.emplace_back(std::move(text_part));

        boost::json::object content;
        content["parts"] = std::move(parts);

        boost::json::object candidate;
        candidate["content"] = std::move(content);
        if (tc.finish_reason != nullptr) {
            candidate["finishReason"] = tc.finish_reason;
        }

        boost::json::array candidates;
        candidates.emplace_back(std::move(candidate));
        raw["candidates"] = std::move(candidates);
    }

    if (tc.prompt_block_reason != nullptr) {
        boost::json::object feedback;
        feedback["blockReason"] = tc.prompt_block_reason;
        raw["promptFeedback"] = std::move(feedback);
    }
    return raw;
}

}  // namespace

TEST(GeminiClient, BuildRequestRoutesModelAndHandlesNonObjectBody) {
    constexpr ModelRoutingCase kModelCases[] = {
        {"omitted_model", ModelFieldKind::Omitted, nullptr, kDefaultModelId},
        {"empty_model_string", ModelFieldKind::StringValue, "", kDefaultModelId},
        {"non_gemini_vendor", ModelFieldKind::StringValue, "claude-sonnet-4-5", kDefaultModelId},
        {"missing_dash_prefix_seam", ModelFieldKind::StringValue, "gemini", kDefaultModelId},
        {"interior_substring_not_prefix", ModelFieldKind::StringValue, "x-gemini-2.5-pro", kDefaultModelId},
        {"uppercase_prefix_mismatch", ModelFieldKind::StringValue, "GEMINI-2.5-pro", kDefaultModelId},
        {"valid_gemini_prefix", ModelFieldKind::StringValue, "gemini-2.5-pro", "gemini-2.5-pro"},
        {"non_string_model_field", ModelFieldKind::IntegerValue, nullptr, kDefaultModelId},
    };
    static_assert(std::size(kModelCases) > kEmptySize);

    for (const auto& tc : kModelCases) {
        SCOPED_TRACE(tc.label);
        const auto [model_id, payload] =
            gem::build_gemini_request(make_model_routing_body(tc));
        EXPECT_EQ(model_id, tc.expected_model_id);
        ASSERT_TRUE(payload.contains("contents"));
        ASSERT_TRUE(payload.at("contents").is_array());
        EXPECT_EQ(payload.at("contents").as_array().size(), kEmptySize);
    }

    boost::json::array non_object_array;
    non_object_array.emplace_back("unexpected");
    const boost::json::value non_object_inputs[] = {
        boost::json::value(nullptr),
        std::move(non_object_array),
    };

    for (const auto& non_obj : non_object_inputs) {
        const auto [model_id, payload] = gem::build_gemini_request(non_obj);
        EXPECT_EQ(model_id, kDefaultModelId);
        EXPECT_FALSE(payload.contains("systemInstruction"));

        ASSERT_TRUE(payload.contains("contents"));
        ASSERT_TRUE(payload.at("contents").is_array());
        EXPECT_EQ(payload.at("contents").as_array().size(), kEmptySize);

        ASSERT_TRUE(payload.contains("generationConfig"));
        ASSERT_TRUE(payload.at("generationConfig").is_object());
        const auto& gen_cfg = payload.at("generationConfig").as_object();
        EXPECT_FALSE(gen_cfg.contains("temperature"));
        ASSERT_TRUE(gen_cfg.contains("maxOutputTokens"));
        ASSERT_TRUE(gen_cfg.at("maxOutputTokens").is_int64() ||
                    gen_cfg.at("maxOutputTokens").is_uint64());
        EXPECT_EQ(gen_cfg.at("maxOutputTokens").to_number<std::int64_t>(),
                  kMinMaxOutputTokens);
        ASSERT_NO_FATAL_FAILURE(verify_thinking_config(gen_cfg));
        ASSERT_NO_FATAL_FAILURE(verify_safety_settings(payload));
    }
}

TEST(GeminiClient, BuildRequestSetsMediumThinkingLevelAndFiveUnblockedSafetyCategories) {
    boost::json::object legacy_thinking;
    legacy_thinking["type"] = "enabled";
    legacy_thinking["budget_tokens"] = kLegacyThinkingBudget;

    boost::json::object body_with_legacy_thinking;
    body_with_legacy_thinking["thinking"] = std::move(legacy_thinking);

    const boost::json::object bodies[] = {
        boost::json::object{},
        std::move(body_with_legacy_thinking),
    };

    for (const auto& body : bodies) {
        const auto [model_id, payload] = gem::build_gemini_request(body);
        EXPECT_EQ(model_id, kDefaultModelId);
        ASSERT_TRUE(payload.contains("generationConfig"));
        ASSERT_TRUE(payload.at("generationConfig").is_object());
        ASSERT_NO_FATAL_FAILURE(
            verify_thinking_config(payload.at("generationConfig").as_object()));
        ASSERT_NO_FATAL_FAILURE(verify_safety_settings(payload));
    }
}

TEST(GeminiClient, BuildRequestAndClientRejectDisallowedArgonAndEmbeddedNulModels) {
    struct DisallowedModelCase {
        const char* label;
        std::string_view model;
    };

    static constexpr char kEmbeddedNulModel[] = "gemini-4-carbon\0extra";
    constexpr DisallowedModelCase kDisallowedCases[] = {
        {"canonical_gemini_4_argon", "gemini-4-argon"},
        {"uppercase_argon_not_silently_rewritten", "GEMINI-4-ARGON"},
        {"prefixed_argon_not_silently_rewritten", "models/gemini-4-argon-preview"},
        {"embedded_nul_byte_seam",
         std::string_view(kEmbeddedNulModel, sizeof(kEmbeddedNulModel) - 1u)},
    };
    static_assert(std::size(kDisallowedCases) > kEmptySize);

    gem::Client client("fake-offline-key");
    for (const auto& tc : kDisallowedCases) {
        SCOPED_TRACE(tc.label);
        boost::json::object body;
        body["model"] = boost::json::string(tc.model.data(), tc.model.size());
        EXPECT_THROW(gem::build_gemini_request(body), gem::ApiError);
        EXPECT_THROW(client.messages(body), gem::ApiError);
    }

    // Valid -> Failing -> Valid seam: rejecting a disallowed model leaves request
    // building intact for subsequent supported Gemini 4 calls.
    boost::json::object valid_body;
    valid_body["model"] = "gemini-4-carbon";
    const auto [resolved_after_failure, valid_payload] =
        gem::build_gemini_request(valid_body);
    EXPECT_EQ(resolved_after_failure, kDefaultModelId);
    ASSERT_NO_FATAL_FAILURE(
        verify_thinking_config(valid_payload.at("generationConfig").as_object()));
}

TEST(GeminiClient, BuildRequestClampsMaxOutputTokensAcrossBoundaries) {
    constexpr MaxTokensCase kCases[] = {
        {"omitted_defaults_to_floor", MaxTokensKind::Omitted, kZeroTokens, kEmptySize, kMinMaxOutputTokens},
        {"zero_clamps_to_floor", MaxTokensKind::SignedInt, kZeroTokens, kEmptySize, kMinMaxOutputTokens},
        {"negative_one_clamps_to_floor", MaxTokensKind::SignedInt, kNegativeTokens, kEmptySize, kMinMaxOutputTokens},
        {"floor_minus_one_16383", MaxTokensKind::SignedInt, kBelowMinMaxOutputTokens, kEmptySize, kMinMaxOutputTokens},
        {"exact_floor_16384", MaxTokensKind::SignedInt, kMinMaxOutputTokens, kEmptySize, kMinMaxOutputTokens},
        {"interior_32768", MaxTokensKind::SignedInt, kInteriorMaxOutputTokens, kEmptySize, kInteriorMaxOutputTokens},
        {"exact_ceiling_65536", MaxTokensKind::SignedInt, kMaxMaxOutputTokens, kEmptySize, kMaxMaxOutputTokens},
        {"ceiling_plus_one_65537", MaxTokensKind::SignedInt, kAboveMaxMaxOutputTokens, kEmptySize, kMaxMaxOutputTokens},
        {"uint64_below_floor", MaxTokensKind::UnsignedInt, kZeroTokens, kUint64BelowMinTokens, kMinMaxOutputTokens},
        {"uint64_interior", MaxTokensKind::UnsignedInt, kZeroTokens, kUint64InteriorTokens, static_cast<std::int64_t>(kUint64InteriorTokens)},
        {"uint64_above_ceiling", MaxTokensKind::UnsignedInt, kZeroTokens, kUint64AboveMaxTokens, kMaxMaxOutputTokens},
        {"non_numeric_string_defaults_to_floor", MaxTokensKind::NonNumericString, kZeroTokens, kEmptySize, kMinMaxOutputTokens},
    };
    static_assert(std::size(kCases) > kEmptySize);

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        const auto [model_id, payload] =
            gem::build_gemini_request(make_max_tokens_body(tc));
        EXPECT_EQ(model_id, kDefaultModelId);
        ASSERT_TRUE(payload.contains("generationConfig"));
        ASSERT_TRUE(payload.at("generationConfig").is_object());
        const auto& gen_cfg = payload.at("generationConfig").as_object();
        ASSERT_TRUE(gen_cfg.contains("maxOutputTokens"));
        const auto& max_out = gen_cfg.at("maxOutputTokens");
        ASSERT_TRUE(max_out.is_int64() || max_out.is_uint64());
        EXPECT_EQ(max_out.to_number<std::int64_t>(), tc.expected_max_output_tokens);
    }
}

TEST(GeminiClient, BuildRequestMapsSystemInstructionAndTemperature) {
    {
        boost::json::object body;
        body["system"] = "Translate SiglusEngine NVL prose faithfully.";
        body["temperature"] = kSampleTemperatureDouble;
        const auto [model_id, payload] = gem::build_gemini_request(body);
        EXPECT_EQ(model_id, kDefaultModelId);

        boost::json::object expected_part;
        expected_part["text"] = "Translate SiglusEngine NVL prose faithfully.";
        boost::json::array expected_parts;
        expected_parts.emplace_back(std::move(expected_part));
        boost::json::object expected_sys;
        expected_sys["parts"] = std::move(expected_parts);

        ASSERT_TRUE(payload.contains("systemInstruction"));
        EXPECT_EQ(payload.at("systemInstruction"), boost::json::value(expected_sys));

        const auto& gen_cfg = payload.at("generationConfig").as_object();
        ASSERT_TRUE(gen_cfg.contains("temperature"));
        ASSERT_TRUE(gen_cfg.at("temperature").is_number());
        EXPECT_DOUBLE_EQ(gen_cfg.at("temperature").to_number<double>(),
                         kSampleTemperatureDouble);
    }

    {
        boost::json::object sys_a;
        sys_a["type"] = "text";
        sys_a["text"] = "Preserve speaker voice.";
        boost::json::object sys_b;
        sys_b["type"] = "text";
        sys_b["text"] = "Keep NVL line pacing.";
        boost::json::array sys_blocks;
        sys_blocks.emplace_back(std::move(sys_a));
        sys_blocks.emplace_back(std::move(sys_b));

        boost::json::object body;
        body["system"] = std::move(sys_blocks);
        body["temperature"] = kSampleTemperatureInt64;
        const auto [model_id, payload] = gem::build_gemini_request(body);
        EXPECT_EQ(model_id, kDefaultModelId);

        boost::json::object expected_part;
        expected_part["text"] = "Preserve speaker voice.\nKeep NVL line pacing.";
        boost::json::array expected_parts;
        expected_parts.emplace_back(std::move(expected_part));
        boost::json::object expected_sys;
        expected_sys["parts"] = std::move(expected_parts);

        ASSERT_TRUE(payload.contains("systemInstruction"));
        EXPECT_EQ(payload.at("systemInstruction"), boost::json::value(expected_sys));

        const auto& gen_cfg = payload.at("generationConfig").as_object();
        ASSERT_TRUE(gen_cfg.contains("temperature"));
        ASSERT_TRUE(gen_cfg.at("temperature").is_number());
        EXPECT_DOUBLE_EQ(gen_cfg.at("temperature").to_number<double>(),
                         kExpectedTemperatureInt64AsDouble);
    }

    {
        boost::json::object body;
        body["temperature"] = kSampleTemperatureUint64;
        const auto [model_id, payload] = gem::build_gemini_request(body);
        EXPECT_EQ(model_id, kDefaultModelId);
        EXPECT_FALSE(payload.contains("systemInstruction"));

        const auto& gen_cfg = payload.at("generationConfig").as_object();
        ASSERT_TRUE(gen_cfg.contains("temperature"));
        ASSERT_TRUE(gen_cfg.at("temperature").is_number());
        EXPECT_DOUBLE_EQ(gen_cfg.at("temperature").to_number<double>(),
                         kExpectedTemperatureUint64AsDouble);
    }

    boost::json::object empty_text_block;
    empty_text_block["type"] = "text";
    empty_text_block["text"] = "";
    boost::json::array empty_text_array;
    empty_text_array.emplace_back(std::move(empty_text_block));

    boost::json::object empty_string_sys;
    empty_string_sys["system"] = "";
    empty_string_sys["temperature"] = "0.35";

    boost::json::object empty_array_sys;
    empty_array_sys["system"] = boost::json::array{};
    empty_array_sys["temperature"] = true;

    boost::json::object blank_part_sys;
    blank_part_sys["system"] = std::move(empty_text_array);
    blank_part_sys["temperature"] = nullptr;

    const boost::json::object omitted_cases[] = {
        std::move(empty_string_sys),
        std::move(empty_array_sys),
        std::move(blank_part_sys),
    };

    for (const auto& body : omitted_cases) {
        const auto [model_id, payload] = gem::build_gemini_request(body);
        EXPECT_EQ(model_id, kDefaultModelId);
        EXPECT_FALSE(payload.contains("systemInstruction"));
        const auto& gen_cfg = payload.at("generationConfig").as_object();
        EXPECT_FALSE(gen_cfg.contains("temperature"));
    }
}

TEST(GeminiClient, BuildRequestMapsRolesAndMultimodalContentBlocks) {
    boost::json::object lead_text;
    lead_text["type"] = "text";
    lead_text["text"] = "Read this menu button:";

    boost::json::object image_source;
    image_source["type"] = "base64";
    image_source["media_type"] = "image/png";
    image_source["data"] = "iVBORw0KGgo=";

    boost::json::object image_block;
    image_block["type"] = "image";
    image_block["source"] = std::move(image_source);

    boost::json::object tail_text;
    tail_text["type"] = "text";
    tail_text["text"] = "Return concise English.";

    boost::json::array user_blocks;
    user_blocks.emplace_back(std::move(lead_text));
    user_blocks.emplace_back(std::move(image_block));
    user_blocks.emplace_back(std::move(tail_text));

    boost::json::object turn_multimodal;
    turn_multimodal["role"] = "user";
    turn_multimodal["content"] = std::move(user_blocks);

    boost::json::object turn_assistant;
    turn_assistant["role"] = "assistant";
    turn_assistant["content"] = "Save / Load";

    boost::json::object turn_user_string;
    turn_user_string["role"] = "user";
    turn_user_string["content"] = "さよならを教えて";

    boost::json::object turn_other_role;
    turn_other_role["role"] = "human";
    turn_other_role["content"] = "人見";

    boost::json::array messages;
    messages.emplace_back(std::move(turn_multimodal));
    messages.emplace_back(std::move(turn_assistant));
    messages.emplace_back(std::move(turn_user_string));
    messages.emplace_back(std::move(turn_other_role));

    boost::json::object body;
    body["messages"] = std::move(messages);

    const auto [model_id, payload] = gem::build_gemini_request(body);
    EXPECT_EQ(model_id, kDefaultModelId);
    ASSERT_TRUE(payload.contains("contents"));
    ASSERT_TRUE(payload.at("contents").is_array());
    const auto& contents = payload.at("contents").as_array();
    ASSERT_EQ(contents.size(), kExpectedTurnCount);

    const auto& c0 = contents[kFirstIndex].as_object();
    EXPECT_EQ(std::string(c0.at("role").as_string()), "user");
    const auto& c0_parts = c0.at("parts").as_array();
    ASSERT_EQ(c0_parts.size(), kExpectedMultimodalPartCount);

    boost::json::object expected_p0;
    expected_p0["text"] = "Read this menu button:";
    EXPECT_EQ(c0_parts[kFirstIndex], boost::json::value(expected_p0));

    boost::json::object expected_inline;
    expected_inline["mimeType"] = "image/png";
    expected_inline["data"] = "iVBORw0KGgo=";
    boost::json::object expected_p1;
    expected_p1["inlineData"] = std::move(expected_inline);
    EXPECT_EQ(c0_parts[kSecondIndex], boost::json::value(expected_p1));

    boost::json::object expected_p2;
    expected_p2["text"] = "Return concise English.";
    EXPECT_EQ(c0_parts[kThirdIndex], boost::json::value(expected_p2));

    const auto& c1 = contents[kSecondIndex].as_object();
    EXPECT_EQ(std::string(c1.at("role").as_string()), "model");
    const auto& c1_parts = c1.at("parts").as_array();
    ASSERT_EQ(c1_parts.size(), kSingleItemSize);
    boost::json::object expected_c1_part;
    expected_c1_part["text"] = "Save / Load";
    EXPECT_EQ(c1_parts[kFirstIndex], boost::json::value(expected_c1_part));

    const auto& c2 = contents[kThirdIndex].as_object();
    EXPECT_EQ(std::string(c2.at("role").as_string()), "user");
    const auto& c2_parts = c2.at("parts").as_array();
    ASSERT_EQ(c2_parts.size(), kSingleItemSize);
    boost::json::object expected_c2_part;
    expected_c2_part["text"] = "さよならを教えて";
    EXPECT_EQ(c2_parts[kFirstIndex], boost::json::value(expected_c2_part));

    const auto& c3 = contents[kFourthIndex].as_object();
    EXPECT_EQ(std::string(c3.at("role").as_string()), "user");
    const auto& c3_parts = c3.at("parts").as_array();
    ASSERT_EQ(c3_parts.size(), kSingleItemSize);
    boost::json::object expected_c3_part;
    expected_c3_part["text"] = "人見";
    EXPECT_EQ(c3_parts[kFirstIndex], boost::json::value(expected_c3_part));
}

TEST(GeminiClient, AdaptResponseFiltersThoughtPartsAndConcatenatesVisibleTextInOrder) {
    boost::json::object part_thought_a;
    part_thought_a["text"] = "Drafting internal reasoning...";
    part_thought_a["thought"] = true;

    boost::json::object part_visible_a;
    part_visible_a["text"] = "Good morning, ";
    part_visible_a["thought"] = false;

    boost::json::object part_thought_b;
    part_thought_b["text"] = "Verifying protagonist name...";
    part_thought_b["thought"] = true;

    boost::json::object part_visible_b;
    part_visible_b["text"] = "Hitomi.";

    boost::json::array parts;
    parts.emplace_back(std::move(part_thought_a));
    parts.emplace_back(std::move(part_visible_a));
    parts.emplace_back(std::move(part_thought_b));
    parts.emplace_back(std::move(part_visible_b));

    boost::json::object content;
    content["role"] = "model";
    content["parts"] = std::move(parts);

    boost::json::object candidate;
    candidate["content"] = std::move(content);
    candidate["finishReason"] = "STOP";

    boost::json::array candidates;
    candidates.emplace_back(std::move(candidate));

    boost::json::object raw;
    raw["candidates"] = std::move(candidates);

    const boost::json::value adapted = gem::adapt_gemini_response(raw);
    ASSERT_TRUE(adapted.is_object());
    const auto& adapted_obj = adapted.as_object();
    ASSERT_TRUE(adapted_obj.contains("content"));
    ASSERT_TRUE(adapted_obj.at("content").is_array());
    const auto& out_content = adapted_obj.at("content").as_array();
    ASSERT_EQ(out_content.size(), kSingleItemSize);

    boost::json::object expected_text_block;
    expected_text_block["type"] = "text";
    expected_text_block["text"] = "Good morning, Hitomi.";
    EXPECT_EQ(out_content[kFirstIndex], boost::json::value(expected_text_block));

    boost::json::object thought_only_part;
    thought_only_part["text"] = "Only internal thoughts.";
    thought_only_part["thought"] = true;
    boost::json::array thought_only_parts;
    thought_only_parts.emplace_back(std::move(thought_only_part));
    boost::json::object thought_only_content;
    thought_only_content["parts"] = std::move(thought_only_parts);
    boost::json::object thought_only_cand;
    thought_only_cand["content"] = std::move(thought_only_content);
    boost::json::array thought_only_cands;
    thought_only_cands.emplace_back(std::move(thought_only_cand));
    boost::json::object thought_only_raw;
    thought_only_raw["candidates"] = std::move(thought_only_cands);

    const boost::json::value empty_visible =
        gem::adapt_gemini_response(thought_only_raw);
    ASSERT_TRUE(empty_visible.is_object());
    const auto& empty_content = empty_visible.as_object().at("content").as_array();
    ASSERT_EQ(empty_content.size(), kSingleItemSize);

    boost::json::object expected_empty_block;
    expected_empty_block["type"] = "text";
    expected_empty_block["text"] = "";
    EXPECT_EQ(empty_content[kFirstIndex], boost::json::value(expected_empty_block));
}

TEST(GeminiClient, AdaptResponseMapsUsageMetadataAndDefaultsMissingCountsToZero) {
    {
        boost::json::object usage_meta;
        usage_meta["promptTokenCount"] = kSamplePromptTokens;
        usage_meta["candidatesTokenCount"] = kSampleCandidatesTokens;
        usage_meta["thoughtsTokenCount"] = kSampleThoughtsTokens;
        usage_meta["cachedContentTokenCount"] = kSampleCachedTokens;
        usage_meta["totalTokenCount"] = kSampleTotalTokens;

        boost::json::object raw;
        raw["usageMetadata"] = std::move(usage_meta);

        const boost::json::value adapted = gem::adapt_gemini_response(raw);
        ASSERT_NO_FATAL_FAILURE(verify_usage_counts(adapted,
                                                    kSamplePromptTokens,
                                                    kExpectedCombinedOutputTokens,
                                                    kSampleThoughtsTokens,
                                                    kSampleCachedTokens,
                                                    kZeroTokens));
    }

    {
        boost::json::object partial_meta;
        partial_meta["promptTokenCount"] = kPartialPromptTokens;
        partial_meta["candidatesTokenCount"] = kPartialCandidatesTokens;

        boost::json::object raw;
        raw["usageMetadata"] = std::move(partial_meta);

        const boost::json::value adapted = gem::adapt_gemini_response(raw);
        ASSERT_NO_FATAL_FAILURE(verify_usage_counts(adapted,
                                                    kPartialPromptTokens,
                                                    kPartialCandidatesTokens,
                                                    kZeroTokens,
                                                    kZeroTokens,
                                                    kZeroTokens));
    }

    const boost::json::value missing_or_non_object[] = {
        boost::json::object{},
        boost::json::value(nullptr),
        boost::json::array{},
    };

    boost::json::object expected_empty_block;
    expected_empty_block["type"] = "text";
    expected_empty_block["text"] = "";

    for (const auto& raw : missing_or_non_object) {
        const boost::json::value adapted = gem::adapt_gemini_response(raw);
        ASSERT_NO_FATAL_FAILURE(verify_usage_counts(
            adapted, kZeroTokens, kZeroTokens, kZeroTokens, kZeroTokens, kZeroTokens));
        const auto& obj = adapted.as_object();
        ASSERT_TRUE(obj.contains("stop_reason"));
        ASSERT_TRUE(obj.at("stop_reason").is_string());
        EXPECT_EQ(std::string(obj.at("stop_reason").as_string()), "end_turn");
        ASSERT_TRUE(obj.contains("content"));
        ASSERT_TRUE(obj.at("content").is_array());
        const auto& content = obj.at("content").as_array();
        ASSERT_EQ(content.size(), kSingleItemSize);
        EXPECT_EQ(content[kFirstIndex], boost::json::value(expected_empty_block));
    }
}

TEST(GeminiClient, AdaptResponseMapsFinishReasonsAndPromptBlockReasons) {
    constexpr StopReasonCase kCases[] = {
        {"absent_finish_reason", CandidatesPresence::SingleCandidate, nullptr, nullptr, "end_turn"},
        {"stop_finish_reason", CandidatesPresence::SingleCandidate, "STOP", nullptr, "end_turn"},
        {"max_tokens_finish_reason", CandidatesPresence::SingleCandidate, "MAX_TOKENS", nullptr, "max_tokens"},
        {"safety_finish_reason", CandidatesPresence::SingleCandidate, "SAFETY", nullptr, "refusal"},
        {"recitation_finish_reason", CandidatesPresence::SingleCandidate, "RECITATION", nullptr, "refusal"},
        {"prohibited_content_finish_reason", CandidatesPresence::SingleCandidate, "PROHIBITED_CONTENT", nullptr, "refusal"},
        {"blocklist_finish_reason", CandidatesPresence::SingleCandidate, "BLOCKLIST", nullptr, "refusal"},
        {"spii_finish_reason", CandidatesPresence::SingleCandidate, "SPII", nullptr, "refusal"},
        {"custom_finish_reason_passthrough", CandidatesPresence::SingleCandidate, "MALFORMED_FUNCTION_CALL", nullptr, "MALFORMED_FUNCTION_CALL"},
        {"missing_candidates_with_block_reason", CandidatesPresence::Absent, nullptr, "PROHIBITED_CONTENT", "refusal"},
        {"empty_candidates_with_block_reason", CandidatesPresence::EmptyArray, nullptr, "SAFETY", "refusal"},
        {"empty_candidates_without_block_reason", CandidatesPresence::EmptyArray, nullptr, nullptr, "end_turn"},
    };
    static_assert(std::size(kCases) > kEmptySize);

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        const boost::json::value adapted =
            gem::adapt_gemini_response(make_stop_reason_response(tc));
        ASSERT_TRUE(adapted.is_object());
        const auto& obj = adapted.as_object();
        ASSERT_TRUE(obj.contains("stop_reason"));
        ASSERT_TRUE(obj.at("stop_reason").is_string());
        EXPECT_EQ(std::string(obj.at("stop_reason").as_string()),
                  tc.expected_stop_reason);
    }
}

namespace {

constexpr int kHttpOk = 200;
constexpr int kHttpRateLimit = 429;
constexpr int kCustomTimeoutSec = 720;
constexpr std::int64_t kErrorCasePromptTokens = 33;
constexpr std::int64_t kErrorCaseThoughtTokens = 7;

class StubRawHttpTransport final : public vn::llm::IRawHttpTransport {
public:
    vn::llm::RawHttpResponse post_json(std::string_view /*target_path*/,
                                       std::string_view json_body,
                                       int /*connect_timeout_sec*/,
                                       int read_timeout_sec) override {
        recorded_bodies.emplace_back(json_body);
        recorded_read_timeouts.push_back(read_timeout_sec);
        if (next_index >= responses.size()) {
            return vn::llm::RawHttpResponse{kHttpOk, "{}"};
        }
        return responses[next_index++];
    }

    std::vector<vn::llm::RawHttpResponse> responses;
    std::size_t next_index = 0u;
    std::vector<std::string> recorded_bodies;
    std::vector<int> recorded_read_timeouts;
};

std::string make_gemini_raw_json(std::string_view text,
                                 std::int64_t thought_tokens) {
    boost::json::object root;
    root["candidates"] = boost::json::array{
        boost::json::object{
            {"content",
             boost::json::object{
                 {"parts",
                  boost::json::array{boost::json::object{{"text", text}}}}}},
            {"finishReason", "STOP"},
        },
    };
    root["usageMetadata"] = boost::json::object{
        {"promptTokenCount", kSamplePromptTokens},
        {"candidatesTokenCount", kSampleCandidatesTokens},
        {"thoughtsTokenCount", thought_tokens},
    };
    return boost::json::serialize(root);
}

}  // namespace

TEST(GeminiClient, AdaptResponseStripsErrorObjectWithoutLeakingRateLimitException) {
    boost::json::object err_only;
    err_only["error"] = boost::json::object{
        {"code", kHttpRateLimit},
        {"status", "RESOURCE_EXHAUSTED"},
        {"message", "Resource has been exhausted (e.g. check quota)."},
    };
    err_only["usageMetadata"] = boost::json::object{
        {"promptTokenCount", kErrorCasePromptTokens},
        {"thoughtsTokenCount", kErrorCaseThoughtTokens},
    };

    const boost::json::value adapted = gem::adapt_gemini_response(err_only);
    ASSERT_NO_FATAL_FAILURE(verify_usage_counts(adapted,
                                                kErrorCasePromptTokens,
                                                kErrorCaseThoughtTokens,
                                                kErrorCaseThoughtTokens,
                                                kZeroTokens,
                                                kZeroTokens));
    const auto& obj = adapted.as_object();
    EXPECT_EQ(std::string(obj.at("stop_reason").as_string()), "end_turn");
    const auto& content = obj.at("content").as_array();
    ASSERT_EQ(content.size(), kSingleItemSize);
    EXPECT_EQ(std::string(content[kFirstIndex].as_object().at("text").as_string()), "");
}

TEST(GeminiClient,
     ClientZeroThoughtGuardCountsLinesAfterOpenTagTimeoutPreservesOptionsAndMovedFromFailsCleanly) {
    // 1. Prompt whose <previous_context> contains a literal "</lines_to_translate>"
    //    before a 4-line <lines_to_translate> block (`slice_size == 4 >= 4` seam)
    //    must still trip `ZeroThoughtError` on initial 0-thought response.
    {
        StubRawHttpTransport transport;
        transport.responses.push_back(
            vn::llm::RawHttpResponse{kHttpOk, make_gemini_raw_json("1. A\n2. B\n3. C\n4. D\n", kZeroTokens)});

        gem::ClientOptions opts;
        opts.api_key = "offline-test-key";
        opts.max_retries = 0;
        opts.zero_thought_max_retries = 0;
        opts.raw_transport = &transport;
        gem::Client client(std::move(opts));

        boost::json::object msg;
        msg["role"] = "user";
        msg["content"] =
            "<previous_context>\n"
            "[Hitomi] Literal tag </lines_to_translate> in prior dialogue.\n"
            "</previous_context>\n\n"
            "<lines_to_translate>\n"
            "1. [Hitomi] 一行目\n"
            "2. [Hitomi] 二行目\n"
            "3. [Hitomi] 三行目\n"
            "4. [Hitomi] 四行目\n"
            "</lines_to_translate>\n";
        boost::json::object body;
        body["model"] = kDefaultModelId;
        body["messages"] = boost::json::array{std::move(msg)};

        EXPECT_THROW((void)client.messages(body), gem::ZeroThoughtError);
        ASSERT_EQ(transport.recorded_read_timeouts.size(), kSingleItemSize);
        EXPECT_EQ(transport.recorded_read_timeouts[kFirstIndex],
                  gem::kDefaultRequestTimeoutSec);
    }

    // 2. Boundary `slice_size == 3` (`< 4`) does NOT trip initial 0-thought guard;
    //    subsequent call with custom `timeout_sec` preserves `raw_transport` and
    //    updates `read_timeout_sec`; moved-from `Client` and `LazyClient` throw
    //    `ApiError` rather than dereferencing null PIMPL.
    {
        StubRawHttpTransport transport;
        transport.responses.push_back(
            vn::llm::RawHttpResponse{kHttpOk, make_gemini_raw_json("1. A\n2. B\n3. C\n", kZeroTokens)});
        transport.responses.push_back(
            vn::llm::RawHttpResponse{kHttpOk, make_gemini_raw_json("1. A\n", kSampleThoughtsTokens)});

        gem::ClientOptions opts;
        opts.api_key = "offline-test-key";
        opts.max_retries = 0;
        opts.zero_thought_max_retries = 0;
        opts.raw_transport = &transport;
        gem::Client client(std::move(opts));

        boost::json::object msg3;
        msg3["role"] = "user";
        msg3["content"] =
            "<lines_to_translate>\n"
            "1. [Hitomi] 一行目\n"
            "2. [Hitomi] 二行目\n"
            "3. [Hitomi] 三行目\n"
            "</lines_to_translate>\n";
        boost::json::object body3;
        body3["model"] = kDefaultModelId;
        body3["messages"] = boost::json::array{std::move(msg3)};

        const boost::json::value r1 = client.messages(body3);
        EXPECT_EQ(r1.as_object().at("usage").as_object().at("thought_tokens").as_int64(),
                  kZeroTokens);

        const boost::json::value r2 = client.messages(body3, kCustomTimeoutSec);
        EXPECT_EQ(r2.as_object().at("usage").as_object().at("thought_tokens").as_int64(),
                  kSampleThoughtsTokens);
        ASSERT_EQ(transport.recorded_read_timeouts.size(), 2u);
        EXPECT_EQ(transport.recorded_read_timeouts[kSecondIndex], kCustomTimeoutSec);

        gem::Client moved_client(std::move(client));
        EXPECT_THROW((void)client.messages(body3), gem::ApiError);
        (void)moved_client;

        gem::LazyClient lazy_src;
        gem::LazyClient lazy_dst(std::move(lazy_src));
        EXPECT_THROW((void)lazy_src.get(), gem::ApiError);
        EXPECT_THROW((void)lazy_src.messages(body3), gem::ApiError);
        (void)lazy_dst;
    }
}
