// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for Gemini 4 Carbon wire generation, response/telemetry parsing,
// zero-thought safety guard, argon ban, and non-mutating abort behavior in cache_core.
#include "cache/glossary.h"
#include "cache/cache_core.h"
#include "cache/gemini_client.h"
#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace exm::translate {
namespace {

constexpr std::string_view kTempDirName = "vn_ut_gemini_wire_ext06";

constexpr std::size_t kFirstIndex = 0;
constexpr std::size_t kSingleElementCount = 1;
constexpr std::size_t kExpectedSafetyCategoryCount = 5;

// Hand-computed BVA inputs and expected outputs for compute_max_output_tokens:
// clamp(batch_line_count * 300, 16384, 65536).
constexpr std::size_t kEmptyBatchCount = 0;
constexpr std::size_t kSingleLineBatchCount = 1;
constexpr std::size_t kBelowFloorSeamCount = 54;       // 54 * 300 = 16200 -> 16384
constexpr std::size_t kAboveFloorSeamCount = 55;        // 55 * 300 = 16500
constexpr std::size_t kInteriorLinearBatchCount = 150;  // 150 * 300 = 45000
constexpr std::size_t kBelowCeilingSeamCount = 218;    // 218 * 300 = 65400
constexpr std::size_t kAtCeilingSeamCount = 219;       // 219 * 300 = 65700 -> 65536
// 61489146912365272 * 300 == 2^64 + 29984, catching 64-bit unsigned wraparound
// that would otherwise land inside [16384, 65536].
constexpr std::size_t kUnsignedWraparoundCount = 61489146912365272ULL;
constexpr std::size_t kMaxSizeTCount = std::numeric_limits<std::size_t>::max();

constexpr std::int64_t kExpectedFloorTokens = 16384;
constexpr std::int64_t kExpectedJustAboveFloorTokens = 16500;
constexpr std::int64_t kExpectedInteriorTokens = 45000;
constexpr std::int64_t kExpectedJustBelowCeilingTokens = 65400;
constexpr std::int64_t kExpectedCeilingTokens = 65536;

// Hand-picked telemetry values for response parsing and fake client tests.
constexpr std::int64_t kSignedInputTokens = 1420;
constexpr std::int64_t kSignedOutputTokens = 210;
constexpr std::int64_t kSignedThoughtTokens = 1850;
constexpr std::int64_t kSignedCachedTokens = 512;
constexpr std::uint64_t kUnsignedSmallTokens = 2048ULL;
constexpr std::int64_t kExpectedUnsignedSmallTokens = 2048;
constexpr std::int64_t kMaxInt64 = std::numeric_limits<std::int64_t>::max();
constexpr std::uint64_t kMaxInt64AsUint64 = static_cast<std::uint64_t>(kMaxInt64);
constexpr std::uint64_t kJustOverInt64Max = kMaxInt64AsUint64 + 1ULL;
constexpr std::uint64_t kMaxUint64 = std::numeric_limits<std::uint64_t>::max();
constexpr std::int64_t kZeroTokens = 0;
constexpr std::int64_t kNegativeOneTokens = -1;
constexpr std::int64_t kSingleThoughtToken = 1;

// Zero-thought guard seams around kZeroThoughtGuardMinBatchLines == 4.
constexpr std::size_t kGuardBelowSeamLines = 3;
constexpr std::size_t kGuardAtSeamLines = 4;
constexpr std::size_t kGuardAboveSeamLines = 5;
constexpr int kGuardBatchSizeOption = 4;
constexpr int kSpeakerGateCachedDialogueLines = 12;

constexpr int kLineOne = 1;
constexpr int kLineTwo = 2;
constexpr int kLineThree = 3;
constexpr int kLineFour = 4;
constexpr std::size_t kFourTranslatedLines = 4;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_);
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

class FakeGeminiClient final : public gemini::Client {
public:
    using Responder = std::function<boost::json::value(
        const std::string& model, const boost::json::value& body)>;

    explicit FakeGeminiClient(Responder responder)
        : responder_(std::move(responder)) {}

    boost::json::value generate_content(const std::string& model,
                                        const boost::json::value& body) override {
        calls_.emplace_back(model, body);
        return responder_(model, body);
    }

    const std::vector<std::pair<std::string, boost::json::value>>& calls()
        const noexcept {
        return calls_;
    }

private:
    Responder responder_;
    std::vector<std::pair<std::string, boost::json::value>> calls_;
};

std::string_view json_str_view(const boost::json::value& val) {
    const auto& s = val.as_string();
    return std::string_view(s.data(), s.size());
}

std::optional<std::int64_t> extract_exact_json_int64(
    const boost::json::value& val) {
    if (val.is_int64()) {
        return val.as_int64();
    }
    if (val.is_uint64() && val.as_uint64() <= kMaxInt64AsUint64) {
        return static_cast<std::int64_t>(val.as_uint64());
    }
    return std::nullopt;
}

std::optional<std::string_view> extract_xml_section(
    std::string_view text, std::string_view tag_name) {
    const std::string open_tag = "<" + std::string(tag_name) + ">";
    const std::string close_tag = "</" + std::string(tag_name) + ">";
    const std::size_t open_pos = text.find(open_tag);
    if (open_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t content_start = open_pos + open_tag.size();
    const std::size_t close_pos = text.find(close_tag, content_start);
    if (close_pos == std::string_view::npos) {
        return std::nullopt;
    }
    return text.substr(content_start, close_pos - content_start);
}

std::vector<std::pair<std::string, std::string>> make_synthetic_batch(
    std::size_t count) {
    std::vector<std::pair<std::string, std::string>> lines;
    lines.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        lines.emplace_back("dialogue", "夢美\r\n「テスト台詞」");
    }
    return lines;
}

boost::json::object make_dialogue_entry(std::string_view text) {
    boost::json::object entry;
    entry["type"] = "dialogue";
    entry["text"] = std::string(text);
    return entry;
}

bool did_zero_thought_guard_trip(std::string_view model,
                                 std::string_view thinking_level,
                                 bool blocked,
                                 std::size_t batch_line_count,
                                 std::int64_t thought_tokens) {
    try {
        enforce_zero_thought_guard(model, thinking_level, blocked,
                                   batch_line_count, thought_tokens);
        return false;
    } catch (const ZeroThoughtError& err) {
        EXPECT_NE(std::string_view(err.what()).find(model),
                  std::string_view::npos);
        return true;
    }
}

TEST(GeminiWireContract, ComputeMaxOutputTokensAndRequestClampBoundaries) {
    struct TokenBoundaryCase {
        const char* label;
        std::size_t batch_line_count;
        std::int64_t expected_max_tokens;
        bool verify_request_json;
    };

    constexpr TokenBoundaryCase kCases[] = {
        {"empty_batch_floor", kEmptyBatchCount, kExpectedFloorTokens, true},
        {"single_line_batch_floor", kSingleLineBatchCount, kExpectedFloorTokens, true},
        {"below_floor_seam_54", kBelowFloorSeamCount, kExpectedFloorTokens, true},
        {"above_floor_seam_55", kAboveFloorSeamCount, kExpectedJustAboveFloorTokens, true},
        {"interior_linear_150", kInteriorLinearBatchCount, kExpectedInteriorTokens, true},
        {"below_ceiling_seam_218", kBelowCeilingSeamCount, kExpectedJustBelowCeilingTokens, true},
        {"at_ceiling_seam_219", kAtCeilingSeamCount, kExpectedCeilingTokens, true},
        {"unsigned_wraparound_into_range", kUnsignedWraparoundCount, kExpectedCeilingTokens, false},
        {"size_t_max_overflow_seam", kMaxSizeTCount, kExpectedCeilingTokens, false},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(compute_max_output_tokens(tc.batch_line_count),
                  tc.expected_max_tokens);

        if (!tc.verify_request_json) {
            continue;
        }
        const auto batch = make_synthetic_batch(tc.batch_line_count);
        const boost::json::object req = build_gemini_request(batch, {});
        ASSERT_TRUE(req.contains("generationConfig"));
        const auto& gen_cfg = req.at("generationConfig").as_object();
        ASSERT_TRUE(gen_cfg.contains("maxOutputTokens"));
        const auto actual_json_tokens =
            extract_exact_json_int64(gen_cfg.at("maxOutputTokens"));
        ASSERT_TRUE(actual_json_tokens.has_value());
        EXPECT_EQ(*actual_json_tokens, tc.expected_max_tokens);
    }
}

TEST(GeminiWireContract, BuildGeminiRequestWireFormatSafetyAndContextSections) {
    const std::vector<std::pair<std::string, std::string>> previous_context = {
        {"「前の文脈テスト」", "Previous context English line."},
    };
    const std::vector<std::pair<std::string, std::string>> batch_lines = {
        {"「翻訳対象テスト」", "「翻訳対象テスト」"},
    };
    const std::vector<std::pair<std::string, std::string>> context_after = {
        {"「直後の文脈テスト」", "「直後の文脈テスト」"},
    };

    const std::string prompt_with_after =
        build_user_prompt(batch_lines, previous_context, context_after);
    const auto prev_section =
        extract_xml_section(prompt_with_after, "previous_context");
    const auto translate_section =
        extract_xml_section(prompt_with_after, "lines_to_translate");
    const auto after_section =
        extract_xml_section(prompt_with_after, "context_after");

    ASSERT_TRUE(prev_section.has_value());
    EXPECT_NE(prev_section->find("Previous context English line."),
              std::string_view::npos);
    EXPECT_EQ(prev_section->find("「翻訳対象テスト」"), std::string_view::npos);

    ASSERT_TRUE(translate_section.has_value());
    EXPECT_NE(translate_section->find("「翻訳対象テスト」"),
              std::string_view::npos);
    EXPECT_EQ(translate_section->find("「直後の文脈テスト」"),
              std::string_view::npos);

    ASSERT_TRUE(after_section.has_value());
    EXPECT_NE(after_section->find("「直後の文脈テスト」"),
              std::string_view::npos);
    EXPECT_EQ(after_section->find("「翻訳対象テスト」"),
              std::string_view::npos);

    const boost::json::object req =
        build_gemini_request(batch_lines, previous_context, context_after);
    EXPECT_EQ(boost::json::serialize(req).find("thinkingBudget"),
              std::string::npos);

    ASSERT_TRUE(req.contains("systemInstruction"));
    const auto& sys_parts =
        req.at("systemInstruction").as_object().at("parts").as_array();
    ASSERT_EQ(sys_parts.size(), kSingleElementCount);
    EXPECT_EQ(json_str_view(sys_parts[kFirstIndex].as_object().at("text")),
              std::string_view(SYSTEM_PROMPT));

    ASSERT_TRUE(req.contains("contents"));
    const auto& contents = req.at("contents").as_array();
    ASSERT_EQ(contents.size(), kSingleElementCount);
    const auto& content_obj = contents[kFirstIndex].as_object();
    EXPECT_EQ(json_str_view(content_obj.at("role")), "user");
    const auto& user_parts = content_obj.at("parts").as_array();
    ASSERT_EQ(user_parts.size(), kSingleElementCount);
    EXPECT_EQ(json_str_view(user_parts[kFirstIndex].as_object().at("text")),
              prompt_with_after);

    ASSERT_TRUE(req.contains("generationConfig"));
    const auto& gen_cfg = req.at("generationConfig").as_object();
    EXPECT_FALSE(gen_cfg.contains("thinkingBudget"));
    ASSERT_TRUE(gen_cfg.contains("thinkingConfig"));
    const auto& thinking_cfg = gen_cfg.at("thinkingConfig").as_object();
    EXPECT_FALSE(thinking_cfg.contains("thinkingBudget"));
    EXPECT_EQ(json_str_view(thinking_cfg.at("thinkingLevel")), "MEDIUM");

    ASSERT_TRUE(req.contains("safetySettings"));
    const auto& safety = req.at("safetySettings").as_array();
    ASSERT_EQ(safety.size(), kExpectedSafetyCategoryCount);
    std::map<std::string, std::string> safety_by_category;
    for (std::size_t i = 0; i < safety.size(); ++i) {
        const auto& item = safety[i].as_object();
        const std::string category(json_str_view(item.at("category")));
        const std::string threshold(json_str_view(item.at("threshold")));
        safety_by_category.emplace(category, threshold);
    }
    ASSERT_EQ(safety_by_category.size(), kExpectedSafetyCategoryCount);
    constexpr std::array<std::string_view, kExpectedSafetyCategoryCount>
        kRequiredCategories = {
            "HARM_CATEGORY_HARASSMENT",
            "HARM_CATEGORY_HATE_SPEECH",
            "HARM_CATEGORY_SEXUALLY_EXPLICIT",
            "HARM_CATEGORY_DANGEROUS_CONTENT",
            "HARM_CATEGORY_CIVIC_INTEGRITY",
        };
    for (const std::string_view category : kRequiredCategories) {
        const auto it = safety_by_category.find(std::string(category));
        ASSERT_NE(it, safety_by_category.end()) << category;
        EXPECT_EQ(it->second, "BLOCK_NONE") << category;
    }

    const std::string prompt_without_after =
        build_user_prompt(batch_lines, previous_context);
    EXPECT_TRUE(
        extract_xml_section(prompt_without_after, "previous_context").has_value());
    EXPECT_TRUE(
        extract_xml_section(prompt_without_after, "lines_to_translate").has_value());
    EXPECT_EQ(prompt_without_after.find("<context_after>"), std::string::npos);
    EXPECT_EQ(prompt_without_after.find("</context_after>"), std::string::npos);

    const boost::json::object req_low =
        build_gemini_request(batch_lines, previous_context, {}, "LOW");
    EXPECT_EQ(boost::json::serialize(req_low).find("thinkingBudget"),
              std::string::npos);
    const auto& low_thinking_cfg = req_low.at("generationConfig")
                                       .as_object()
                                       .at("thinkingConfig")
                                       .as_object();
    EXPECT_EQ(json_str_view(low_thinking_cfg.at("thinkingLevel")), "LOW");
    const std::string_view low_user_text =
        json_str_view(req_low.at("contents")
                          .as_array()[kFirstIndex]
                          .as_object()
                          .at("parts")
                          .as_array()[kFirstIndex]
                          .as_object()
                          .at("text"));
    EXPECT_EQ(low_user_text.find("<context_after>"), std::string_view::npos);
}

TEST(GeminiWireContract, ParseGeminiResponseUsageTelemetryAndThoughtPartFiltering) {
    // Interleaved thought parts before and between answer parts verify every
    // element of candidates[0].content.parts is filtered, not only parts[0].
    boost::json::array parts;
    parts.push_back(boost::json::object{
        {"thought", true},
        {"text", "Internal chain of thought step 1.\n"},
    });
    parts.push_back(boost::json::object{
        {"text", "1. Yumemi\n\"Stop...\"\n"},
    });
    parts.push_back(boost::json::object{
        {"thought", true},
        {"text", "Internal chain of thought step 2.\n"},
    });
    parts.push_back(boost::json::object{
        {"thought", false},
        {"text", "2. Yumemi\n\"Let me go!\""},
    });
    parts.push_back(boost::json::object{
        {"thought", false},
    });

    boost::json::object signed_response;
    signed_response["candidates"] = boost::json::array{
        boost::json::object{
            {"finishReason", "STOP"},
            {"content", boost::json::object{{"parts", std::move(parts)}}},
        },
    };
    signed_response["usageMetadata"] = boost::json::object{
        {"promptTokenCount", kSignedInputTokens},
        {"candidatesTokenCount", kSignedOutputTokens},
        {"thoughtsTokenCount", kSignedThoughtTokens},
        {"cachedContentTokenCount", kSignedCachedTokens},
    };

    const ParsedGeminiResponse parsed_signed =
        parse_gemini_response(signed_response);
    EXPECT_FALSE(parsed_signed.blocked);
    EXPECT_TRUE(parsed_signed.block_reason.empty());
    EXPECT_EQ(parsed_signed.candidate_text,
              "1. Yumemi\n\"Stop...\"\n2. Yumemi\n\"Let me go!\"");
    EXPECT_EQ(parsed_signed.usage.input_tokens, kSignedInputTokens);
    EXPECT_EQ(parsed_signed.usage.output_tokens, kSignedOutputTokens);
    EXPECT_EQ(parsed_signed.usage.thought_tokens, kSignedThoughtTokens);
    EXPECT_EQ(parsed_signed.usage.cached_tokens, kSignedCachedTokens);

    // Unsigned uint64 token counts: small uint64, exact INT64_MAX, INT64_MAX + 1,
    // and UINT64_MAX clamping to INT64_MAX without signed wraparound.
    boost::json::object uint_usage;
    uint_usage["promptTokenCount"] = kUnsignedSmallTokens;
    uint_usage["candidatesTokenCount"] = kMaxInt64AsUint64;
    uint_usage["thoughtsTokenCount"] = kJustOverInt64Max;
    uint_usage["cachedContentTokenCount"] = kMaxUint64;
    boost::json::object uint_response;
    uint_response["usageMetadata"] = std::move(uint_usage);

    const ParsedGeminiResponse parsed_uint = parse_gemini_response(uint_response);
    EXPECT_EQ(parsed_uint.usage.input_tokens, kExpectedUnsignedSmallTokens);
    EXPECT_EQ(parsed_uint.usage.output_tokens, kMaxInt64);
    EXPECT_EQ(parsed_uint.usage.thought_tokens, kMaxInt64);
    EXPECT_EQ(parsed_uint.usage.cached_tokens, kMaxInt64);

    // Malformed / missing usageMetadata fields default cleanly to 0.
    boost::json::object malformed_response;
    malformed_response["usageMetadata"] = boost::json::object{
        {"promptTokenCount", "not_an_int"},
        {"candidatesTokenCount", nullptr},
        {"thoughtsTokenCount", true},
    };
    const ParsedGeminiResponse parsed_malformed =
        parse_gemini_response(malformed_response);
    EXPECT_EQ(parsed_malformed.usage.input_tokens, kZeroTokens);
    EXPECT_EQ(parsed_malformed.usage.output_tokens, kZeroTokens);
    EXPECT_EQ(parsed_malformed.usage.thought_tokens, kZeroTokens);
    EXPECT_EQ(parsed_malformed.usage.cached_tokens, kZeroTokens);

    const ParsedGeminiResponse parsed_null =
        parse_gemini_response(boost::json::value(nullptr));
    EXPECT_TRUE(parsed_null.candidate_text.empty());
    EXPECT_FALSE(parsed_null.blocked);
    EXPECT_EQ(parsed_null.usage.thought_tokens, kZeroTokens);
}

TEST(GeminiWireContract, ParseGeminiResponseBlockReasonAndFinishReasonMatrix) {
    struct BlockCase {
        const char* label;
        const char* prompt_block_reason;
        const char* candidate_finish_reason;
        bool expected_blocked;
        std::string_view expected_reason_token;
    };

    constexpr BlockCase kCases[] = {
        {"prompt_safety", "SAFETY", nullptr, true, "SAFETY"},
        {"prompt_prohibited", "PROHIBITED_CONTENT", nullptr, true, "PROHIBITED_CONTENT"},
        {"prompt_unspecified_with_stop", "BLOCK_REASON_UNSPECIFIED", "STOP", false, ""},
        {"prompt_empty_with_stop", "", "STOP", false, ""},
        {"finish_prohibited_content", nullptr, "PROHIBITED_CONTENT", true, "PROHIBITED_CONTENT"},
        {"finish_safety", nullptr, "SAFETY", true, "SAFETY"},
        {"finish_recitation", nullptr, "RECITATION", true, "RECITATION"},
        {"finish_blocklist", nullptr, "BLOCKLIST", true, "BLOCKLIST"},
        {"finish_spii", nullptr, "SPII", true, "SPII"},
        {"finish_image_safety", nullptr, "IMAGE_SAFETY", true, "IMAGE_SAFETY"},
        {"finish_language", nullptr, "LANGUAGE", true, "LANGUAGE"},
        {"finish_other", nullptr, "OTHER", true, "OTHER"},
        {"finish_stop", nullptr, "STOP", false, ""},
        {"finish_max_tokens", nullptr, "MAX_TOKENS", false, ""},
        {"finish_unspecified", nullptr, "FINISH_REASON_UNSPECIFIED", false, ""},
        {"finish_absent", nullptr, nullptr, false, ""},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        boost::json::object resp;
        if (tc.prompt_block_reason != nullptr) {
            resp["promptFeedback"] = boost::json::object{
                {"blockReason", tc.prompt_block_reason},
            };
        }
        boost::json::object candidate;
        if (tc.candidate_finish_reason != nullptr) {
            candidate["finishReason"] = tc.candidate_finish_reason;
        }
        candidate["content"] = boost::json::object{
            {"parts", boost::json::array{boost::json::object{{"text", "1. Test"}}}},
        };
        resp["candidates"] = boost::json::array{std::move(candidate)};

        const ParsedGeminiResponse parsed = parse_gemini_response(resp);
        EXPECT_EQ(parsed.blocked, tc.expected_blocked);
        if (tc.expected_blocked) {
            EXPECT_NE(parsed.block_reason.find(tc.expected_reason_token),
                      std::string::npos);
            continue;
        }
        EXPECT_TRUE(parsed.block_reason.empty());
    }
}

TEST(GeminiWireContract, EnforceZeroThoughtGuardBoundaryMatrix) {
    struct GuardCase {
        const char* label;
        std::string_view model;
        std::string_view thinking_level;
        bool blocked;
        std::size_t batch_line_count;
        std::int64_t thought_tokens;
        bool should_throw;
    };

    constexpr GuardCase kCases[] = {
        {"seam_carbon_medium_4_lines_0_thoughts_throws",
         "gemini-4-carbon", "MEDIUM", false, kGuardAtSeamLines, kZeroTokens, true},
        {"uppercase_carbon_high_4_lines_0_thoughts_throws",
         "GEMINI-4-CARBON", "HIGH", false, kGuardAtSeamLines, kZeroTokens, true},
        {"mixed_substring_carbon_5_lines_neg1_thoughts_throws",
         "models/GeMiNi-4-CaRbOn-preview", "medium", false, kGuardAboveSeamLines, kNegativeOneTokens, true},
        {"neon_medium_4_lines_0_thoughts_throws",
         "gemini-4-neon", "MEDIUM", false, kGuardAtSeamLines, kZeroTokens, true},
        {"hydrogen_high_4_lines_0_thoughts_throws",
         "gemini-4-hydrogen", "HIGH", false, kGuardAtSeamLines, kZeroTokens, true},
        {"below_line_seam_3_lines_ok",
         "gemini-4-carbon", "MEDIUM", false, kGuardBelowSeamLines, kZeroTokens, false},
        {"empty_batch_0_lines_ok",
         "gemini-4-carbon", "MEDIUM", false, kEmptyBatchCount, kZeroTokens, false},
        {"above_thought_seam_1_token_ok",
         "gemini-4-carbon", "MEDIUM", false, kGuardAtSeamLines, kSingleThoughtToken, false},
        {"blocked_response_exempt",
         "gemini-4-carbon", "MEDIUM", true, kGuardAtSeamLines, kZeroTokens, false},
        {"low_thinking_level_exempt",
         "gemini-4-carbon", "LOW", false, kGuardAtSeamLines, kZeroTokens, false},
        {"non_gemini4_model_exempt",
         "gemini-2.5-pro", "MEDIUM", false, kGuardAtSeamLines, kZeroTokens, false},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(did_zero_thought_guard_trip(
                      tc.model, tc.thinking_level, tc.blocked,
                      tc.batch_line_count, tc.thought_tokens),
                  tc.should_throw);
    }
}

TEST(GeminiWireContract, CallGeminiDetailedStripsThoughtsPropagatesUsageAndBansArgon) {
    const std::vector<std::pair<std::string, std::string>> previous_context = {
        {"「前の台詞」", "Previous English line."},
    };
    const std::vector<std::pair<std::string, std::string>> four_lines = {
        {"「台詞一」", "「台詞一」"},
        {"「台詞二」", "「台詞二」"},
        {"「台詞三」", "「台詞三」"},
        {"「台詞四」", "「台詞四」"},
    };
    const std::vector<std::pair<std::string, std::string>> lookahead = {
        {"「直後の台詞」", "「直後の台詞」"},
    };

    FakeGeminiClient ok_client(
        [](const std::string&, const boost::json::value&) -> boost::json::value {
            // Put decoy numbered lines inside the thought part so any failure to
            // strip "thought": true overwrites the parsed numbered map.
            boost::json::array parts;
            parts.push_back(boost::json::object{
                {"thought", true},
                {"text", "1. DECOY THOUGHT ONE\n2. DECOY THOUGHT TWO\n"},
            });
            parts.push_back(boost::json::object{
                {"text", "1. First translated line.\n2. Second translated line.\n"},
            });
            parts.push_back(boost::json::object{
                {"thought", false},
                {"text", "3. Third translated line.\n4. Fourth translated line."},
            });
            return boost::json::object{
                {"candidates",
                 boost::json::array{boost::json::object{
                     {"finishReason", "STOP"},
                     {"content", boost::json::object{{"parts", std::move(parts)}}},
                 }}},
                {"usageMetadata",
                 boost::json::object{
                     {"promptTokenCount", kSignedInputTokens},
                     {"candidatesTokenCount", kSignedOutputTokens},
                     {"thoughtsTokenCount", kSignedThoughtTokens},
                     {"cachedContentTokenCount", kSignedCachedTokens},
                 }},
            };
        });

    const GeminiBatchResult detailed = call_gemini_detailed(
        ok_client, four_lines, previous_context, lookahead, "gemini-4-carbon",
        "MEDIUM");
    ASSERT_EQ(ok_client.calls().size(), kSingleElementCount);
    EXPECT_EQ(ok_client.calls()[kFirstIndex].first, "gemini-4-carbon");
    const auto& sent_obj = ok_client.calls()[kFirstIndex].second.as_object();
    EXPECT_EQ(boost::json::serialize(sent_obj).find("thinkingBudget"),
              std::string::npos);
    EXPECT_EQ(json_str_view(sent_obj.at("generationConfig")
                                .as_object()
                                .at("thinkingConfig")
                                .as_object()
                                .at("thinkingLevel")),
              "MEDIUM");
    const std::string_view sent_prompt =
        json_str_view(sent_obj.at("contents")
                          .as_array()[kFirstIndex]
                          .as_object()
                          .at("parts")
                          .as_array()[kFirstIndex]
                          .as_object()
                          .at("text"));
    ASSERT_TRUE(extract_xml_section(sent_prompt, "context_after").has_value());

    EXPECT_FALSE(detailed.blocked);
    EXPECT_EQ(detailed.usage.input_tokens, kSignedInputTokens);
    EXPECT_EQ(detailed.usage.output_tokens, kSignedOutputTokens);
    EXPECT_EQ(detailed.usage.thought_tokens, kSignedThoughtTokens);
    EXPECT_EQ(detailed.usage.cached_tokens, kSignedCachedTokens);
    ASSERT_EQ(detailed.translations.size(), kFourTranslatedLines);
    EXPECT_EQ(detailed.translations.at(kLineOne), "First translated line.");
    EXPECT_EQ(detailed.translations.at(kLineTwo), "Second translated line.");
    EXPECT_EQ(detailed.translations.at(kLineThree), "Third translated line.");
    EXPECT_EQ(detailed.translations.at(kLineFour), "Fourth translated line.");

    // Single-line batch with 0 thought tokens does not trip the 4-line guard
    // and prepends "1. " when the model omits a leading number prefix.
    FakeGeminiClient single_line_client(
        [](const std::string&, const boost::json::value&) -> boost::json::value {
            return boost::json::object{
                {"candidates",
                 boost::json::array{boost::json::object{
                     {"finishReason", "STOP"},
                     {"content",
                      boost::json::object{
                          {"parts",
                           boost::json::array{boost::json::object{
                               {"text", "Unprefixed single line translation."},
                           }}},
                      }},
                 }}},
                {"usageMetadata",
                 boost::json::object{
                     {"promptTokenCount", kSignedInputTokens},
                     {"candidatesTokenCount", kSignedOutputTokens},
                     {"thoughtsTokenCount", kZeroTokens},
                 }},
            };
        });
    const std::vector<std::pair<std::string, std::string>> one_line = {
        {"「単一行」", "「単一行」"},
    };
    const GeminiBatchResult single_res = call_gemini_detailed(
        single_line_client, one_line, {}, {}, "gemini-4-carbon", "MEDIUM");
    ASSERT_EQ(single_res.translations.size(), kSingleElementCount);
    EXPECT_EQ(single_res.translations.at(kLineOne),
              "Unprefixed single line translation.");
    EXPECT_EQ(single_res.usage.thought_tokens, kZeroTokens);

    // 4-line batch with 0 thought tokens on gemini-4-carbon throws ZeroThoughtError.
    FakeGeminiClient zero_thought_client(
        [](const std::string&, const boost::json::value&) -> boost::json::value {
            return boost::json::object{
                {"candidates",
                 boost::json::array{boost::json::object{
                     {"finishReason", "STOP"},
                     {"content",
                      boost::json::object{
                          {"parts",
                           boost::json::array{boost::json::object{
                               {"text", "1. A\n2. B\n3. C\n4. D"},
                           }}},
                      }},
                 }}},
                {"usageMetadata",
                 boost::json::object{
                     {"promptTokenCount", kSignedInputTokens},
                     {"candidatesTokenCount", kSignedOutputTokens},
                     {"thoughtsTokenCount", kZeroTokens},
                 }},
            };
        });
    EXPECT_THROW(call_gemini_detailed(zero_thought_client, four_lines, {}, {},
                                      "gemini-4-carbon", "MEDIUM"),
                 ZeroThoughtError);

    // Disallowed argon / embedded-NUL model identifiers are rejected via
    // vn::llm::ensure_supported_model before touching transport, leaving valid
    // subsequent calls unaffected.
    constexpr std::string_view kEmbeddedNulModel("gemini-4-carbon\0bad", 19);
    constexpr std::array<std::pair<const char*, std::string_view>, 4>
        kDisallowedModels = {{
            {"canonical_argon", "gemini-4-argon"},
            {"uppercase_argon", "GEMINI-4-ARGON"},
            {"prefixed_mixed_argon", "models/Gemini-4-Argon-preview"},
            {"embedded_nul_model", kEmbeddedNulModel},
        }};
    auto https_client = gemini::make_https_client("offline-dummy-key");
    for (const auto& [label, disallowed_model] : kDisallowedModels) {
        SCOPED_TRACE(label);
        try {
            (void)call_gemini_detailed(single_line_client, one_line, {}, {},
                                       disallowed_model, "MEDIUM");
            FAIL() << "Expected vn::llm::LlmError for disallowed model in call_gemini_detailed";
        } catch (const ZeroThoughtError&) {
            FAIL() << "Disallowed model must fail model policy, not ZeroThoughtError";
        } catch (const vn::llm::LlmError& err) {
            EXPECT_NE(std::string_view(err.what()).find("disallowed"),
                      std::string_view::npos);
        }
        EXPECT_EQ(single_line_client.calls().size(), kSingleElementCount);

        try {
            (void)https_client->generate_content(std::string(disallowed_model),
                                                 boost::json::object{});
            FAIL() << "Expected vn::llm::LlmError for disallowed model in HttpsClient";
        } catch (const vn::llm::LlmError& err) {
            EXPECT_NE(std::string_view(err.what()).find("disallowed"),
                      std::string_view::npos);
        }
    }
}

TEST(GeminiWireContract, TranslateAllAbortsOnZeroThoughtWithoutMutatingCacheOrOutputs) {
    const ScopedTempDir temp(kTempDirName);
    const fs::path input_path = temp.path() / "extracted_text.json";
    const fs::path cache_path = temp.path() / "translation_cache.json";
    const fs::path output_path = temp.path() / "translated_text.json";
    const fs::path test_dir_path = temp.path() / "test_batches";
    const fs::path tsv_path = temp.path() / "translations.tsv";

    const std::string cached_dialogue_jp = "夢美\r\n「やめてください……」";
    const std::string cached_dialogue_en =
        postprocess(cached_dialogue_jp, "Yumemi\r\n\"Please stop...\"");
    const std::string extra_paid_jp = "夢美\r\n「先輩、おはようございます。」";
    const std::string extra_paid_en =
        postprocess(extra_paid_jp, "Yumemi\r\n\"Good morning, Senpai.\"");
    ASSERT_FALSE(is_failed_entry(cached_dialogue_jp, cached_dialogue_en));
    ASSERT_FALSE(is_failed_entry(extra_paid_jp, extra_paid_en));

    Cache seeded_cache;
    seeded_cache.set(cached_dialogue_jp, cached_dialogue_en);
    seeded_cache.set(extra_paid_jp, extra_paid_en);
    save_cache(seeded_cache, cache_path.u8string());

    const Bytes cache_bytes_before = read_file(cache_path.u8string());
    ASSERT_FALSE(cache_bytes_before.empty());

    const std::array<std::string_view, kFourTranslatedLines> kUncachedLines = {
        "夢美\r\n「ここは一体どこなの……？」",
        "夢美\r\n「誰かいませんか……！」",
        "夢美\r\n「暗い部屋で何も見えないわ……」",
        "夢美\r\n「お願い、扉を開けてください！」",
    };
    constexpr std::string_view kLookaheadLine =
        "夢美\r\n「足音が近づいてくる……！」";
    constexpr std::string_view kLookaheadDistinctivePhrase =
        "足音が近づいてくる……！";

    boost::json::array script_strings;
    for (int i = 0; i < kSpeakerGateCachedDialogueLines; ++i) {
        script_strings.push_back(make_dialogue_entry(cached_dialogue_jp));
    }
    for (const std::string_view line : kUncachedLines) {
        script_strings.push_back(make_dialogue_entry(line));
    }
    script_strings.push_back(make_dialogue_entry(kLookaheadLine));

    boost::json::object root;
    root["m_tokuten.spt"] = boost::json::object{
        {"strings", std::move(script_strings)},
    };
    write_file_text(input_path.u8string(), json_pretty(root));

    std::vector<std::pair<std::string, boost::json::value>> captured_calls;
    TranslateOptions opt;
    opt.batch_size = kGuardBatchSizeOption;
    opt.input_file = input_path.u8string();
    opt.cache_file = cache_path.u8string();
    opt.output_file = output_path.u8string();
    opt.test_dir = test_dir_path.u8string();
    opt.tsv_file = tsv_path.u8string();
    opt.client_factory = [&captured_calls]() -> std::unique_ptr<gemini::Client> {
        return std::make_unique<FakeGeminiClient>(
            [&captured_calls](const std::string& model,
                              const boost::json::value& body)
                -> boost::json::value {
                captured_calls.emplace_back(model, body);
                // Return valid translations for all 4 lines alongside
                // thoughtsTokenCount == 0 so a missing guard would mutate
                // cache_file on disk.
                return boost::json::object{
                    {"candidates",
                     boost::json::array{boost::json::object{
                         {"finishReason", "STOP"},
                         {"content",
                          boost::json::object{
                              {"parts",
                               boost::json::array{boost::json::object{
                                   {"text",
                                    "1. Yumemi\n\"Where on earth am I...?\"\n"
                                    "2. Yumemi\n\"Is anyone there...?!\"\n"
                                    "3. Yumemi\n\"It's so dark I can't see...\"\n"
                                    "4. Yumemi\n\"Please, open the door!\""},
                               }}},
                          }},
                     }}},
                    {"usageMetadata",
                     boost::json::object{
                         {"promptTokenCount", kSignedInputTokens},
                         {"candidatesTokenCount", kSignedOutputTokens},
                         {"thoughtsTokenCount", kZeroTokens},
                     }},
                };
            });
    };

    EXPECT_THROW(translate_all(opt), ZeroThoughtError);

    ASSERT_EQ(captured_calls.size(), kSingleElementCount);
    EXPECT_EQ(captured_calls[kFirstIndex].first, "gemini-4-carbon");
    const auto& captured_req = captured_calls[kFirstIndex].second.as_object();
    const std::string_view captured_prompt =
        json_str_view(captured_req.at("contents")
                          .as_array()[kFirstIndex]
                          .as_object()
                          .at("parts")
                          .as_array()[kFirstIndex]
                          .as_object()
                          .at("text"));

    const auto prev_section =
        extract_xml_section(captured_prompt, "previous_context");
    const auto translate_section =
        extract_xml_section(captured_prompt, "lines_to_translate");
    const auto after_section =
        extract_xml_section(captured_prompt, "context_after");

    ASSERT_TRUE(prev_section.has_value());
    EXPECT_NE(prev_section->find("Please stop..."), std::string_view::npos);

    ASSERT_TRUE(translate_section.has_value());
    EXPECT_NE(translate_section->find("ここは一体どこなの……？"),
              std::string_view::npos);
    EXPECT_NE(translate_section->find("お願い、扉を開けてください！"),
              std::string_view::npos);
    EXPECT_EQ(translate_section->find(kLookaheadDistinctivePhrase),
              std::string_view::npos);

    ASSERT_TRUE(after_section.has_value());
    EXPECT_NE(after_section->find(kLookaheadDistinctivePhrase),
              std::string_view::npos);

    const Bytes cache_bytes_after = read_file(cache_path.u8string());
    EXPECT_EQ(cache_bytes_after, cache_bytes_before);
    EXPECT_FALSE(fs::exists(output_path));
    EXPECT_FALSE(fs::exists(tsv_path));
}

}  // namespace
}  // namespace exm::translate
