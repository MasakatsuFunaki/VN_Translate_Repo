// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-room unit tests for `vn::llm_client` public contracts and the
// `CROSS_CHANNEL` model-degradation baseline evaluator.
//
// Compile-time physical insulation gate: include EVERY public header first and
// reject the build immediately if any Windows, WinSock, WinCrypt, OpenSSL, or
// Boost header/macro leaked across the public boundary.
#include "vn/llm/gemini_wire_codec.h"
#include "vn/llm/llm_client.h"
#include "vn/llm/llm_types.h"
#include "vn/llm/zero_thought_guard.h"

#ifdef min
#error "Windows macro `min` leaked through vn/llm public headers!"
#endif
#ifdef max
#error "Windows macro `max` leaked through vn/llm public headers!"
#endif
#ifdef X509_NAME
#error "WinCrypt/OpenSSL `X509_NAME` leaked through vn/llm public headers!"
#endif
#ifdef X509_EXTENSIONS
#error "WinCrypt `X509_EXTENSIONS` leaked through vn/llm public headers!"
#endif
#ifdef _WINDOWS_
#error "`<windows.h>` leaked through vn/llm public headers!"
#endif
#ifdef _WINSOCKAPI_
#error "`<winsock.h>` leaked through vn/llm public headers!"
#endif
#ifdef _WINSOCK2API_
#error "`<winsock2.h>` leaked through vn/llm public headers!"
#endif
#ifdef __wincrypt_h__
#error "`<wincrypt.h>` leaked through vn/llm public headers!"
#endif
#ifdef BOOST_VERSION
#error "Boost headers leaked through vn/llm public headers!"
#endif
#ifdef OPENSSL_VERSION_NUMBER
#error "OpenSSL headers leaked through vn/llm public headers!"
#endif

#include "model_baseline_gauntlet.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace vn::llm {
namespace {

namespace fs = std::filesystem;

static_assert(!RawHttpTransportLike<int>);
static_assert(BackoffSleepCallable<void (*)(std::chrono::milliseconds)>);
static_assert(DiagnosticLogCallable<void (*)(std::string_view)>);

constexpr std::size_t kZeroBatchLines = 0U;
constexpr std::size_t kBelowFloorBatchLines = 54U;
constexpr std::size_t kAboveFloorBatchLines = 55U;
constexpr std::size_t kProductionBatchLines = 120U;
constexpr std::size_t kBelowCeilingBatchLines = 218U;
constexpr std::size_t kAboveCeilingBatchLines = 219U;
constexpr std::size_t kLargeBatchLines = 500U;
constexpr std::int64_t kUnitTokenPerLine = 1;
constexpr std::int64_t kZeroTokensPerLine = 0;
constexpr std::int64_t kNegativeTokensPerLine = -25;
constexpr std::int64_t kExpectedAboveFloorTokens = 16500;
constexpr std::int64_t kExpectedProductionBatchTokens = 36000;
constexpr std::int64_t kExpectedBelowCeilingTokens = 65400;

constexpr char kEmbeddedNulRaw[] = "HIGH\0EXTRA";
constexpr std::size_t kEmbeddedNulBytes = sizeof(kEmbeddedNulRaw) - 1U;
constexpr char kEmbeddedNulModelRaw[] = "gemini-4-carbon\0extra";
constexpr std::size_t kEmbeddedNulModelBytes =
    sizeof(kEmbeddedNulModelRaw) - 1U;
constexpr std::string_view kEmbeddedNulModel(kEmbeddedNulModelRaw,
                                             kEmbeddedNulModelBytes);

constexpr std::int64_t kSamplePromptTokens = 1'000'000;
constexpr std::int64_t kSampleCachedTokens = 250'000;
constexpr std::int64_t kSampleCandidateTokens = 100'000;
constexpr std::int64_t kSampleThoughtTokens = 50'000;
constexpr std::int64_t kPinnedTotalOutputTokens = 150'000;
constexpr double kPinnedCarbonCostUsd = 3.05;
constexpr double kPinnedCarbonCostEur = 2.8975;
constexpr double kPinnedNeonCostUsd = 4.575;
constexpr double kPinnedNeonCostEur = 4.34625;
constexpr double kPinnedHydrogenCostUsd = 2.2875;
constexpr double kPinnedHydrogenCostEur = 2.173125;
constexpr double kPinnedGemini31ProCostUsd = 3.35;
constexpr double kPinnedGemini31ProCostEur = 3.1825;
constexpr double kCostFloatingTolerance = 1e-9;

constexpr std::int64_t kZeroTokens = 0;
constexpr std::int64_t kSaturatingHeadroom = 3;
constexpr std::int64_t kSaturatingThoughtAddend = 10;
constexpr std::int64_t kNegativePromptSample = -100;
constexpr std::int64_t kNegativeCachedSample = -50;
constexpr std::int64_t kNegativeCandidateSample = -20;
constexpr std::int64_t kNegativeThoughtSample = -10;
constexpr double kZeroUsdCost = 0.0;

constexpr std::int64_t kSubFloorOutputTokens = 300;
constexpr std::int64_t kOverCeilingOutputTokens = 999'999;
constexpr std::int64_t kFallbackThinkingBudget = 4096;
constexpr std::int64_t kDisabledThinkingBudget = -1;

constexpr std::int64_t kWirePromptTokens = 4200;
constexpr std::int64_t kWireCachedTokens = 1100;
constexpr std::int64_t kWireCandidateTokens = 310;
constexpr std::int64_t kWireThoughtTokens = 875;
constexpr std::int64_t kWirePinnedTotalOutput = 1185;
constexpr std::int64_t kPromptBlockedInputTokens = 512;
constexpr std::int64_t kCandidateBlockedInputTokens = 99;
constexpr std::int64_t kCandidateBlockedThoughtTokens = 12;

constexpr int kHttpStatusBadRequest = 400;
constexpr int kHttpStatusForbidden = 403;
constexpr int kHttpStatusInternalError = 500;
constexpr int kHttpStatusServiceUnavailable = 503;
constexpr int kHttpStatusMaxServerError = 599;

constexpr std::size_t kBelowInitialSliceMin =
    kMinInitialBatchSliceForThoughtCheck - 1U;
constexpr std::size_t kBelowSubsequentSliceMin =
    kMinSubsequentBatchSliceForThoughtCheck - 1U;
constexpr std::size_t kSubsliceBatchCount = 60U;
constexpr std::size_t kMidSubsliceBatchCount = 32U;
constexpr std::int64_t kMinimalPositiveThoughts = 1;
constexpr std::int64_t kHealthyBatchThoughts = 500;
constexpr std::int64_t kRecoveryBatchThoughts = 320;
constexpr std::int64_t kHealthyGauntletThoughts = 400;
constexpr std::int64_t kOneBelowFloorOffset = 1;
constexpr std::size_t kZeroCompletedBatches = 0U;
constexpr std::size_t kZeroConsecutiveStrikes = 0U;
constexpr std::size_t kSingleCompletedBatch = 1U;
constexpr std::size_t kSingleZeroStrike = 1U;
constexpr std::size_t kSubFloorRepeatCount = 3U;
constexpr std::size_t kSubsliceRepeatCount = 4U;

constexpr std::size_t kIdxCanopyLine = 0U;
constexpr std::size_t kIdxPluralSetupLine = 1U;
constexpr std::size_t kIdxPluralRetortLine = 2U;
constexpr std::size_t kIdxDeadpanAsideLine = 3U;
constexpr std::size_t kIdxHomophoneLine = 4U;
constexpr std::size_t kIdxPercentNLine = 5U;
constexpr std::size_t kIdxSharkPunLine = 6U;
constexpr std::size_t kIdxSkincareLine = 7U;
constexpr std::size_t kShortVectorDropCount = 1U;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0};
        dir_ = fs::temp_directory_path() /
               ("vn_llm_ut_" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

[[nodiscard]] static std::vector<std::string> valid_baseline_eight_lines() {
    return {
        "There was a four-poster canopy bed and European-made chairs in the room.",
        "\"Honestly, you upperclassmen are always like this...\"",
        "\"'You upperclassmen'? Don't lump us together.\"",
        "Miyuki reached ecstasy (not).",
        "\"Theoretically, conquest is achievable as long as you calmly deploy the pieces you can afford in positions where they are most effective.\"",
        "1st Place: 'Mystery' 22 times%N2nd Place: 'Shock' 19 times",
        "A rough texture. Terrible sharkskin.",
        "What kind of skincare routine does she even use?",
    };
}

TEST(LlmTypesUt, ComputeBatchMaxOutputTokensClampsAndPreventsOverflow) {
    struct Case {
        const char* label;
        std::size_t batch_lines;
        std::int64_t tokens_per_line;
        std::int64_t expected;
    };
    constexpr Case kCases[] = {
        {"zero_lines_floor", kZeroBatchLines, kDefaultTokensPerBatchLine,
         kMinOutputTokensFloor},
        {"just_below_floor_b_minus_1", kBelowFloorBatchLines,
         kDefaultTokensPerBatchLine, kMinOutputTokensFloor},
        {"exact_floor_boundary",
         static_cast<std::size_t>(kMinOutputTokensFloor), kUnitTokenPerLine,
         kMinOutputTokensFloor},
        {"just_above_floor_b_plus_1", kAboveFloorBatchLines,
         kDefaultTokensPerBatchLine, kExpectedAboveFloorTokens},
        {"production_120_line_batch", kProductionBatchLines,
         kDefaultTokensPerBatchLine, kExpectedProductionBatchTokens},
        {"just_below_ceiling_b_minus_1", kBelowCeilingBatchLines,
         kDefaultTokensPerBatchLine, kExpectedBelowCeilingTokens},
        {"exact_ceiling_boundary",
         static_cast<std::size_t>(kMaxOutputTokensCeiling), kUnitTokenPerLine,
         kMaxOutputTokensCeiling},
        {"just_above_ceiling_b_plus_1", kAboveCeilingBatchLines,
         kDefaultTokensPerBatchLine, kMaxOutputTokensCeiling},
        {"size_max_times_int64_max_overflow",
         std::numeric_limits<std::size_t>::max(),
         std::numeric_limits<std::int64_t>::max(),
         kMaxOutputTokensCeiling},
        {"zero_tokens_per_line_floor", kLargeBatchLines, kZeroTokensPerLine,
         kMinOutputTokensFloor},
        {"negative_tokens_per_line_floor", kLargeBatchLines,
         kNegativeTokensPerLine, kMinOutputTokensFloor},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(compute_batch_max_output_tokens(tc.batch_lines,
                                                  tc.tokens_per_line),
                  tc.expected);
    }
}

TEST(LlmTypesUt, DisallowedModelPolicyEnforcesArgonBanAndEmbeddedNullSeam) {
    struct ModelCase {
        const char* label;
        std::string_view model;
    };
    constexpr ModelCase kAllowedCases[] = {
        {"empty_resolves_to_default", ""},
        {"whitespace_only_resolves_to_default", "  \t\r\n "},
        {"gemini_4_carbon_lowercase", "gemini-4-carbon"},
        {"gemini_4_carbon_uppercase", "GEMINI-4-CARBON"},
        {"gemini_4_neon", "gemini-4-neon"},
        {"gemini_4_hydrogen", "gemini-4-hydrogen"},
        {"gemini_31_pro_preview", "gemini-3.1-pro-preview"},
        {"partial_prefix_argo_seam", "carbon-argo"},
        {"high_bit_bytes_non_argon", "\x80\xFF-custom"},
    };
    for (const auto& tc : kAllowedCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_FALSE(is_disallowed_model(tc.model));
        ensure_supported_model(tc.model);
    }

    constexpr ModelCase kDisallowedCases[] = {
        {"exact_argon_lowercase", "argon"},
        {"gemini_4_argon_canonical", "gemini-4-argon"},
        {"gemini_4_argon_uppercase_padded", "  GEMINI-4-ARGON  "},
        {"embedded_argon_substring", "my-argon-v2"},
        {"high_bit_bytes_around_mixed_argon", "\xFF" "ArGoN\x80"},
        {"embedded_nul_seam", kEmbeddedNulModel},
    };
    for (const auto& tc : kDisallowedCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_TRUE(is_disallowed_model(tc.model));
        EXPECT_THROW(ensure_supported_model(tc.model), LlmError);
    }
}

TEST(LlmTypesUt, ThinkingLevelNormalizationAndValidationBoundaries) {
    struct Case {
        const char* label;
        std::string_view input;
        const char* expected_normalized;
        bool expected_valid;
    };
    constexpr Case kCases[] = {
        {"empty_budget_fallback", "", "", true},
        {"whitespace_only_budget_fallback", "  \t\r\n ", "", true},
        {"minimal_lowercase", "minimal", "MINIMAL", true},
        {"low_mixed_case_padded", " \tLoW\r\n", "LOW", true},
        {"medium_mixed_case", "MeDiUm", "MEDIUM", true},
        {"high_uppercase", "HIGH", "HIGH", true},
        {"unknown_level_turbo", "turbo", "TURBO", false},
        {"numeric_string", "2048", "2048", false},
        {"prefix_plus_suffix", "HIGH_MAX", "HIGH_MAX", false},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(normalize_thinking_level(tc.input), tc.expected_normalized);
        EXPECT_EQ(is_valid_thinking_level(tc.input), tc.expected_valid);
    }

    constexpr std::string_view kEmbeddedNulLevel(kEmbeddedNulRaw,
                                                 kEmbeddedNulBytes);
    EXPECT_FALSE(is_valid_thinking_level(kEmbeddedNulLevel));
}

TEST(LlmTypesUt, TokenUsageAndCostEstimationMatchPinnedVectors) {
    const LlmTokenUsage usage{kSamplePromptTokens, kSampleCachedTokens,
                              kSampleCandidateTokens, kSampleThoughtTokens};
    EXPECT_EQ(usage.total_output_tokens(), kPinnedTotalOutputTokens);

    struct CostVectorCase {
        const char* label;
        std::string_view model;
        double expected_usd;
        double expected_eur;
    };
    constexpr CostVectorCase kCostVectors[] = {
        {"default_gemini_4_carbon", kDefaultModel, kPinnedCarbonCostUsd,
         kPinnedCarbonCostEur},
        {"gemini_4_neon", "gemini-4-neon", kPinnedNeonCostUsd,
         kPinnedNeonCostEur},
        {"gemini_4_hydrogen", "gemini-4-hydrogen", kPinnedHydrogenCostUsd,
         kPinnedHydrogenCostEur},
        {"gemini_31_pro_uppercase", "GEMINI-3.1-PRO-PREVIEW",
         kPinnedGemini31ProCostUsd, kPinnedGemini31ProCostEur},
    };
    for (const auto& vc : kCostVectors) {
        SCOPED_TRACE(vc.label);
        EXPECT_NEAR(estimate_cost_usd(usage, vc.model), vc.expected_usd,
                    kCostFloatingTolerance);
        EXPECT_NEAR(estimate_cost_eur(usage, vc.model, kDefaultUsdToEurRate),
                    vc.expected_eur, kCostFloatingTolerance);
    }

    const LlmTokenUsage saturating{
        kZeroTokens, kZeroTokens,
        std::numeric_limits<std::int64_t>::max() - kSaturatingHeadroom,
        kSaturatingThoughtAddend};
    EXPECT_EQ(saturating.total_output_tokens(),
              std::numeric_limits<std::int64_t>::max());

    const LlmTokenUsage negative{kNegativePromptSample, kNegativeCachedSample,
                                 kNegativeCandidateSample, kNegativeThoughtSample};
    EXPECT_EQ(negative.total_output_tokens(), kZeroTokens);
    EXPECT_DOUBLE_EQ(estimate_cost_usd(negative, kDefaultModel), kZeroUsdCost);
}

TEST(GeminiWireCodecUt, BuildRequestSerializesMediumThinkingBlockNoneAndClamping) {
    LlmRequest req;
    req.system_prompt = "System role.";
    req.user_prompt = "1. 桜の詩";

    const std::string json = build_gemini_request_json(req);
    EXPECT_NE(json.find("\"systemInstruction\":{\"parts\":[{\"text\":\"System role.\"}]}"),
              std::string::npos) << json;
    EXPECT_NE(json.find("\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"1. 桜の詩\"}]}"),
              std::string::npos) << json;
    EXPECT_NE(json.find("\"thinkingConfig\":{\"thinkingLevel\":\"MEDIUM\"}"),
              std::string::npos) << json;
    EXPECT_EQ(json.find("\"thinkingLevel\":\"HIGH\""), std::string::npos) << json;
    EXPECT_EQ(json.find("thinkingBudget"), std::string::npos) << json;
    EXPECT_NE(json.find("\"maxOutputTokens\":16384"), std::string::npos) << json;
    EXPECT_EQ(json.find("responseMimeType"), std::string::npos) << json;

    constexpr const char* kExpectedHarmCategories[] = {
        "HARM_CATEGORY_HARASSMENT",
        "HARM_CATEGORY_HATE_SPEECH",
        "HARM_CATEGORY_SEXUALLY_EXPLICIT",
        "HARM_CATEGORY_DANGEROUS_CONTENT",
        "HARM_CATEGORY_CIVIC_INTEGRITY",
    };
    for (const char* category : kExpectedHarmCategories) {
        SCOPED_TRACE(category);
        const std::string expected_entry =
            std::string("{\"category\":\"") + category +
            "\",\"threshold\":\"BLOCK_NONE\"}";
        EXPECT_NE(json.find(expected_entry), std::string::npos) << json;
    }

    LlmRequest custom;
    custom.user_prompt = "check";
    custom.thinking_level = " high ";
    custom.max_output_tokens = kSubFloorOutputTokens;
    custom.enforce_min_output_floor = true;
    const std::string high_json = build_gemini_request_json(custom);
    EXPECT_NE(high_json.find("\"thinkingConfig\":{\"thinkingLevel\":\"HIGH\"}"),
              std::string::npos) << high_json;
    EXPECT_NE(high_json.find("\"maxOutputTokens\":16384"),
              std::string::npos) << high_json;

    custom.enforce_min_output_floor = false;
    EXPECT_NE(build_gemini_request_json(custom).find("\"maxOutputTokens\":300"),
              std::string::npos);

    custom.max_output_tokens = kZeroTokens;
    EXPECT_NE(build_gemini_request_json(custom).find("\"maxOutputTokens\":1"),
              std::string::npos);

    custom.max_output_tokens = kOverCeilingOutputTokens;
    custom.enforce_min_output_floor = true;
    custom.safety_block_none = false;
    custom.thinking_level.clear();
    custom.thinking_budget = kFallbackThinkingBudget;
    custom.json_mode = true;
    const std::string fallback_json = build_gemini_request_json(custom);
    EXPECT_EQ(fallback_json.find("systemInstruction"), std::string::npos)
        << fallback_json;
    EXPECT_EQ(fallback_json.find("safetySettings"), std::string::npos)
        << fallback_json;
    EXPECT_EQ(fallback_json.find("thinkingLevel"), std::string::npos)
        << fallback_json;
    EXPECT_NE(fallback_json.find("\"thinkingConfig\":{\"thinkingBudget\":4096}"),
              std::string::npos) << fallback_json;
    EXPECT_NE(fallback_json.find("\"maxOutputTokens\":65536"), std::string::npos)
        << fallback_json;
    EXPECT_NE(fallback_json.find("\"responseMimeType\":\"application/json\""),
              std::string::npos) << fallback_json;

    custom.thinking_budget = kDisabledThinkingBudget;
    EXPECT_EQ(build_gemini_request_json(custom).find("thinkingConfig"),
              std::string::npos);

    custom.thinking_level = "INVALID_LEVEL";
    EXPECT_THROW((void)build_gemini_request_json(custom), LlmError);

    constexpr std::string_view kDisallowedWireModels[] = {
        "gemini-4-argon",
        "GEMINI-4-ARGON",
        kEmbeddedNulModel,
    };
    for (const std::string_view bad_model : kDisallowedWireModels) {
        LlmRequest disallowed_req;
        disallowed_req.model.assign(bad_model.data(), bad_model.size());
        disallowed_req.user_prompt = "check";
        EXPECT_THROW((void)build_gemini_request_json(disallowed_req), LlmError);
    }
}

TEST(GeminiWireCodecUt, ParseResponseStripsThoughtPartsAndExtractsTokenUsage) {
    constexpr std::string_view kWireJson = R"json({
        "candidates": [{
            "content": {
                "role": "model",
                "parts": [
                    {"thought": true, "text": "Internal chain-of-thought reasoning MUST NOT leak."},
                    {"text": "1. First translated line\n"},
                    {"thought": false, "text": "2. Second translated line"}
                ]
            },
            "finishReason": "STOP"
        }],
        "usageMetadata": {
            "promptTokenCount": 4200,
            "cachedContentTokenCount": 1100,
            "candidatesTokenCount": 310,
            "thoughtsTokenCount": 875
        }
    })json";

    const LlmResponse resp = parse_gemini_response_json(kWireJson, kHttpStatusOk);
    EXPECT_FALSE(resp.content_blocked);
    EXPECT_EQ(resp.finish_reason, "STOP");
    EXPECT_EQ(resp.text, "1. First translated line\n2. Second translated line");
    EXPECT_EQ(resp.usage.prompt_tokens, kWirePromptTokens);
    EXPECT_EQ(resp.usage.cached_tokens, kWireCachedTokens);
    EXPECT_EQ(resp.usage.candidate_tokens, kWireCandidateTokens);
    EXPECT_EQ(resp.usage.thought_tokens, kWireThoughtTokens);
    EXPECT_EQ(resp.usage.total_output_tokens(), kWirePinnedTotalOutput);
}

TEST(GeminiWireCodecUt, ParseResponseDetectsPromptBlockAndAllBlockedFinishReasons) {
    constexpr std::string_view kPromptBlockedJson = R"json({
        "promptFeedback": {"blockReason": "PROHIBITED_CONTENT"},
        "usageMetadata": {"promptTokenCount": 512, "thoughtsTokenCount": 0}
    })json";
    const LlmResponse prompt_blocked =
        parse_gemini_response_json(kPromptBlockedJson, kHttpStatusOk);
    EXPECT_TRUE(prompt_blocked.content_blocked);
    EXPECT_EQ(prompt_blocked.finish_reason, "PROHIBITED_CONTENT");
    EXPECT_EQ(prompt_blocked.text, "");
    EXPECT_EQ(prompt_blocked.usage.prompt_tokens, kPromptBlockedInputTokens);
    EXPECT_EQ(prompt_blocked.usage.thought_tokens, kZeroTokens);

    constexpr const char* kBlockedFinishReasons[] = {
        "SAFETY",
        "PROHIBITED_CONTENT",
        "BLOCKLIST",
        "RECITATION",
        "SPII",
        "IMAGE_SAFETY",
    };
    for (const char* reason : kBlockedFinishReasons) {
        SCOPED_TRACE(reason);
        const std::string candidate_blocked_json =
            std::string(R"({"candidates":[{"content":{"parts":[]},"finishReason":")") +
            reason + R"("}],"usageMetadata":{"promptTokenCount":99,"thoughtsTokenCount":12}})";
        const LlmResponse resp =
            parse_gemini_response_json(candidate_blocked_json, kHttpStatusOk);
        EXPECT_TRUE(resp.content_blocked);
        EXPECT_EQ(resp.finish_reason, reason);
        EXPECT_EQ(resp.usage.prompt_tokens, kCandidateBlockedInputTokens);
        EXPECT_EQ(resp.usage.thought_tokens, kCandidateBlockedThoughtTokens);
    }
}

TEST(GeminiWireCodecUt, ParseResponseMapsHttpAndMalformedFailuresToExactExceptions) {
    try {
        (void)parse_gemini_response_json(
            R"({"error":{"status":"RESOURCE_EXHAUSTED","message":"Quota hit"}})",
            kHttpStatusTooManyRequests);
        FAIL() << "Expected LlmRateLimitError on HTTP 429";
    } catch (const LlmRateLimitError& ex) {
        EXPECT_EQ(ex.http_status(), kHttpStatusTooManyRequests);
        EXPECT_NE(std::string_view(ex.what()).find("Quota hit"),
                  std::string_view::npos);
    }

    try {
        (void)parse_gemini_response_json(
            R"({"error":{"status":"RESOURCE_EXHAUSTED","message":"Throttled"}})",
            kHttpStatusForbidden);
        FAIL() << "Expected LlmRateLimitError on RESOURCE_EXHAUSTED";
    } catch (const LlmRateLimitError& ex) {
        EXPECT_EQ(ex.http_status(), kHttpStatusForbidden);
    }

    constexpr int kServerErrors[] = {
        kHttpStatusInternalError,
        kHttpStatusServiceUnavailable,
        kHttpStatusMaxServerError,
    };
    for (const int status : kServerErrors) {
        SCOPED_TRACE(status);
        try {
            (void)parse_gemini_response_json(
                R"({"error":{"message":"Backend unavailable"}})", status);
            FAIL() << "Expected LlmTransientError";
        } catch (const LlmTransientError& ex) {
            EXPECT_EQ(ex.http_status(), status);
        }
    }

    try {
        (void)parse_gemini_response_json(
            R"({"error":{"status":"INVALID_ARGUMENT","message":"Bad field"}})",
            kHttpStatusBadRequest);
        FAIL() << "Expected LlmError on HTTP 400";
    } catch (const LlmRateLimitError&) {
        FAIL() << "HTTP 400 must not throw LlmRateLimitError";
    } catch (const LlmTransientError&) {
        FAIL() << "HTTP 400 must not throw LlmTransientError";
    } catch (const LlmError& ex) {
        EXPECT_EQ(ex.http_status(), kHttpStatusBadRequest);
    }

    constexpr std::string_view kCorruptPayloads[] = {
        "",
        "{not valid json",
        "[]",
        R"({"usageMetadata":{"promptTokenCount":10}})",
        R"({"candidates":[{"content":{"parts":[{"thought":true,"text":"only thought"}]},"finishReason":"STOP"}]})",
    };
    for (std::size_t i = 0; i < std::size(kCorruptPayloads); ++i) {
        SCOPED_TRACE(i);
        EXPECT_THROW(
            (void)parse_gemini_response_json(kCorruptPayloads[i], kHttpStatusOk),
            LlmError);
    }
}

TEST(DotenvParserUt, HandlesBomQuotesEscapesCommentsAndOverrideFile) {
    constexpr std::string_view kDotenv =
        "\xEF\xBB\xBF"
        "# Full-line comment\r\n"
        "OTHER_GEMINI_API_KEY=wrong_prefix\r\n"
        "export GEMINI_API_KEY = first_val # inline comment\n"
        "SINGLE_KEY='literal#hash\\nkept'\n"
        "DOUBLE_KEY=\"line1\\nline2\\\"quoted\\\\slash\" # trailing comment\n"
        "GEMINI_API_KEY=final_override_key   # wins\n";

    EXPECT_EQ(parse_dotenv_value(kDotenv, "GEMINI_API_KEY"), "final_override_key");
    EXPECT_EQ(parse_dotenv_value(kDotenv, "SINGLE_KEY"), "literal#hash\\nkept");
    EXPECT_EQ(parse_dotenv_value(kDotenv, "DOUBLE_KEY"),
              "line1\nline2\"quoted\\slash");
    EXPECT_EQ(parse_dotenv_value(kDotenv, "MISSING_KEY"), "");
    EXPECT_EQ(parse_dotenv_value(kDotenv, "   "), "");

    ScopedTempDir tmp;
    const fs::path env_file = tmp.path() / ".env";
    {
        std::ofstream out(env_file, std::ios::binary);
        out << "\xEF\xBB\xBF"
            << "export GEMINI_API_KEY=\"file_scoped_key_42\"\r\n";
    }
    EXPECT_EQ(resolve_gemini_api_key(env_file), "file_scoped_key_42");
    EXPECT_EQ(resolve_gemini_api_key(tmp.path() / "nonexistent.env"), "");
}

TEST(ZeroThoughtCircuitBreakerUt, EnforcesBatchZeroSubsliceAndConsecutiveBoundaries) {
    {
        ZeroThoughtCircuitBreaker guard;
        guard.check_response(
            kDefaultModel, kDefaultThinkingLevel, /*is_top_level=*/true,
            kBelowInitialSliceMin, /*content_blocked=*/false, kZeroTokens);
        EXPECT_EQ(guard.top_level_batches_completed(), kZeroCompletedBatches);
        EXPECT_EQ(guard.consecutive_top_level_zero_thoughts(),
                  kZeroConsecutiveStrikes);

        guard.check_response(
            kDefaultModel, kDefaultThinkingLevel, /*is_top_level=*/false,
            kSubsliceBatchCount, /*content_blocked=*/false, kZeroTokens);
        EXPECT_EQ(guard.top_level_batches_completed(), kZeroCompletedBatches);
        EXPECT_EQ(guard.consecutive_top_level_zero_thoughts(),
                  kZeroConsecutiveStrikes);

        guard.check_response(
            kDefaultModel, kDefaultThinkingLevel, /*is_top_level=*/true,
            kProductionBatchLines, /*content_blocked=*/true, kZeroTokens);
        EXPECT_EQ(guard.top_level_batches_completed(), kZeroCompletedBatches);
        EXPECT_EQ(guard.consecutive_top_level_zero_thoughts(),
                  kZeroConsecutiveStrikes);

        guard.check_response(
            "gemini-2.5-flash", kDefaultThinkingLevel, /*is_top_level=*/true,
            kProductionBatchLines, /*content_blocked=*/false, kZeroTokens);
        EXPECT_EQ(guard.consecutive_top_level_zero_thoughts(),
                  kZeroConsecutiveStrikes);

        guard.check_response(
            kDefaultModel, "LOW", /*is_top_level=*/true,
            kProductionBatchLines, /*content_blocked=*/false, kZeroTokens);
        EXPECT_EQ(guard.consecutive_top_level_zero_thoughts(),
                  kZeroConsecutiveStrikes);

        struct GuardModeCase {
            const char* label;
            std::string_view model;
            std::string_view level;
            bool expected_guarded;
        };
        constexpr GuardModeCase kModeCases[] = {
            {"argon_high_disallowed", "gemini-4-argon", "HIGH", false},
            {"argon_medium_disallowed", "gemini-4-argon", "MEDIUM", false},
            {"embedded_nul_carbon_disallowed", kEmbeddedNulModel, "HIGH", false},
            {"carbon_argon_hybrid_disallowed", "gemini-4-carbon-argon", "HIGH",
             false},
            {"carbon_medium_trimmed", "gemini-4-carbon", " medium ", true},
            {"carbon_high_uppercase", "GEMINI-4-CARBON", "HIGH", true},
            {"neon_medium", "gemini-4-neon", "MEDIUM", true},
            {"neon_high_uppercase", "GEMINI-4-NEON", " high ", true},
            {"hydrogen_medium", "GEMINI-4-HYDROGEN", "medium", true},
            {"hydrogen_high", "gemini-4-hydrogen", "HIGH", true},
            {"carbon_low_unguarded", "gemini-4-carbon", "LOW", false},
            {"carbon_minimal_unguarded", "gemini-4-carbon", "MINIMAL", false},
            {"empty_level_unguarded", "gemini-4-carbon", "", false},
            {"non_gemini4_model_unguarded", "gemini-3.1-pro-preview", "HIGH",
             false},
        };
        for (const auto& mc : kModeCases) {
            SCOPED_TRACE(mc.label);
            EXPECT_EQ(is_guarded_thinking_mode(mc.model, mc.level),
                      mc.expected_guarded);
        }

        EXPECT_THROW(guard.check_response(
                         "gemini-4-carbon", " high ", /*is_top_level=*/true,
                         kMinInitialBatchSliceForThoughtCheck,
                         /*content_blocked=*/false, kZeroTokens),
                     LlmZeroThoughtError);

        EXPECT_THROW(guard.check_response(
                         "gemini-4-carbon", " medium ", /*is_top_level=*/true,
                         kMinInitialBatchSliceForThoughtCheck,
                         /*content_blocked=*/false, kZeroTokens),
                     LlmZeroThoughtError);

        guard.reset();
        EXPECT_EQ(guard.top_level_batches_completed(), kZeroCompletedBatches);
        EXPECT_EQ(guard.consecutive_top_level_zero_thoughts(),
                  kZeroConsecutiveStrikes);

        guard.check_response(
            kDefaultModel, kDefaultThinkingLevel, /*is_top_level=*/true,
            kMinInitialBatchSliceForThoughtCheck,
            /*content_blocked=*/false, kMinimalPositiveThoughts);
        guard.record_top_level_batch_completed();
        EXPECT_EQ(guard.top_level_batches_completed(), kSingleCompletedBatch);
        EXPECT_EQ(guard.consecutive_top_level_zero_thoughts(),
                  kZeroConsecutiveStrikes);
    }

    {
        ZeroThoughtCircuitBreaker guard;
        guard.check_response(kDefaultModel, kDefaultThinkingLevel, true,
                             kProductionBatchLines, false,
                             kHealthyBatchThoughts);
        guard.record_top_level_batch_completed();
        ASSERT_EQ(guard.top_level_batches_completed(), kSingleCompletedBatch);
        EXPECT_EQ(guard.consecutive_top_level_zero_thoughts(),
                  kZeroConsecutiveStrikes);

        for (std::size_t i = 0; i < kSubFloorRepeatCount; ++i) {
            guard.check_response(kDefaultModel, kDefaultThinkingLevel, true,
                                 kBelowSubsequentSliceMin, false, kZeroTokens);
            guard.record_top_level_batch_completed();
            EXPECT_EQ(guard.consecutive_top_level_zero_thoughts(),
                      kZeroConsecutiveStrikes);
        }
        EXPECT_EQ(guard.top_level_batches_completed(),
                  kSingleCompletedBatch + kSubFloorRepeatCount);

        guard.check_response(kDefaultModel, kDefaultThinkingLevel, true,
                             kMinSubsequentBatchSliceForThoughtCheck, false,
                             kZeroTokens);
        EXPECT_EQ(guard.consecutive_top_level_zero_thoughts(), kSingleZeroStrike);
        guard.record_top_level_batch_completed();

        for (std::size_t i = 0; i < kSubsliceRepeatCount; ++i) {
            guard.check_response(kDefaultModel, kDefaultThinkingLevel,
                                 /*is_top_level=*/false, kMidSubsliceBatchCount,
                                 false, kZeroTokens);
            EXPECT_EQ(guard.consecutive_top_level_zero_thoughts(),
                      kSingleZeroStrike);
        }

        guard.check_response(kDefaultModel, kDefaultThinkingLevel, true,
                             kMinSubsequentBatchSliceForThoughtCheck, false,
                             kRecoveryBatchThoughts);
        EXPECT_EQ(guard.consecutive_top_level_zero_thoughts(),
                  kZeroConsecutiveStrikes);
        guard.record_top_level_batch_completed();

        guard.check_response(kDefaultModel, kDefaultThinkingLevel, true,
                             kMinSubsequentBatchSliceForThoughtCheck, false,
                             kZeroTokens);
        EXPECT_EQ(guard.consecutive_top_level_zero_thoughts(), kSingleZeroStrike);
        guard.record_top_level_batch_completed();

        EXPECT_THROW(guard.check_response(
                         kDefaultModel, kDefaultThinkingLevel, true,
                         kMinSubsequentBatchSliceForThoughtCheck, false,
                         kZeroTokens),
                     LlmZeroThoughtError);
        EXPECT_EQ(guard.consecutive_top_level_zero_thoughts(),
                  kConsecutiveZeroThoughtBatchLimit);
    }
}

TEST(ModelBaselineGauntletUt, PromptAndNumberedParserEnforceExactEightLineContract) {
    ASSERT_EQ(testing::cross_channel_gauntlet_batch().size(),
              testing::kGauntletLineCount);
    ASSERT_EQ(testing::cross_channel_gauntlet_context_after().size(),
              testing::kGauntletLookaheadLineCount);
    EXPECT_NE(testing::gauntlet_system_prompt().find("CROSS†CHANNEL"),
              std::string::npos);

    const std::string prompt = testing::build_gauntlet_user_prompt();
    EXPECT_NE(prompt.find("<lines_to_translate>"), std::string::npos);
    EXPECT_NE(prompt.find("<context_after>"), std::string::npos);
    EXPECT_NE(prompt.find("欧羅巴(ヨーロッパ)"), std::string::npos);
    EXPECT_NE(prompt.find("巨大なサメ"), std::string::npos);

    const std::string fenced_output =
        "```\n"
        "1. [NARRATION] Line one\n"
        "２． [Girl] \"Line two\"\n"
        "**3.** [Tomoki] \"Line three\"\n"
        "4) Line four\n"
        "5. Line five\n"
        "6. Line six%Nsecond half\n"
        "7. Line seven\n"
        "8. Line eight\n"
        "```";
    const std::vector<std::string> parsed =
        testing::parse_gauntlet_numbered_lines(fenced_output);
    ASSERT_EQ(parsed.size(), testing::kGauntletLineCount);
    EXPECT_EQ(parsed[kIdxCanopyLine], "Line one");
    EXPECT_EQ(parsed[kIdxPluralSetupLine], "\"Line two\"");
    EXPECT_EQ(parsed[kIdxPluralRetortLine], "\"Line three\"");
    EXPECT_EQ(parsed[kIdxPercentNLine], "Line six%Nsecond half");
    EXPECT_EQ(parsed[kIdxSkincareLine], "Line eight");

    EXPECT_TRUE(testing::parse_gauntlet_numbered_lines("1. Only one line").empty());
    EXPECT_TRUE(testing::parse_gauntlet_numbered_lines(
                    "1. A\n3. B\n2. C\n4. D\n5. E\n6. F\n7. G\n8. H")
                    .empty());
}

TEST(ModelBaselineGauntletUt, AcceptsValidBaselineAndRejectsAllSixDegradationModes) {
    const std::vector<std::string> good = valid_baseline_eight_lines();
    const auto pass_verdict = testing::evaluate_gauntlet_outputs(
        std::span<const std::string>(good), testing::kBaselineMinThoughtTokens);
    EXPECT_TRUE(pass_verdict.passed)
        << (pass_verdict.failures.empty() ? "" : pass_verdict.failures.front());
    EXPECT_TRUE(pass_verdict.failures.empty());

    {
        const auto v = testing::evaluate_gauntlet_outputs(
            good, testing::kBaselineMinThoughtTokens - kOneBelowFloorOffset);
        EXPECT_FALSE(v.passed);
        ASSERT_FALSE(v.failures.empty());
    }

    struct LineMutationCase {
        const char* label;
        std::size_t line_index;
        const char* degraded_line;
    };
    constexpr LineMutationCase kDegradations[] = {
        {"gate2_furigana_romaji_leak",
         kIdxCanopyLine,
         "There was a canopy bed and Yooroppa-made chairs there."},
        {"gate2_missing_canopy_concept",
         kIdxCanopyLine,
         "There was a large bed and European chairs there."},
        {"gate3_gata_romaji_leak",
         kIdxPluralRetortLine,
         "\"'Gata'? What is that supposed to mean?\""},
        {"gate3_rattle_mistranslation",
         kIdxPluralRetortLine,
         "\"Did something rattle just now?\""},
        {"gate3_singular_senpai_loss",
         kIdxPluralSetupLine,
         "\"Honestly, Senpai is always like this...\""},
        {"gate4_dropped_deadpan_lie_aside",
         kIdxDeadpanAsideLine,
         "Miyuki reached ecstasy."},
        {"gate5_homophone_friendly_trap",
         kIdxHomophoneLine,
         "\"Theoretically, conquest works if you deploy pieces in the most friendly positions.\""},
        {"gate6_lookahead_shark_pun_lost",
         kIdxSharkPunLine,
         "A rough texture. Terrible sandpaper skin."},
        {"structural_missing_percent_n",
         kIdxPercentNLine,
         "1st Place: 'Mystery' 22 times / 2nd Place: 'Shock' 19 times"},
        {"structural_leaked_japanese",
         kIdxSkincareLine,
         "What kind of お手入れ does she even use?"},
    };

    for (const auto& tc : kDegradations) {
        SCOPED_TRACE(tc.label);
        std::vector<std::string> mutated = good;
        mutated[tc.line_index] = tc.degraded_line;
        const auto verdict =
            testing::evaluate_gauntlet_outputs(mutated, kHealthyGauntletThoughts);
        EXPECT_FALSE(verdict.passed);
        EXPECT_FALSE(verdict.failures.empty());
    }

    const std::span<const std::string> seven_lines(
        good.data(), good.size() - kShortVectorDropCount);
    const auto short_verdict = testing::evaluate_gauntlet_outputs(
        seven_lines, kHealthyGauntletThoughts);
    EXPECT_FALSE(short_verdict.passed);
    EXPECT_FALSE(short_verdict.failures.empty());
}

}  // namespace
}  // namespace vn::llm
