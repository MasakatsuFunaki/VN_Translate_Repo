// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for src/cache/cache_core.{h,cpp} and src/cache/gemini_client.{h,cpp}:
// speaker extraction, English speaker-plate re-attachment, response parsing,
// context window prompt construction, Gemini API request/response handling,
// and translation_cache_gemini.json persistence and discard guards.
#include "cache/cache_core.h"

#include "cache/gemini_client.h"
#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/util.h"
#include "vn/llm/llm_types.h"
#include "vn/llm/zero_thought_guard.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace tr = exm::translate;
namespace bj = boost::json;
using namespace exm;

namespace {

const char* const kNotACache[] = {"", "{\"unterminated\": ", "[]", "\"text\""};
constexpr std::size_t kSampleLargeCacheCount = 2163;
constexpr std::size_t kSampleDiskCacheCount = 7;
constexpr std::size_t kSeedCacheEntryCount = 4;
constexpr std::int64_t kFakePromptTokenCount = 120;
constexpr std::int64_t kFakeCandidatesTokenCount = 25;
constexpr std::int64_t kFakeCachedContentTokenCount = 80;
constexpr std::int64_t kFakeThoughtsTokenCount = 64;
constexpr std::int64_t kZeroThoughtTokens = 0;
constexpr std::size_t kExpectedSafetyCategoryCount = 5;
constexpr std::size_t kSingleElementCount = 1;
constexpr std::int64_t kSamplePromptTokens = 410;
constexpr std::int64_t kSampleCandidateTokens = 52;
constexpr std::int64_t kSampleCachedTokens = 190;
constexpr std::int64_t kSampleThoughtTokens = 317;
constexpr std::size_t kDetailedBatchLineCount = 4;
constexpr int kTranslateBatchSize = 4;
constexpr std::size_t kUncachedScriptLineCount = 6;
constexpr std::size_t kSpeakerGateSeedFloor = 36;
constexpr std::size_t kExpectedCallsOnAbort = 1;
constexpr std::size_t kExpectedCallsOnComplete = 2;
constexpr std::size_t kEmptyLineCount = 0;
constexpr std::size_t kSingleLineCount = 1;
constexpr std::size_t kMaxLinesBelowFloor = 54;
constexpr std::size_t kMinLinesAboveFloor = 55;
constexpr std::int64_t kExpected55LineTokens = 16500;
constexpr std::size_t kMaxLinesBelowCeiling = 218;
constexpr std::int64_t kExpected218LineTokens = 65400;
constexpr std::size_t kMinLinesAtCeiling = 219;
constexpr std::size_t kBelowInitialGuardSlice = 3;
constexpr std::int64_t kNegativeThoughtTokens = -1;
constexpr std::int64_t kSingleThoughtToken = 1;
constexpr int kFirstLineNumber = 1;
constexpr int kSecondLineNumber = 2;
constexpr int kThirdLineNumber = 3;
constexpr int kFourthLineNumber = 4;

std::int64_t json_int64(const bj::value& v) {
    if (v.is_uint64()) {
        return static_cast<std::int64_t>(v.as_uint64());
    }
    return v.as_int64();
}

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::string file_bytes(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

class FakeGeminiClient final : public gemini::Client {
public:
    std::size_t call_count = 0;
    std::string last_model;
    bj::value last_body;
    std::string canned_text = "1. \"Ah...\"\n2. Morning came.\n";
    std::int64_t thought_tokens = kFakeThoughtsTokenCount;
    bool include_thought_part = false;
    std::string canned_thought = "Internal reasoning draft: 1. \"Wrong\"\n";

    bj::value generate_content(const std::string& model, const bj::value& body) override {
        ++call_count;
        last_model = model;
        last_body = body;
        bj::array parts;
        if (include_thought_part) {
            parts.push_back(bj::object{{"thought", true}, {"text", canned_thought}});
        }
        parts.push_back(bj::object{{"text", canned_text}});
        bj::object content{{"parts", std::move(parts)}};
        bj::object candidate{
            {"content", std::move(content)},
            {"finishReason", "STOP"},
        };
        bj::object usage{
            {"promptTokenCount", kFakePromptTokenCount},
            {"candidatesTokenCount", kFakeCandidatesTokenCount},
            {"cachedContentTokenCount", kFakeCachedContentTokenCount},
            {"thoughtsTokenCount", thought_tokens},
        };
        return bj::object{
            {"candidates", bj::array{std::move(candidate)}},
            {"usageMetadata", std::move(usage)},
        };
    }
};

struct RecordedRequest {
    std::string model;
    bj::value body;
};

struct FakeTranslateSession {
    std::vector<RecordedRequest> requests;
    std::vector<std::string> canned_batch_responses;
    std::int64_t thought_tokens = kFakeThoughtsTokenCount;
};

class SessionGeminiClient final : public gemini::Client {
public:
    explicit SessionGeminiClient(FakeTranslateSession* session) : session_(session) {}

    bj::value generate_content(const std::string& model, const bj::value& body) override {
        const std::size_t idx = session_->requests.size();
        session_->requests.push_back(RecordedRequest{model, body});
        const std::string response_text =
            idx < session_->canned_batch_responses.size()
                ? session_->canned_batch_responses[idx]
                : std::string{};
        bj::array parts{
            bj::object{
                {"thought", true},
                {"text", "Checking speaker voice and upcoming context.\n"},
            },
            bj::object{{"text", response_text}},
        };
        bj::object candidate{
            {"content", bj::object{{"parts", std::move(parts)}}},
            {"finishReason", "STOP"},
        };
        bj::object usage{
            {"promptTokenCount", kFakePromptTokenCount},
            {"candidatesTokenCount", kFakeCandidatesTokenCount},
            {"cachedContentTokenCount", kFakeCachedContentTokenCount},
            {"thoughtsTokenCount", session_->thought_tokens},
        };
        return bj::object{
            {"candidates", bj::array{std::move(candidate)}},
            {"usageMetadata", std::move(usage)},
        };
    }

private:
    FakeTranslateSession* session_;
};

bj::object make_extracted_entry(std::string_view type, std::string_view text) {
    bj::object entry;
    entry["type"] = std::string(type);
    entry["text"] = std::string(text);
    entry["has_jp"] = true;
    return entry;
}

}  // namespace

TEST(CacheMerge, has_real_japanese_excludes_fullwidth_latin_and_brackets) {
    EXPECT_TRUE(tr::has_real_japanese("あ"));
    EXPECT_TRUE(tr::has_real_japanese("アゲハ"));
    EXPECT_FALSE(tr::has_real_japanese("Ａ"));
    EXPECT_FALSE(tr::has_real_japanese("「"));
}

TEST(CacheMerge, needs_translation_rules) {
    EXPECT_TRUE(tr::needs_translation("あい"));
    EXPECT_FALSE(tr::needs_translation(""));
    EXPECT_FALSE(tr::needs_translation("   "));
    EXPECT_FALSE(tr::needs_translation("abc"));
    EXPECT_FALSE(tr::needs_translation("abcd"));
}

TEST(CacheMerge, extract_speaker_requires_quote_bracket_and_maps_speakers) {
    EXPECT_EQ(tr::extract_speaker("レン\r\n「あ」"), "Ren");
    EXPECT_EQ(tr::extract_speaker("夢美\r\n『ええ』"), "Yumemi");
    EXPECT_EQ(tr::extract_speaker("司会者\r\n「それでは」"), "Emcee");
    EXPECT_EQ(tr::extract_speaker("その一心から、\r\nレンは"), "NARRATION");
    EXPECT_EQ(tr::extract_speaker("あ"), "NARRATION");
    EXPECT_EQ(tr::extract_speaker("未知\r\n「x」"), "未知");
}

TEST(CacheMerge, sanitize_ascii_replaces_smart_punctuation) {
    EXPECT_EQ(tr::sanitize_ascii("a—b"), "a--b");
    EXPECT_EQ(tr::sanitize_ascii("“hi”"), "\"hi\"");
    EXPECT_EQ(tr::sanitize_ascii("…"), "...");
    EXPECT_EQ(tr::sanitize_ascii("a　b"), "a b");
    EXPECT_EQ(tr::sanitize_ascii("★Top"), "*Top");
}

TEST(CacheMerge, postprocess_reattaches_english_speaker_plate_and_trailing_crlf) {
    const std::string original = "レン\r\n「あ」";
    EXPECT_EQ(tr::postprocess(original, "\"Ah\""), "Ren\r\n\"Ah\"");
    EXPECT_EQ(tr::postprocess(original, "レン\r\n\"Ah\""), "Ren\r\n\"Ah\"");
    EXPECT_EQ(tr::postprocess(original, "Ren\r\n\"Ah\""), "Ren\r\n\"Ah\"");

    EXPECT_EQ(tr::postprocess("あ\r\n", "Ah"), "Ah\r\n");
    EXPECT_EQ(tr::postprocess("あ\r\n", "Ah\r\n"), "Ah\r\n");
    EXPECT_EQ(tr::postprocess("あ", ""), "");
    EXPECT_EQ(tr::postprocess("あ", "あ"), "あ");
}

TEST(CacheMerge, parses_numbered_response_and_unescapes) {
    auto t = tr::parse_numbered_response("1. Ageha\\r\\n\"Ah\"\n2) Second\n3. ★Top\n");
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[1], "Ageha\r\n\"Ah\"");
    EXPECT_EQ(t[2], "Second");
    EXPECT_EQ(tr::sanitize_ascii(t[3]), "*Top");
}

TEST(CacheMerge, parses_and_strips_leaked_speaker_tag) {
    auto t = tr::parse_numbered_response("[NARRATION] 1. Night fell.\n");
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[1], "Night fell.");
}

TEST(CacheMerge, build_user_prompt_includes_previous_context_and_context_after) {
    const std::vector<std::pair<std::string, std::string>> prev = {
        {"Ageha", "Ageha\r\n\"Previous line\""},
    };
    const std::vector<std::pair<std::string, std::string>> batch = {
        {"Shumi", "シュミ\r\n「現在の行」"},
    };
    const std::vector<std::pair<std::string, std::string>> after = {
        {"NARRATION", "次の行。"},
    };
    const std::string prompt = tr::build_user_prompt(batch, prev, after);
    EXPECT_NE(prompt.find("<previous_context>"), std::string::npos);
    EXPECT_NE(prompt.find("<lines_to_translate>"), std::string::npos);
    EXPECT_NE(prompt.find("<context_after>"), std::string::npos);
    EXPECT_NE(prompt.find("1. [Shumi] シュミ\\r\\n「現在の行」"), std::string::npos);
}

TEST(CacheMerge, call_gemini_sends_gemini_4_carbon_payload_and_parses_candidates) {
    FakeGeminiClient fake;
    const std::vector<std::pair<std::string, std::string>> batch = {
        {"Ageha", "アゲハ\r\n「あ……」"},
        {"NARRATION", "朝が来た。"},
    };
    const auto result = tr::call_gemini(fake, batch, {}, {});
    EXPECT_EQ(fake.last_model, "gemini-4-carbon");
    ASSERT_TRUE(fake.last_body.is_object());
    EXPECT_TRUE(fake.last_body.get_object().if_contains("systemInstruction"));
    EXPECT_TRUE(fake.last_body.get_object().if_contains("contents"));
    EXPECT_TRUE(fake.last_body.get_object().if_contains("generationConfig"));
    EXPECT_TRUE(fake.last_body.get_object().if_contains("safetySettings"));
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result.at(1), "\"Ah...\"");
    EXPECT_EQ(result.at(2), "Morning came.");
}

TEST(CacheMerge, refuse_cache_discard_protects_paid_cache_entries) {
    EXPECT_FALSE(tr::refuse_cache_discard(0, "--test", false).has_value());
    EXPECT_FALSE(
        tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD, "--test", false).has_value());

    for (const char* flag : {"--test", "--retranslate", "--clean"}) {
        auto why = tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD + 1, flag, false);
        ASSERT_TRUE(why.has_value()) << flag;
        EXPECT_NE(why->find(flag), std::string::npos);
        EXPECT_NE(why->find("--max-batches"), std::string::npos);
        EXPECT_NE(why->find("--discard-cache"), std::string::npos);
    }

    auto why = tr::refuse_cache_discard(kSampleLargeCacheCount, "--test", false);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find(std::to_string(kSampleLargeCacheCount)), std::string::npos);
    EXPECT_FALSE(tr::refuse_cache_discard(kSampleLargeCacheCount, "--test", true).has_value());
}

TEST(CacheMerge, failed_entry_predicate_and_purge_preserve_valid_entries) {
    EXPECT_TRUE(tr::is_failed_entry("こんにちは", "こんにちは"));
    EXPECT_TRUE(tr::is_failed_entry("……", "……"));
    EXPECT_FALSE(tr::is_failed_entry("こんにちは", "Hello"));
    EXPECT_FALSE(tr::is_failed_entry("OK", "OK"));
    EXPECT_FALSE(tr::is_failed_entry("", ""));

    const ScopedTempDir tmp("ageha_ut_cache_purge_gemini");
    const std::string path = (tmp.path() / "cache.json").u8string();
    const std::string expected = (tmp.path() / "cache.json.expected").u8string();

    tr::Cache seed;
    seed.set("朝が来た。", "Morning came.");
    seed.set("こんにちは", "こんにちは");
    seed.set("OK", "OK");
    seed.set("雨が降る。", "It is raining.");
    tr::save_cache(seed, path);

    const tr::Cache before = tr::load_cache(path);
    ASSERT_EQ(before.size(), kSeedCacheEntryCount);

    tr::Cache survivors;
    for (const auto& [jp, en] : before.items()) {
        if (!tr::is_failed_entry(jp, en)) survivors.set(jp, en);
    }
    tr::save_cache(survivors, expected);

    tr::Cache purged = tr::load_cache(path);
    EXPECT_EQ(tr::purge_failed_entries(purged), 1u);
    tr::save_cache(purged, path);

    EXPECT_EQ(file_bytes(path), file_bytes(expected));
}

TEST(CacheMerge, disk_cache_io_handles_missing_and_malformed_files) {
    const ScopedTempDir tmp("ageha_ut_translation_cache_gemini");
    const std::string missing = (tmp.path() / "missing.json").u8string();
    EXPECT_EQ(tr::load_cache(missing).size(), 0u);
    EXPECT_EQ(tr::cache_entry_count(missing), 0u);

    const std::string path = (tmp.path() / "cache.json").u8string();
    for (const char* content : kNotACache) {
        exm::write_file(path, std::string(content));
        try {
            tr::load_cache(path);
            ADD_FAILURE() << "loaded a file that is not a cache: [" << content << "]";
        } catch (const std::exception& e) {
            EXPECT_NE(std::string(e.what()).find(path), std::string::npos);
        }
    }

    tr::Cache cache;
    for (std::size_t i = 0; i < kSampleDiskCacheCount; ++i) {
        cache.set("日本語" + std::to_string(i), "English " + std::to_string(i));
    }
    tr::save_cache(cache, path);
    EXPECT_EQ(tr::cache_entry_count(path), kSampleDiskCacheCount);
}

TEST(CacheMerge, build_gemini_request_configures_thinking_level_and_safety_settings) {
    const std::vector<std::pair<std::string, std::string>> batch = {
        {"Ren", "レン\r\n「あ……」"},
    };

    const bj::object req = tr::build_gemini_request(batch);
    ASSERT_TRUE(req.if_contains("generationConfig"));
    const bj::object& gen_cfg = req.at("generationConfig").as_object();
    EXPECT_FALSE(gen_cfg.if_contains("thinkingBudget"));
    ASSERT_TRUE(gen_cfg.if_contains("thinkingConfig"));
    const bj::object& thinking_cfg = gen_cfg.at("thinkingConfig").as_object();
    EXPECT_FALSE(thinking_cfg.if_contains("thinkingBudget"));
    ASSERT_TRUE(thinking_cfg.if_contains("thinkingLevel"));
    EXPECT_EQ(thinking_cfg.at("thinkingLevel").as_string(), "MEDIUM");

    constexpr std::array<std::string_view, 3> kDisallowedModels = {
        "gemini-4-argon",
        "  models/GEMINI-4-ARGON-PREVIEW  ",
        std::string_view("gemini-4-carbon\0argon", 21),
    };
    auto https_client = gemini::make_https_client("offline-dummy-key");
    for (const std::string_view bad_model : kDisallowedModels) {
        EXPECT_TRUE(vn::llm::is_disallowed_model(bad_model));
        EXPECT_THROW(vn::llm::ensure_supported_model(bad_model), vn::llm::LlmError);
        EXPECT_THROW(tr::build_gemini_request(batch, {}, {}, "MEDIUM", bad_model),
                     vn::llm::LlmError);
        EXPECT_THROW(
            https_client->generate_content(std::string(bad_model), bj::object{}),
            vn::llm::LlmError);
    }

    const bj::object low_req = tr::build_gemini_request(batch, {}, {}, "LOW");
    ASSERT_TRUE(low_req.if_contains("generationConfig"));
    const bj::object& low_gen = low_req.at("generationConfig").as_object();
    EXPECT_FALSE(low_gen.if_contains("thinkingBudget"));
    ASSERT_TRUE(low_gen.if_contains("thinkingConfig"));
    const bj::object& low_thinking = low_gen.at("thinkingConfig").as_object();
    EXPECT_FALSE(low_thinking.if_contains("thinkingBudget"));
    ASSERT_TRUE(low_thinking.if_contains("thinkingLevel"));
    EXPECT_EQ(low_thinking.at("thinkingLevel").as_string(), "LOW");

    ASSERT_TRUE(req.if_contains("safetySettings"));
    const bj::array& safety = req.at("safetySettings").as_array();
    ASSERT_EQ(safety.size(), kExpectedSafetyCategoryCount);

    std::set<std::string> actual_categories;
    for (const bj::value& item : safety) {
        const bj::object& entry = item.as_object();
        ASSERT_TRUE(entry.if_contains("category"));
        ASSERT_TRUE(entry.if_contains("threshold"));
        EXPECT_EQ(entry.at("threshold").as_string(), "BLOCK_NONE");
        const bj::string& category = entry.at("category").as_string();
        actual_categories.emplace(category.data(), category.size());
    }

    const std::set<std::string> expected_categories = {
        "HARM_CATEGORY_HARASSMENT",
        "HARM_CATEGORY_HATE_SPEECH",
        "HARM_CATEGORY_SEXUALLY_EXPLICIT",
        "HARM_CATEGORY_DANGEROUS_CONTENT",
        "HARM_CATEGORY_CIVIC_INTEGRITY",
    };
    EXPECT_EQ(actual_categories, expected_categories);
}

TEST(CacheMerge, build_gemini_request_clamps_max_output_tokens_at_boundaries) {
    struct MaxOutputTokensCase {
        std::size_t line_count;
        std::int64_t expected_max_output_tokens;
    };
    constexpr MaxOutputTokensCase kCases[] = {
        {kEmptyLineCount, vn::llm::kMinOutputTokensFloor},
        {kSingleLineCount, vn::llm::kMinOutputTokensFloor},
        {kMaxLinesBelowFloor, vn::llm::kMinOutputTokensFloor},
        {kMinLinesAboveFloor, kExpected55LineTokens},
        {kMaxLinesBelowCeiling, kExpected218LineTokens},
        {kMinLinesAtCeiling, vn::llm::kMaxOutputTokensCeiling},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.line_count);
        std::vector<std::pair<std::string, std::string>> batch;
        batch.reserve(tc.line_count);
        for (std::size_t i = 0; i < tc.line_count; ++i) {
            batch.emplace_back("NARRATION", "境界確認行" + std::to_string(i + 1) + "。");
        }

        const bj::object req = tr::build_gemini_request(batch);
        ASSERT_TRUE(req.if_contains("generationConfig"));
        const bj::object& gen_cfg = req.at("generationConfig").as_object();
        ASSERT_TRUE(gen_cfg.if_contains("maxOutputTokens"));
        EXPECT_EQ(json_int64(gen_cfg.at("maxOutputTokens")), tc.expected_max_output_tokens);
    }
}

TEST(CacheMerge, parse_gemini_response_filters_thought_parts_and_reads_usage_metadata) {
    const bj::value full_response = bj::parse(R"json({
        "candidates": [{
            "finishReason": "STOP",
            "content": {
                "parts": [
                    {"thought": true, "text": "1. \"Draft leak\"\n"},
                    {"text": "1. \"Ah...\"\n"},
                    {"thought": true, "text": "2. Scratchpad reasoning\n"},
                    {"thought": false, "text": "2. Morning came.\n"}
                ]
            }
        }],
        "usageMetadata": {
            "promptTokenCount": 410,
            "candidatesTokenCount": 52,
            "cachedContentTokenCount": 190,
            "thoughtsTokenCount": 317
        }
    })json");

    const tr::ParsedGeminiResponse parsed = tr::parse_gemini_response(full_response);
    EXPECT_EQ(parsed.candidate_text, "1. \"Ah...\"\n2. Morning came.\n");
    EXPECT_EQ(parsed.telemetry.prompt_tokens, kSamplePromptTokens);
    EXPECT_EQ(parsed.telemetry.candidate_tokens, kSampleCandidateTokens);
    EXPECT_EQ(parsed.telemetry.cached_tokens, kSampleCachedTokens);
    EXPECT_EQ(parsed.telemetry.thought_tokens, kSampleThoughtTokens);
    EXPECT_FALSE(parsed.telemetry.blocked);

    const bj::value no_thoughts_field = bj::parse(R"json({
        "candidates": [{
            "finishReason": "STOP",
            "content": {"parts": [{"text": "1. \"Hello\"\n"}]}
        }],
        "usageMetadata": {
            "promptTokenCount": 410,
            "candidatesTokenCount": 52,
            "cachedContentTokenCount": 190
        }
    })json");

    const tr::ParsedGeminiResponse without_thoughts = tr::parse_gemini_response(no_thoughts_field);
    EXPECT_EQ(without_thoughts.candidate_text, "1. \"Hello\"\n");
    EXPECT_EQ(without_thoughts.telemetry.prompt_tokens, kSamplePromptTokens);
    EXPECT_EQ(without_thoughts.telemetry.candidate_tokens, kSampleCandidateTokens);
    EXPECT_EQ(without_thoughts.telemetry.cached_tokens, kSampleCachedTokens);
    EXPECT_EQ(without_thoughts.telemetry.thought_tokens, kZeroThoughtTokens);
    EXPECT_FALSE(without_thoughts.telemetry.blocked);

    const bj::value no_usage = bj::parse(R"json({
        "candidates": [{
            "finishReason": "STOP",
            "content": {"parts": [{"text": "1. \"Solo\"\n"}]}
        }]
    })json");

    const tr::ParsedGeminiResponse without_usage = tr::parse_gemini_response(no_usage);
    EXPECT_EQ(without_usage.candidate_text, "1. \"Solo\"\n");
    EXPECT_EQ(without_usage.telemetry.prompt_tokens, kZeroThoughtTokens);
    EXPECT_EQ(without_usage.telemetry.candidate_tokens, kZeroThoughtTokens);
    EXPECT_EQ(without_usage.telemetry.cached_tokens, kZeroThoughtTokens);
    EXPECT_EQ(without_usage.telemetry.thought_tokens, kZeroThoughtTokens);
    EXPECT_FALSE(without_usage.telemetry.blocked);
}

TEST(CacheMerge, parse_gemini_response_detects_blocked_payloads) {
    struct BlockedCase {
        const char* name;
        const char* raw_json;
        bool expected_blocked;
    };
    constexpr BlockedCase kCases[] = {
        {
            "stop_finish_reason",
            R"json({"candidates":[{"finishReason":"STOP","content":{"parts":[{"text":"1. Ok\n"}]}}]})json",
            false,
        },
        {
            "max_tokens_finish_reason",
            R"json({"candidates":[{"finishReason":"MAX_TOKENS","content":{"parts":[{"text":"1. Ok\n"}]}}]})json",
            false,
        },
        {
            "omitted_finish_reason",
            R"json({"candidates":[{"content":{"parts":[{"text":"1. Ok\n"}]}}]})json",
            false,
        },
        {
            "empty_block_reason_with_stop",
            R"json({"promptFeedback":{"blockReason":""},"candidates":[{"finishReason":"STOP","content":{"parts":[{"text":"1. Ok\n"}]}}]})json",
            false,
        },
        {
            "prompt_feedback_prohibited_content",
            R"json({"promptFeedback":{"blockReason":"PROHIBITED_CONTENT"}})json",
            true,
        },
        {
            "missing_candidates",
            R"json({"usageMetadata":{"promptTokenCount":10}})json",
            true,
        },
        {
            "empty_candidates",
            R"json({"candidates":[]})json",
            true,
        },
        {
            "finish_reason_safety",
            R"json({"candidates":[{"finishReason":"SAFETY"}]})json",
            true,
        },
        {
            "finish_reason_prohibited_content",
            R"json({"candidates":[{"finishReason":"PROHIBITED_CONTENT"}]})json",
            true,
        },
        {
            "finish_reason_blocklist",
            R"json({"candidates":[{"finishReason":"BLOCKLIST"}]})json",
            true,
        },
        {
            "finish_reason_recitation",
            R"json({"candidates":[{"finishReason":"RECITATION"}]})json",
            true,
        },
        {
            "finish_reason_spii",
            R"json({"candidates":[{"finishReason":"SPII"}]})json",
            true,
        },
        {
            "finish_reason_image_safety",
            R"json({"candidates":[{"finishReason":"IMAGE_SAFETY"}]})json",
            true,
        },
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.name);
        const bj::value payload = bj::parse(tc.raw_json);
        const tr::ParsedGeminiResponse parsed = tr::parse_gemini_response(payload);
        EXPECT_EQ(parsed.telemetry.blocked, tc.expected_blocked);
    }
}

TEST(CacheMerge, enforce_thought_telemetry_guard_checks_all_preconditions) {
    struct ThoughtGuardCase {
        const char* name;
        const char* model;
        const char* thinking_level;
        std::size_t batch_line_count;
        std::int64_t thought_tokens;
        bool blocked;
        bool expect_throw;
    };
    constexpr ThoughtGuardCase kCases[] = {
        {"below_min_line_limit", "gemini-4-carbon", "MEDIUM",
         kBelowInitialGuardSlice, kZeroThoughtTokens, false, false},
        {"at_min_line_limit_medium_zero_thoughts", "gemini-4-carbon", "MEDIUM",
         vn::llm::kMinInitialBatchSliceForThoughtCheck, kZeroThoughtTokens, false, true},
        {"uppercase_carbon_high_negative_thoughts", "models/GEMINI-4-CARBON", "HIGH",
         vn::llm::kMinInitialBatchSliceForThoughtCheck, kNegativeThoughtTokens, false, true},
        {"single_thought_token_passes", "gemini-4-carbon", "MEDIUM",
         vn::llm::kMinInitialBatchSliceForThoughtCheck, kSingleThoughtToken, false, false},
        {"blocked_response_exempt", "gemini-4-carbon", "MEDIUM",
         vn::llm::kMinInitialBatchSliceForThoughtCheck, kZeroThoughtTokens, true, false},
        {"low_thinking_level_exempt", "gemini-4-carbon", "LOW",
         vn::llm::kMinInitialBatchSliceForThoughtCheck, kZeroThoughtTokens, false, false},
        {"non_gemini4_model_exempt", "gemini-2.5-pro", "MEDIUM",
         vn::llm::kMinInitialBatchSliceForThoughtCheck, kZeroThoughtTokens, false, false},
        {"disallowed_argon_exempt_from_circuit_breaker", "gemini-4-argon", "HIGH",
         vn::llm::kMinInitialBatchSliceForThoughtCheck, kZeroThoughtTokens, false, false},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.name);
        tr::GeminiUsageTelemetry telemetry{};
        telemetry.thought_tokens = tc.thought_tokens;
        telemetry.blocked = tc.blocked;

        bool threw_zero_thought = false;
        try {
            tr::enforce_thought_telemetry_guard(
                tc.model, tc.thinking_level, tc.batch_line_count, telemetry);
        } catch (const tr::ZeroThoughtError&) {
            threw_zero_thought = true;
        }
        EXPECT_EQ(threw_zero_thought, tc.expect_throw);
    }
}

TEST(CacheMerge, call_gemini_detailed_returns_telemetry_and_rejects_zero_thoughts) {
    FakeGeminiClient fake;
    fake.include_thought_part = true;
    fake.canned_text =
        "1. \"Who goes there?\"\n"
        "2. Footsteps echoed.\n"
        "3. \"Stay close.\"\n"
        "4. The lantern flickered.\n";
    fake.thought_tokens = kFakeThoughtsTokenCount;

    const std::vector<std::pair<std::string, std::string>> batch = {
        {"Ren", "レン\r\n「誰ですか？」"},
        {"NARRATION", "足音が響いた。"},
        {"Yumemi", "夢美\r\n「離れないで。」"},
        {"NARRATION", "ランプが揺れた。"},
    };

    const tr::GeminiBatchResult valid = tr::call_gemini_detailed(fake, batch);
    EXPECT_EQ(fake.last_model, "gemini-4-carbon");
    EXPECT_EQ(fake.call_count, 1u);
    ASSERT_EQ(valid.translations.size(), kDetailedBatchLineCount);
    EXPECT_EQ(valid.translations.at(kFirstLineNumber), "\"Who goes there?\"");
    EXPECT_EQ(valid.translations.at(kSecondLineNumber), "Footsteps echoed.");
    EXPECT_EQ(valid.translations.at(kThirdLineNumber), "\"Stay close.\"");
    EXPECT_EQ(valid.translations.at(kFourthLineNumber), "The lantern flickered.");
    EXPECT_EQ(valid.telemetry.prompt_tokens, kFakePromptTokenCount);
    EXPECT_EQ(valid.telemetry.candidate_tokens, kFakeCandidatesTokenCount);
    EXPECT_EQ(valid.telemetry.cached_tokens, kFakeCachedContentTokenCount);
    EXPECT_EQ(valid.telemetry.thought_tokens, kFakeThoughtsTokenCount);
    EXPECT_FALSE(valid.telemetry.blocked);

    const std::size_t calls_before_argon_ban = fake.call_count;
    EXPECT_THROW(
        tr::call_gemini_detailed(fake, batch, {}, {}, "gemini-4-argon"),
        vn::llm::LlmError);
    EXPECT_EQ(fake.call_count, calls_before_argon_ban);

    fake.thought_tokens = kZeroThoughtTokens;
    EXPECT_THROW(tr::call_gemini_detailed(fake, batch), tr::ZeroThoughtError);

    fake.thought_tokens = kFakeThoughtsTokenCount;
    const tr::GeminiBatchResult recovered = tr::call_gemini_detailed(fake, batch);
    ASSERT_EQ(recovered.translations.size(), kDetailedBatchLineCount);
    EXPECT_EQ(recovered.telemetry.thought_tokens, kFakeThoughtsTokenCount);
}

TEST(CacheMerge, translate_all_sends_context_after_and_aborts_on_zero_thoughts_without_touching_cache) {
    struct ScriptFixtureLine {
        const char* type;
        const char* jp;
        const char* expected_en;
    };
    constexpr std::array<ScriptFixtureLine, kUncachedScriptLineCount> kUncachedLines = {{
        {"narrative", "暗闇の中で足音が響いた。", "Footsteps echoed in the dark."},
        {"dialogue", "レン\r\n「誰かいるのですか？」", "Ren\r\n\"Is someone there?\""},
        {"narrative", "冷たい風が頬を撫でた。", "A cold breeze brushed her cheek."},
        {"dialogue", "夢美\r\n「静かにしてください。」", "Yumemi\r\n\"Please keep quiet.\""},
        {"narrative", "奥の扉がわずかに開いていた。", "The inner door stood slightly ajar."},
        {"dialogue", "レン\r\n「先読み確認用の台詞です。」", "Ren\r\n\"This is the lookahead line.\""},
    }};

    const ScopedTempDir tmp("ren_mushi_ut_translate_all_carbon");
    const fs::path out_dir = tmp.path() / "script_output";
    const fs::path game_dir = tmp.path() / "game";
    fs::create_directories(out_dir);
    fs::create_directories(game_dir);

    bj::array cached_gate_strings;
    tr::Cache seeded_cache;
    const auto& speakers = tr::name_translations_ordered();
    const std::size_t gate_line_count = std::max(kSpeakerGateSeedFloor, speakers.size());
    for (std::size_t i = 0; i < gate_line_count; ++i) {
        const auto& [jp_speaker, en_speaker] = speakers[i % speakers.size()];
        const std::string jp_line =
            jp_speaker + "\r\n「既訳ゲート確認行その" + std::to_string(i + 1) + "です。」";
        const std::string en_line =
            en_speaker + "\r\n\"Cached speaker gate line " + std::to_string(i + 1) + ".\"";
        cached_gate_strings.push_back(make_extracted_entry("dialogue", jp_line));
        seeded_cache.set(jp_line, en_line);
    }

    bj::array mushi_strings;
    for (const auto& row : kUncachedLines) {
        mushi_strings.push_back(make_extracted_entry(row.type, row.jp));
    }

    bj::object extracted_root;
    extracted_root["mushi.spt"] = bj::object{{"strings", std::move(mushi_strings)}};
    extracted_root["backscript.spt"] = bj::object{{"strings", std::move(cached_gate_strings)}};

    const tr::SpeakerCheckReport gate_report = tr::run_speaker_checks(extracted_root);
    ASSERT_TRUE(gate_report.passed) << gate_report.reason;

    const std::string input_path = (out_dir / "extracted_text.json").u8string();
    const std::string cache_path = (out_dir / "translation_cache_gemini.json").u8string();
    const std::string output_path = (out_dir / "translated_text.json").u8string();
    const std::string test_dir_path = (out_dir / "test_batches").u8string();
    const std::string tsv_path = (game_dir / "translation_table.tsv").u8string();

    exm::write_file_text(input_path, exm::json_pretty(bj::value(std::move(extracted_root))));
    tr::save_cache(seeded_cache, cache_path);
    const std::string cache_bytes_before = file_bytes(cache_path);
    ASSERT_EQ(tr::cache_entry_count(cache_path), seeded_cache.size());

    FakeTranslateSession session;
    session.canned_batch_responses = {
        "1. Footsteps echoed in the dark.\n"
        "2. \"Is someone there?\"\n"
        "3. A cold breeze brushed her cheek.\n"
        "4. \"Please keep quiet.\"\n",
        "1. The inner door stood slightly ajar.\n"
        "2. \"This is the lookahead line.\"\n",
    };
    session.thought_tokens = kZeroThoughtTokens;

    tr::TranslateOptions opt;
    opt.batch_size = kTranslateBatchSize;
    opt.input_file = input_path;
    opt.cache_file = cache_path;
    opt.output_file = output_path;
    opt.test_dir = test_dir_path;
    opt.tsv_file = tsv_path;
    opt.client_factory = [&session] {
        return std::make_unique<SessionGeminiClient>(&session);
    };

    EXPECT_THROW(tr::translate_all(opt), tr::ZeroThoughtError);
    ASSERT_EQ(session.requests.size(), kExpectedCallsOnAbort);
    EXPECT_EQ(session.requests[0].model, "gemini-4-carbon");
    EXPECT_EQ(file_bytes(cache_path), cache_bytes_before);
    EXPECT_FALSE(fs::exists(fs::u8path(output_path)));
    EXPECT_FALSE(fs::exists(fs::u8path(tsv_path)));

    const bj::object& first_req = session.requests[0].body.as_object();
    ASSERT_TRUE(first_req.if_contains("contents"));
    const bj::array& contents = first_req.at("contents").as_array();
    ASSERT_EQ(contents.size(), kSingleElementCount);
    const bj::array& parts = contents[0].as_object().at("parts").as_array();
    ASSERT_EQ(parts.size(), kSingleElementCount);
    const bj::string& raw_prompt = parts[0].as_object().at("text").as_string();
    const std::string prompt_text(raw_prompt.data(), raw_prompt.size());

    const std::size_t lines_tag_pos = prompt_text.find("<lines_to_translate>");
    const std::size_t after_tag_pos = prompt_text.find("<context_after>");
    ASSERT_NE(lines_tag_pos, std::string::npos);
    ASSERT_NE(after_tag_pos, std::string::npos);
    EXPECT_LT(lines_tag_pos, after_tag_pos);

    const std::string after_section = prompt_text.substr(after_tag_pos);
    EXPECT_NE(after_section.find("奥の扉がわずかに開いていた。"), std::string::npos);
    EXPECT_NE(after_section.find("先読み確認用の台詞です。"), std::string::npos);
    EXPECT_EQ(after_section.find("暗闇の中で足音が響いた。"), std::string::npos);

    session.requests.clear();
    session.thought_tokens = kFakeThoughtsTokenCount;
    ASSERT_EQ(tr::translate_all(opt), 0);
    ASSERT_EQ(session.requests.size(), kExpectedCallsOnComplete);

    const tr::Cache saved_cache = tr::load_cache(cache_path);
    ASSERT_EQ(saved_cache.size(),
              seeded_cache.size() + speakers.size() + kUncachedLines.size());
    ASSERT_EQ(speakers.size(), tr::name_translations().size());
    for (const auto& [jp_speaker, en_speaker] : speakers) {
        SCOPED_TRACE(jp_speaker);
        const std::string* cached_speaker = saved_cache.get(jp_speaker);
        ASSERT_NE(cached_speaker, nullptr);
        EXPECT_EQ(*cached_speaker, en_speaker);
    }
    for (const auto& row : kUncachedLines) {
        SCOPED_TRACE(row.jp);
        const std::string* translated = saved_cache.get(row.jp);
        ASSERT_NE(translated, nullptr);
        EXPECT_EQ(*translated, row.expected_en);
    }
    EXPECT_TRUE(fs::exists(fs::u8path(output_path)));
    EXPECT_TRUE(fs::exists(fs::u8path(tsv_path)));
}
