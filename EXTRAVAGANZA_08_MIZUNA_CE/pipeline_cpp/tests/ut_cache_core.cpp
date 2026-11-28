// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Clean-room unit tests for src/cache/cache_core.h public contracts:
// Gemini generateContent wire request construction, response part/telemetry
// parsing and block classification, Gemini 4 zero-thought guard & Argon ban
// seams, and translate_all lookahead forwarding and fail-fast rollback.
#include "cache/cache_core.h"

#include "cache/gemini_client.h"
#include "cache/glossary.h"
#include "common/util.h"
#include "vn/llm/llm_types.h"
#include "vn/llm/zero_thought_guard.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace exm::translate {
namespace {

namespace bj = boost::json;

constexpr std::size_t kExpectedSafetyCategoryCount = 5;
constexpr std::size_t kSingleElementCount = 1;
constexpr std::size_t kTwoElementCount = 2;
constexpr int kTwoEightLineBatchSize = 8;
constexpr int kFourLineBatchSize = 4;
constexpr std::size_t kHappyPathLineCount = 16;
constexpr std::size_t kHappyPathBatchCount = 2;
constexpr std::size_t kPreCachedLineCount = 10;
constexpr std::size_t kRollbackScriptLineCount = 14;
constexpr std::size_t kZeroLineCount = 0;
constexpr std::size_t kFloorSeamBelowLines = 54;
constexpr std::size_t kFloorSeamAboveLines = 55;
constexpr std::size_t kCapSeamBelowLines = 218;
constexpr std::size_t kCapSeamAboveLines = 219;
constexpr std::int64_t kMinExpectedMaxOutputTokens = 16384;
constexpr std::int64_t kExpected55LineMaxTokens = 16500;
constexpr std::int64_t kExpected218LineMaxTokens = 65400;
constexpr std::int64_t kMaxExpectedMaxOutputTokens = 65536;
constexpr std::size_t kBelowInitialGuardSlice =
    vn::llm::kMinInitialBatchSliceForThoughtCheck - 1;
constexpr std::int64_t kZeroThoughts = 0;
constexpr std::int64_t kNegativeThoughts = -1;
constexpr std::int64_t kSinglePositiveThought = 1;

constexpr std::int64_t kSignedPromptTokens = 1250;
constexpr std::int64_t kSignedCandidatesTokens = 480;
constexpr std::int64_t kSignedThoughtTokens = 920;
constexpr std::int64_t kSignedCachedTokens = 600;
constexpr std::int64_t kSignedTotalTokens = 2650;

constexpr std::uint64_t kUnsignedPromptTokens = 5000000100ULL;
constexpr std::uint64_t kUnsignedCandidatesTokens = 730ULL;
constexpr std::uint64_t kUnsignedThoughtTokens = 1540ULL;
constexpr std::uint64_t kUnsignedCachedTokens = 1000ULL;
constexpr std::uint64_t kUnsignedTotalTokens = 5000002370ULL;

constexpr std::int64_t kNegativeTokenSample = -5;
constexpr double kFractionalTokenSample = 12.5;

constexpr std::int64_t kDetailedCallPromptTokens = 500;
constexpr std::int64_t kDetailedCallCandidatesTokens = 120;
constexpr std::int64_t kDetailedCallThoughtTokens = 410;
constexpr std::int64_t kDetailedCallOkTotalTokens = 1030;
constexpr std::int64_t kDetailedCallZeroThoughtTotalTokens = 620;

constexpr std::int64_t kRollbackBatchPromptTokens = 800;
constexpr std::int64_t kRollbackBatchCandidatesTokens = 140;
constexpr std::int64_t kRollbackBatchTotalTokens = 940;

constexpr std::int64_t kBatch1PromptTokens = 1000;
constexpr std::int64_t kBatch1CandidatesTokens = 200;
constexpr std::int64_t kBatch1ThoughtTokens = 310;
constexpr std::int64_t kBatch1CachedTokens = 400;
constexpr std::int64_t kBatch1TotalTokens = 1510;

constexpr std::int64_t kBatch2PromptTokens = 1400;
constexpr std::int64_t kBatch2CandidatesTokens = 250;
constexpr std::int64_t kBatch2ThoughtTokens = 425;
constexpr std::int64_t kBatch2CachedTokens = 600;
constexpr std::int64_t kBatch2TotalTokens = 2075;

constexpr std::int64_t kExpectedAccumulatedPromptTokens = 2400;
constexpr std::int64_t kExpectedAccumulatedCandidatesTokens = 450;
constexpr std::int64_t kExpectedAccumulatedThoughtTokens = 735;
constexpr std::int64_t kExpectedAccumulatedCachedTokens = 1000;
constexpr std::int64_t kExpectedAccumulatedTotalTokens = 3585;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix) {
        static std::atomic<std::uint64_t> sequence{0};
        const std::uint64_t seq = sequence.fetch_add(1, std::memory_order_relaxed);
        const std::string dir_name =
            std::string(prefix) + "_" +
            std::to_string(static_cast<unsigned long>(::GetCurrentProcessId())) + "_" +
            std::to_string(seq);
        path_ = std::filesystem::temp_directory_path() / dir_name;
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
    ScopedTempDir(ScopedTempDir&&) = delete;
    ScopedTempDir& operator=(ScopedTempDir&&) = delete;

    std::string file_path(std::string_view filename) const {
        return (path_ / filename).string();
    }

private:
    std::filesystem::path path_;
};

struct RecordedRequest {
    std::string model;
    bj::value body;
};

struct FakeGeminiState {
    std::vector<bj::value> queued_responses;
    std::vector<RecordedRequest> requests;
};

class FakeGeminiClient final : public gemini::Client {
public:
    explicit FakeGeminiClient(FakeGeminiState& state) noexcept : state_(state) {}

    bj::value generate_content(const std::string& model, const bj::value& body) override {
        const std::size_t idx = state_.requests.size();
        state_.requests.push_back(RecordedRequest{model, body});
        if (idx >= state_.queued_responses.size()) {
            throw std::runtime_error("FakeGeminiClient exhausted queued responses");
        }
        return state_.queued_responses[idx];
    }

private:
    FakeGeminiState& state_;
};

std::int64_t json_integer_to_int64(const bj::value& val) {
    if (val.is_int64()) {
        return val.as_int64();
    }
    if (val.is_uint64()) {
        return static_cast<std::int64_t>(val.as_uint64());
    }
    throw std::runtime_error("Expected JSON integer value");
}

std::vector<std::pair<std::string, std::string>> make_synthetic_batch(std::size_t count) {
    std::vector<std::pair<std::string, std::string>> lines;
    lines.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::string jp = "水菜\r\n「台詞番号" + std::to_string(i + 1) + "。」";
        lines.emplace_back(jp, jp);
    }
    return lines;
}

struct SyntheticScriptLine {
    const char* type;
    std::string jp;
    std::string raw_en;
};

bj::object make_script_entry(const char* type, const std::string& text) {
    bj::object entry;
    entry["type"] = type;
    entry["has_jp"] = true;
    entry["text"] = text;
    return entry;
}

bj::object make_extracted_root(const std::vector<SyntheticScriptLine>& script_lines) {
    bj::array strings;
    for (const SyntheticScriptLine& line : script_lines) {
        strings.push_back(make_script_entry(line.type, line.jp));
    }
    bj::object script_obj;
    script_obj["strings"] = std::move(strings);
    bj::object root;
    root["tantai3.spt"] = std::move(script_obj);
    return root;
}

const std::vector<SyntheticScriptLine>& synthetic_story_lines() {
    static const std::vector<SyntheticScriptLine> kLines = {
        {"dialogue", "水菜\r\n「史郎くん、おはよう……」", "\"Good morning, Shirou-kun...\""},
        {"dialogue", "史郎\r\n「ああ、おはよう。」", "\"Yeah, good morning.\""},
        {"dialogue", "瑞菜\r\n「今日もいい天気だね。」", "\"The weather is nice again today.\""},
        {"dialogue", "？？？\r\n「……見つけたぞ。」", "\"...Found you.\""},
        {"dialogue", "男Ａ\r\n「逃がすな！」", "\"Don't let them escape!\""},
        {"dialogue", "水菜\r\n「っ……誰なの！？」", "\"Ghk... Who are you?!\""},
        {"dialogue", "史郎\r\n「下がっていろ、水菜。」", "\"Stay back, Mizuna.\""},
        {"dialogue", "瑞菜\r\n「わかった……気をつけて。」", "\"Got it... Please be careful.\""},
        {"narrative",
         "巨大なサメが、そこに口を開いていた。",
         "A giant shark stood there with its jaws wide open."},
        {"dialogue", "男Ａ\r\n「観念しろ！」", "\"Give it up already!\""},
        {"dialogue", "？？？\r\n「無駄な抵抗はやめろ。」", "\"Stop your pointless resistance.\""},
        {"dialogue", "水菜\r\n「いや……近づかないで！」", "\"No... Stay away from me!\""},
        {"dialogue", "史郎\r\n「させるものか。」", "\"Not on my watch.\""},
        {"dialogue", "瑞菜\r\n「足元が揺れているよ！」", "\"The ground beneath us is shaking!\""},
        {"dialogue", "男Ａ\r\n「くそっ、何だこの生き物は！？」", "\"Damn it, what is this creature?!\""},
        {"dialogue", "史郎\r\n「今のうちに走るぞ！」", "\"Let's run while we have the chance!\""},
    };
    return kLines;
}

bj::value make_numbered_gemini_response(
    const std::vector<SyntheticScriptLine>& lines,
    std::size_t start_idx,
    std::size_t count,
    std::int64_t prompt_tokens,
    std::int64_t candidates_tokens,
    std::int64_t thought_tokens,
    std::int64_t cached_tokens,
    std::int64_t total_tokens) {
    std::string body_text;
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) {
            body_text += '\n';
        }
        body_text += std::to_string(i + 1) + ". " + lines[start_idx + i].raw_en;
    }

    bj::object thought_part;
    thought_part["thought"] = true;
    thought_part["text"] = "1. Decoy thought translation\n";

    bj::object answer_part;
    answer_part["text"] = body_text;

    bj::array parts;
    parts.push_back(std::move(thought_part));
    parts.push_back(std::move(answer_part));

    bj::object content;
    content["role"] = "model";
    content["parts"] = std::move(parts);

    bj::object candidate;
    candidate["content"] = std::move(content);
    candidate["finishReason"] = "STOP";

    bj::array candidates;
    candidates.push_back(std::move(candidate));

    bj::object usage;
    usage["promptTokenCount"] = prompt_tokens;
    usage["candidatesTokenCount"] = candidates_tokens;
    usage["thoughtsTokenCount"] = thought_tokens;
    usage["cachedContentTokenCount"] = cached_tokens;
    usage["totalTokenCount"] = total_tokens;

    bj::object root;
    root["candidates"] = std::move(candidates);
    root["usageMetadata"] = std::move(usage);
    return root;
}

bool throws_zero_thought_error(std::string_view model,
                               std::string_view thinking_level,
                               bool blocked,
                               std::size_t batch_line_count,
                               std::int64_t thought_tokens) {
    try {
        enforce_zero_thought_guard(
            model, thinking_level, blocked, batch_line_count, thought_tokens);
        return false;
    } catch (const ZeroThoughtError&) {
        return true;
    }
}

struct BlockClassificationCase {
    std::string_view prompt_block_reason;
    std::string_view candidate_finish_reason;
    bool empty_object;
    bool expected_blocked;
    std::string_view expected_block_reason;
    std::string_view expected_finish_reason;
    const char* label;
};

bj::value make_block_case_json(const BlockClassificationCase& tc) {
    if (tc.empty_object) {
        return bj::object{};
    }
    bj::object root;
    if (!tc.prompt_block_reason.empty()) {
        bj::object feedback;
        feedback["blockReason"] = tc.prompt_block_reason;
        root["promptFeedback"] = std::move(feedback);
    }
    if (!tc.candidate_finish_reason.empty()) {
        bj::object candidate;
        candidate["finishReason"] = tc.candidate_finish_reason;
        bj::array candidates;
        candidates.push_back(std::move(candidate));
        root["candidates"] = std::move(candidates);
    }
    return root;
}

TEST(CacheCoreRequestContract, ThinkingConfigSafetySettingsAndUserPromptStructure) {
    const std::vector<std::pair<std::string, std::string>> previous_context = {
        {"史郎\r\n「ああ、おはよう。」", "Shirou\r\n\"Yeah, good morning.\""},
    };
    const std::vector<std::pair<std::string, std::string>> batch_lines = {
        {"水菜\r\n「史郎くん、おはよう……」", "水菜\r\n「史郎くん、おはよう……」"},
        {"史郎\r\n「下がっていろ、水菜。」", "史郎\r\n「下がっていろ、水菜。」"},
    };
    const std::vector<std::pair<std::string, std::string>> context_after = {
        {"巨大なサメが、そこに口を開いていた。", "巨大なサメが、そこに口を開いていた。"},
    };

    const bj::object req = build_gemini_request_body(batch_lines, previous_context, context_after);
    const std::string wire_json = bj::serialize(req);
    EXPECT_EQ(wire_json.find("thinkingBudget"), std::string::npos);

    ASSERT_TRUE(req.contains("generationConfig"));
    const bj::object& gen_cfg = req.at("generationConfig").as_object();
    ASSERT_TRUE(gen_cfg.contains("thinkingConfig"));
    const bj::object& thinking_cfg = gen_cfg.at("thinkingConfig").as_object();
    EXPECT_EQ(thinking_cfg.size(), kSingleElementCount);
    EXPECT_EQ(thinking_cfg.at("thinkingLevel").as_string(), "MEDIUM");
    EXPECT_FALSE(thinking_cfg.contains("thinkingBudget"));

    constexpr std::array<std::string_view, 3> kDisallowedModels = {
        "gemini-4-argon",
        "  models/GEMINI-4-ARGON-PREVIEW  ",
        std::string_view("gemini-4-carbon\0argon", 21),
    };
    auto https_client = gemini::make_https_client("offline-dummy-key");
    for (const std::string_view bad_model : kDisallowedModels) {
        EXPECT_TRUE(vn::llm::is_disallowed_model(bad_model));
        EXPECT_THROW(vn::llm::ensure_supported_model(bad_model), vn::llm::LlmError);
        EXPECT_THROW(
            build_gemini_request_body(
                batch_lines, previous_context, context_after, "MEDIUM", bad_model),
            vn::llm::LlmError);
        EXPECT_THROW(
            https_client->generate_content(std::string(bad_model), bj::object{}),
            vn::llm::LlmError);
    }

    const bj::object omitted_thinking_req =
        build_gemini_request_body(batch_lines, previous_context, context_after, "");
    EXPECT_EQ(bj::serialize(omitted_thinking_req).find("thinkingBudget"), std::string::npos);
    const bj::object& omitted_gen_cfg = omitted_thinking_req.at("generationConfig").as_object();
    EXPECT_FALSE(omitted_gen_cfg.contains("thinkingConfig"));

    ASSERT_TRUE(req.contains("safetySettings"));
    const bj::array& safety = req.at("safetySettings").as_array();
    ASSERT_EQ(safety.size(), kExpectedSafetyCategoryCount);

    constexpr std::array<std::string_view, kExpectedSafetyCategoryCount> kExpectedCategories = {
        "HARM_CATEGORY_HARASSMENT",
        "HARM_CATEGORY_HATE_SPEECH",
        "HARM_CATEGORY_SEXUALLY_EXPLICIT",
        "HARM_CATEGORY_DANGEROUS_CONTENT",
        "HARM_CATEGORY_CIVIC_INTEGRITY",
    };
    const std::set<std::string> expected_category_set(
        kExpectedCategories.begin(), kExpectedCategories.end());

    std::set<std::string> actual_categories;
    for (const bj::value& item : safety) {
        const bj::object& entry = item.as_object();
        EXPECT_EQ(entry.at("threshold").as_string(), "BLOCK_NONE");
        actual_categories.emplace(entry.at("category").as_string());
    }
    EXPECT_EQ(actual_categories, expected_category_set);

    ASSERT_TRUE(req.contains("systemInstruction"));
    const bj::array& sys_parts =
        req.at("systemInstruction").as_object().at("parts").as_array();
    ASSERT_EQ(sys_parts.size(), kSingleElementCount);
    const std::string sys_text(sys_parts[0].as_object().at("text").as_string());
    EXPECT_EQ(sys_text, std::string(SYSTEM_PROMPT));
    EXPECT_NE(sys_text.find("Mizuna"), std::string::npos);

    ASSERT_TRUE(req.contains("contents"));
    const bj::array& contents = req.at("contents").as_array();
    ASSERT_EQ(contents.size(), kSingleElementCount);
    const bj::object& user_turn = contents[0].as_object();
    EXPECT_EQ(user_turn.at("role").as_string(), "user");
    const bj::array& user_parts = user_turn.at("parts").as_array();
    ASSERT_EQ(user_parts.size(), kSingleElementCount);
    const std::string user_text(user_parts[0].as_object().at("text").as_string());
    EXPECT_EQ(user_text, build_user_prompt(batch_lines, previous_context, context_after));

    const std::size_t prev_open = user_text.find("<previous_context>");
    const std::size_t prev_close = user_text.find("</previous_context>");
    const std::size_t lines_open = user_text.find("<lines_to_translate>");
    const std::size_t lines_close = user_text.find("</lines_to_translate>");
    const std::size_t after_open = user_text.find("<context_after>");
    const std::size_t after_close = user_text.find("</context_after>");

    ASSERT_NE(prev_open, std::string::npos);
    ASSERT_NE(prev_close, std::string::npos);
    ASSERT_NE(lines_open, std::string::npos);
    ASSERT_NE(lines_close, std::string::npos);
    ASSERT_NE(after_open, std::string::npos);
    ASSERT_NE(after_close, std::string::npos);

    EXPECT_LT(prev_open, prev_close);
    EXPECT_LT(prev_close, lines_open);
    EXPECT_LT(lines_open, lines_close);
    EXPECT_LT(lines_close, after_open);
    EXPECT_LT(after_open, after_close);

    const std::string prev_section = user_text.substr(prev_open, prev_close - prev_open);
    EXPECT_NE(prev_section.find("ああ、おはよう。"), std::string::npos);
    EXPECT_NE(prev_section.find("Yeah, good morning."), std::string::npos);

    const std::string lines_section = user_text.substr(lines_open, lines_close - lines_open);
    const std::size_t item1_pos = lines_section.find("1.");
    const std::size_t item2_pos = lines_section.find("2.");
    ASSERT_NE(item1_pos, std::string::npos);
    ASSERT_NE(item2_pos, std::string::npos);
    EXPECT_LT(item1_pos, item2_pos);
    EXPECT_NE(lines_section.find("史郎くん、おはよう……"), std::string::npos);
    EXPECT_NE(lines_section.find("下がっていろ、水菜。"), std::string::npos);

    const std::string after_section = user_text.substr(after_open, after_close - after_open);
    EXPECT_NE(after_section.find("巨大なサメが、そこに口を開いていた。"), std::string::npos);

    const std::string no_context_prompt = build_user_prompt(batch_lines, {}, {});
    EXPECT_EQ(no_context_prompt.find("<previous_context>\n"), std::string::npos);
    EXPECT_EQ(no_context_prompt.find("</previous_context>"), std::string::npos);
    EXPECT_EQ(no_context_prompt.find("<context_after>\n"), std::string::npos);
    EXPECT_EQ(no_context_prompt.find("</context_after>"), std::string::npos);
    EXPECT_NE(no_context_prompt.find("<lines_to_translate>\n"), std::string::npos);
    EXPECT_NE(no_context_prompt.find("</lines_to_translate>"), std::string::npos);
}

TEST(CacheCoreRequestContract, MaxOutputTokensBoundaryMatrix) {
    struct MaxTokensCase {
        std::size_t line_count;
        std::int64_t expected_max_tokens;
        const char* label;
    };

    constexpr MaxTokensCase kCases[] = {
        {kZeroLineCount, kMinExpectedMaxOutputTokens, "0_lines_floor"},
        {kSingleElementCount, kMinExpectedMaxOutputTokens, "1_line_floor"},
        {kFloorSeamBelowLines, kMinExpectedMaxOutputTokens, "54_lines_16200_floor_seam"},
        {kFloorSeamAboveLines, kExpected55LineMaxTokens, "55_lines_above_floor_seam"},
        {kCapSeamBelowLines, kExpected218LineMaxTokens, "218_lines_below_cap_seam"},
        {kCapSeamAboveLines, kMaxExpectedMaxOutputTokens, "219_lines_65700_cap_seam"},
    };

    for (const MaxTokensCase& tc : kCases) {
        SCOPED_TRACE(tc.label);
        const auto batch = make_synthetic_batch(tc.line_count);
        const bj::object req = build_gemini_request_body(batch);
        const bj::object& gen_cfg = req.at("generationConfig").as_object();
        EXPECT_EQ(json_integer_to_int64(gen_cfg.at("maxOutputTokens")), tc.expected_max_tokens);
    }
}

TEST(CacheCoreResponseContract, FiltersThoughtPartsIncludingDecoyNumberedLines) {
    bj::object decoy_thought;
    decoy_thought["thought"] = true;
    decoy_thought["text"] = "1. Decoy thought\n2. Second decoy thought\n";

    bj::object omitted_flag_part;
    omitted_flag_part["text"] = "1. \"Good morning, Shirou-kun...\"\n";

    bj::object second_thought;
    second_thought["thought"] = true;
    second_thought["text"] = "99. Ignored internal reasoning";

    bj::object explicit_false_part;
    explicit_false_part["thought"] = false;
    explicit_false_part["text"] = "2. \"Stay back, Mizuna.\"";

    bj::array parts;
    parts.push_back(std::move(decoy_thought));
    parts.push_back(std::move(omitted_flag_part));
    parts.push_back(std::move(second_thought));
    parts.push_back(std::move(explicit_false_part));

    bj::object content;
    content["parts"] = std::move(parts);

    bj::object candidate;
    candidate["content"] = std::move(content);
    candidate["finishReason"] = "STOP";

    bj::array candidates;
    candidates.push_back(std::move(candidate));

    bj::object root;
    root["candidates"] = std::move(candidates);

    const ParsedGeminiResponse parsed = parse_gemini_response(root);
    EXPECT_EQ(parsed.text, "1. \"Good morning, Shirou-kun...\"\n2. \"Stay back, Mizuna.\"");
    EXPECT_EQ(parsed.text.find("Decoy thought"), std::string::npos);

    const std::map<int, std::string> numbered = parse_numbered_response(parsed.text);
    ASSERT_EQ(numbered.size(), kTwoElementCount);
    EXPECT_EQ(numbered.at(1), "\"Good morning, Shirou-kun...\"");
    EXPECT_EQ(numbered.at(2), "\"Stay back, Mizuna.\"");
}

TEST(CacheCoreResponseContract, ExtractsSignedAndUnsignedUsageMetadataAndZerosInvalidFields) {
    bj::object signed_usage;
    signed_usage["promptTokenCount"] = kSignedPromptTokens;
    signed_usage["candidatesTokenCount"] = kSignedCandidatesTokens;
    signed_usage["thoughtsTokenCount"] = kSignedThoughtTokens;
    signed_usage["cachedContentTokenCount"] = kSignedCachedTokens;
    signed_usage["totalTokenCount"] = kSignedTotalTokens;
    bj::object signed_root;
    signed_root["usageMetadata"] = std::move(signed_usage);

    const ParsedGeminiResponse signed_parsed = parse_gemini_response(signed_root);
    EXPECT_EQ(signed_parsed.usage.prompt_tokens, kSignedPromptTokens);
    EXPECT_EQ(signed_parsed.usage.candidates_tokens, kSignedCandidatesTokens);
    EXPECT_EQ(signed_parsed.usage.thought_tokens, kSignedThoughtTokens);
    EXPECT_EQ(signed_parsed.usage.cached_tokens, kSignedCachedTokens);
    EXPECT_EQ(signed_parsed.usage.total_tokens, kSignedTotalTokens);

    bj::object unsigned_usage;
    unsigned_usage["promptTokenCount"] = kUnsignedPromptTokens;
    unsigned_usage["candidatesTokenCount"] = kUnsignedCandidatesTokens;
    unsigned_usage["thoughtsTokenCount"] = kUnsignedThoughtTokens;
    unsigned_usage["cachedContentTokenCount"] = kUnsignedCachedTokens;
    unsigned_usage["totalTokenCount"] = kUnsignedTotalTokens;
    bj::object unsigned_root;
    unsigned_root["usageMetadata"] = std::move(unsigned_usage);

    const ParsedGeminiResponse unsigned_parsed = parse_gemini_response(unsigned_root);
    EXPECT_EQ(unsigned_parsed.usage.prompt_tokens,
              static_cast<std::int64_t>(kUnsignedPromptTokens));
    EXPECT_EQ(unsigned_parsed.usage.candidates_tokens,
              static_cast<std::int64_t>(kUnsignedCandidatesTokens));
    EXPECT_EQ(unsigned_parsed.usage.thought_tokens,
              static_cast<std::int64_t>(kUnsignedThoughtTokens));
    EXPECT_EQ(unsigned_parsed.usage.cached_tokens,
              static_cast<std::int64_t>(kUnsignedCachedTokens));
    EXPECT_EQ(unsigned_parsed.usage.total_tokens,
              static_cast<std::int64_t>(kUnsignedTotalTokens));

    bj::object malformed_usage;
    malformed_usage["promptTokenCount"] = kNegativeTokenSample;
    malformed_usage["candidatesTokenCount"] = kFractionalTokenSample;
    malformed_usage["thoughtsTokenCount"] = "900";
    malformed_usage["cachedContentTokenCount"] = nullptr;
    bj::object malformed_root;
    malformed_root["usageMetadata"] = std::move(malformed_usage);

    const ParsedGeminiResponse malformed_parsed = parse_gemini_response(malformed_root);
    EXPECT_EQ(malformed_parsed.usage.prompt_tokens, 0);
    EXPECT_EQ(malformed_parsed.usage.candidates_tokens, 0);
    EXPECT_EQ(malformed_parsed.usage.thought_tokens, 0);
    EXPECT_EQ(malformed_parsed.usage.cached_tokens, 0);
    EXPECT_EQ(malformed_parsed.usage.total_tokens, 0);
}

TEST(CacheCoreResponseContract, ClassifiesPromptAndCandidateBlockReasons) {
    constexpr BlockClassificationCase kCases[] = {
        {"PROHIBITED_CONTENT", "", false, true, "PROHIBITED_CONTENT", "",
         "prompt_prohibited_content"},
        {"BLOCK_REASON_UNSPECIFIED", "", false, false, "BLOCK_REASON_UNSPECIFIED", "",
         "prompt_block_reason_unspecified"},
        {"", "SAFETY", false, true, "", "SAFETY", "candidate_safety"},
        {"", "RECITATION", false, true, "", "RECITATION", "candidate_recitation"},
        {"", "BLOCKLIST", false, true, "", "BLOCKLIST", "candidate_blocklist"},
        {"", "PROHIBITED_CONTENT", false, true, "", "PROHIBITED_CONTENT",
         "candidate_prohibited_content"},
        {"", "SPII", false, true, "", "SPII", "candidate_spii"},
        {"", "IMAGE_SAFETY", false, true, "", "IMAGE_SAFETY", "candidate_image_safety"},
        {"", "STOP", false, false, "", "STOP", "candidate_stop"},
        {"", "MAX_TOKENS", false, false, "", "MAX_TOKENS", "candidate_max_tokens"},
        {"", "", true, false, "", "", "empty_response_object"},
    };

    for (const BlockClassificationCase& tc : kCases) {
        SCOPED_TRACE(tc.label);
        const ParsedGeminiResponse parsed = parse_gemini_response(make_block_case_json(tc));
        EXPECT_EQ(parsed.blocked, tc.expected_blocked);
        EXPECT_EQ(parsed.block_reason, tc.expected_block_reason);
        EXPECT_EQ(parsed.finish_reason, tc.expected_finish_reason);
    }
}

TEST(CacheCoreZeroThoughtGuard, EnforcesCarbonMediumAndHighUnblockedBatchSeamAndRejectsArgon) {
    struct GuardSeamCase {
        std::string_view model;
        std::string_view thinking_level;
        bool blocked;
        std::size_t batch_line_count;
        std::int64_t thought_tokens;
        bool expect_throw;
        const char* label;
    };

    constexpr GuardSeamCase kCases[] = {
        {"gemini-4-carbon", "MEDIUM", false, vn::llm::kMinInitialBatchSliceForThoughtCheck,
         kZeroThoughts, true, "exact_4_lines_medium_0_thoughts"},
        {"models/GEMINI-4-CARBON", "high", false, vn::llm::kMinInitialBatchSliceForThoughtCheck,
         kNegativeThoughts, true,
         "uppercase_prefixed_carbon_lowercase_high_negative_thoughts"},
        {"gemini-4-carbon", "MEDIUM", false, kBelowInitialGuardSlice, kZeroThoughts, false,
         "below_min_batch_3_lines"},
        {"gemini-4-carbon", "MEDIUM", false, vn::llm::kMinInitialBatchSliceForThoughtCheck,
         kSinglePositiveThought, false, "positive_1_thought_token"},
        {"gemini-4-carbon", "MEDIUM", true, vn::llm::kMinInitialBatchSliceForThoughtCheck,
         kZeroThoughts, false, "blocked_response_exempt"},
        {"gemini-2.5-pro", "MEDIUM", false, vn::llm::kMinInitialBatchSliceForThoughtCheck,
         kZeroThoughts, false, "non_gemini4_model_exempt"},
        {"gemini-4-carbon", "LOW", false, vn::llm::kMinInitialBatchSliceForThoughtCheck,
         kZeroThoughts, false, "low_thinking_exempt"},
        {"gemini-4-argon", "HIGH", false, vn::llm::kMinInitialBatchSliceForThoughtCheck,
         kZeroThoughts, false, "disallowed_argon_exempt_from_circuit_breaker"},
    };

    for (const GuardSeamCase& tc : kCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(throws_zero_thought_error(tc.model,
                                            tc.thinking_level,
                                            tc.blocked,
                                            tc.batch_line_count,
                                            tc.thought_tokens),
                  tc.expect_throw);
    }

    const auto& lines = synthetic_story_lines();
    std::vector<std::pair<std::string, std::string>> four_line_batch;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kFourLineBatchSize); ++i) {
        four_line_batch.emplace_back(lines[i].jp, lines[i].jp);
    }

    FakeGeminiState state;
    state.queued_responses.push_back(make_numbered_gemini_response(lines,
                                                                   0,
                                                                   static_cast<std::size_t>(kFourLineBatchSize),
                                                                   kDetailedCallPromptTokens,
                                                                   kDetailedCallCandidatesTokens,
                                                                   kDetailedCallThoughtTokens,
                                                                   0,
                                                                   kDetailedCallOkTotalTokens));
    state.queued_responses.push_back(make_numbered_gemini_response(lines,
                                                                   0,
                                                                   static_cast<std::size_t>(kFourLineBatchSize),
                                                                   kDetailedCallPromptTokens,
                                                                   kDetailedCallCandidatesTokens,
                                                                   0,
                                                                   0,
                                                                   kDetailedCallZeroThoughtTotalTokens));

    FakeGeminiClient client(state);
    const GeminiBatchResult ok_batch = call_gemini_detailed(client, four_line_batch);
    ASSERT_EQ(state.requests.size(), kSingleElementCount);
    EXPECT_EQ(state.requests[0].model, "gemini-4-carbon");
    EXPECT_FALSE(ok_batch.blocked);
    EXPECT_EQ(ok_batch.usage.thought_tokens, kDetailedCallThoughtTokens);
    ASSERT_EQ(ok_batch.translations.size(), static_cast<std::size_t>(kFourLineBatchSize));
    EXPECT_EQ(ok_batch.translations.at(1), "\"Good morning, Shirou-kun...\"");

    EXPECT_THROW(
        call_gemini_detailed(client, four_line_batch, {}, {}, "gemini-4-argon"),
        vn::llm::LlmError);
    EXPECT_THROW(
        call_gemini_detailed(client, {}, {}, {}, "models/GEMINI-4-ARGON"),
        vn::llm::LlmError);
    EXPECT_EQ(state.requests.size(), kSingleElementCount);

    EXPECT_THROW(call_gemini_detailed(client, four_line_batch), ZeroThoughtError);
}

TEST(CacheCoreTranslateAll, ForwardsLookaheadContextAndAccumulatesThoughtUsage) {
    ScopedTempDir temp_dir("exm_ut_cache_happy");
    const auto& lines = synthetic_story_lines();
    ASSERT_EQ(lines.size(), kHappyPathLineCount);

    const std::string input_file = temp_dir.file_path("extracted_strings.json");
    const std::string cache_file = temp_dir.file_path("translation_cache.json");
    const std::string output_file = temp_dir.file_path("translated_strings.json");
    const std::string tsv_file = temp_dir.file_path("translations.tsv");
    const std::string test_dir = temp_dir.file_path("test_batches");

    exm::write_file_text(input_file, exm::json_pretty(make_extracted_root(lines)));

    FakeGeminiState state;
    state.queued_responses.push_back(make_numbered_gemini_response(
        lines,
        0,
        static_cast<std::size_t>(kTwoEightLineBatchSize),
        kBatch1PromptTokens,
        kBatch1CandidatesTokens,
        kBatch1ThoughtTokens,
        kBatch1CachedTokens,
        kBatch1TotalTokens));
    state.queued_responses.push_back(make_numbered_gemini_response(
        lines,
        static_cast<std::size_t>(kTwoEightLineBatchSize),
        static_cast<std::size_t>(kTwoEightLineBatchSize),
        kBatch2PromptTokens,
        kBatch2CandidatesTokens,
        kBatch2ThoughtTokens,
        kBatch2CachedTokens,
        kBatch2TotalTokens));

    UsageTelemetry total_usage{};
    TranslateOptions opt;
    opt.batch_size = kTwoEightLineBatchSize;
    opt.input_file = input_file;
    opt.cache_file = cache_file;
    opt.output_file = output_file;
    opt.test_dir = test_dir;
    opt.tsv_file = tsv_file;
    opt.client_factory = [&state] { return std::make_unique<FakeGeminiClient>(state); };
    opt.total_usage = &total_usage;

    ASSERT_EQ(translate_all(opt), 0);
    ASSERT_EQ(state.requests.size(), kHappyPathBatchCount);

    const bj::object& req1 = state.requests[0].body.as_object();
    EXPECT_EQ(state.requests[0].model, "gemini-4-carbon");
    EXPECT_EQ(bj::serialize(req1).find("thinkingBudget"), std::string::npos);

    const bj::object& gen_cfg1 = req1.at("generationConfig").as_object();
    EXPECT_GE(json_integer_to_int64(gen_cfg1.at("maxOutputTokens")), kMinExpectedMaxOutputTokens);
    const bj::object& thinking1 = gen_cfg1.at("thinkingConfig").as_object();
    EXPECT_EQ(thinking1.at("thinkingLevel").as_string(), "MEDIUM");
    EXPECT_FALSE(thinking1.contains("thinkingBudget"));

    const bj::array& req1_contents = req1.at("contents").as_array();
    ASSERT_EQ(req1_contents.size(), kSingleElementCount);
    const bj::array& req1_parts = req1_contents[0].as_object().at("parts").as_array();
    ASSERT_EQ(req1_parts.size(), kSingleElementCount);
    const std::string req1_prompt(req1_parts[0].as_object().at("text").as_string());

    EXPECT_EQ(req1_prompt.find("</previous_context>"), std::string::npos);
    const std::size_t after_open = req1_prompt.find("<context_after>\n");
    const std::size_t after_close = req1_prompt.find("</context_after>");
    ASSERT_NE(after_open, std::string::npos);
    ASSERT_NE(after_close, std::string::npos);
    EXPECT_LT(after_open, after_close);
    const std::string after_section = req1_prompt.substr(after_open, after_close - after_open);
    EXPECT_NE(after_section.find("巨大なサメが、そこに口を開いていた。"), std::string::npos);

    const bj::object& req2 = state.requests[1].body.as_object();
    const bj::array& req2_contents = req2.at("contents").as_array();
    ASSERT_EQ(req2_contents.size(), kSingleElementCount);
    const bj::array& req2_parts = req2_contents[0].as_object().at("parts").as_array();
    ASSERT_EQ(req2_parts.size(), kSingleElementCount);
    const std::string req2_prompt(req2_parts[0].as_object().at("text").as_string());
    const std::size_t req2_prev_open = req2_prompt.find("<previous_context>\n");
    const std::size_t req2_prev_close = req2_prompt.find("</previous_context>");
    ASSERT_NE(req2_prev_open, std::string::npos);
    ASSERT_NE(req2_prev_close, std::string::npos);
    EXPECT_LT(req2_prev_open, req2_prev_close);
    const std::string req2_prev_section =
        req2_prompt.substr(req2_prev_open, req2_prev_close - req2_prev_open);
    EXPECT_NE(req2_prev_section.find("Good morning, Shirou-kun..."), std::string::npos);

    EXPECT_EQ(total_usage.prompt_tokens, kExpectedAccumulatedPromptTokens);
    EXPECT_EQ(total_usage.candidates_tokens, kExpectedAccumulatedCandidatesTokens);
    EXPECT_EQ(total_usage.thought_tokens, kExpectedAccumulatedThoughtTokens);
    EXPECT_EQ(total_usage.cached_tokens, kExpectedAccumulatedCachedTokens);
    EXPECT_EQ(total_usage.total_tokens, kExpectedAccumulatedTotalTokens);

    const Cache saved_cache = load_cache(cache_file);
    EXPECT_EQ(saved_cache.size(), kHappyPathLineCount + character_names().size());
    const std::string* mizuna_name = saved_cache.get("水菜");
    ASSERT_NE(mizuna_name, nullptr);
    EXPECT_EQ(*mizuna_name, "Mizuna");
    const std::string* shark_en = saved_cache.get("巨大なサメが、そこに口を開いていた。");
    ASSERT_NE(shark_en, nullptr);
    EXPECT_EQ(*shark_en, "A giant shark stood there with its jaws wide open.");
    EXPECT_TRUE(std::filesystem::exists(output_file));
    EXPECT_TRUE(std::filesystem::exists(tsv_file));
}

TEST(CacheCoreTranslateAll, AbortsOnZeroThoughtsBeforeMutatingCacheOrCreatingOutputs) {
    ScopedTempDir temp_dir("exm_ut_cache_rollback");
    const auto& all_lines = synthetic_story_lines();
    const std::vector<SyntheticScriptLine> lines(
        all_lines.begin(),
        all_lines.begin() + static_cast<std::ptrdiff_t>(kRollbackScriptLineCount));

    const std::string input_file = temp_dir.file_path("extracted_strings.json");
    const std::string cache_file = temp_dir.file_path("translation_cache.json");
    const std::string output_file = temp_dir.file_path("translated_strings.json");
    const std::string tsv_file = temp_dir.file_path("translations.tsv");
    const std::string test_dir = temp_dir.file_path("test_batches");

    exm::write_file_text(input_file, exm::json_pretty(make_extracted_root(lines)));

    Cache initial_cache;
    for (std::size_t i = 0; i < kPreCachedLineCount; ++i) {
        const std::string& jp = lines[i].jp;
        const std::string postprocessed = postprocess(jp, lines[i].raw_en);
        ASSERT_FALSE(is_failed_entry(jp, postprocessed));
        initial_cache.set(jp, postprocessed);
    }
    save_cache(initial_cache, cache_file);

    const exm::Bytes cache_bytes_before = exm::read_file(cache_file);
    ASSERT_FALSE(cache_bytes_before.empty());
    ASSERT_FALSE(std::filesystem::exists(output_file));
    ASSERT_FALSE(std::filesystem::exists(tsv_file));

    FakeGeminiState state;
    state.queued_responses.push_back(make_numbered_gemini_response(
        lines,
        kPreCachedLineCount,
        static_cast<std::size_t>(kFourLineBatchSize),
        kRollbackBatchPromptTokens,
        kRollbackBatchCandidatesTokens,
        0,
        0,
        kRollbackBatchTotalTokens));

    TranslateOptions opt;
    opt.batch_size = kFourLineBatchSize;
    opt.input_file = input_file;
    opt.cache_file = cache_file;
    opt.output_file = output_file;
    opt.test_dir = test_dir;
    opt.tsv_file = tsv_file;
    opt.client_factory = [&state] { return std::make_unique<FakeGeminiClient>(state); };

    EXPECT_THROW(translate_all(opt), ZeroThoughtError);
    ASSERT_EQ(state.requests.size(), kSingleElementCount);

    const exm::Bytes cache_bytes_after = exm::read_file(cache_file);
    EXPECT_EQ(cache_bytes_after, cache_bytes_before);
    EXPECT_FALSE(std::filesystem::exists(output_file));
    EXPECT_FALSE(std::filesystem::exists(tsv_file));
}

}  // namespace
}  // namespace exm::translate
