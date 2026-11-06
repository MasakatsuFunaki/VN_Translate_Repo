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
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "cache/cache_core.h"
#include "cache/gemini_client.h"
#include "cache/glossary.h"
#include "common/util.h"

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

std::string scratch_cache() {
    return (fs::temp_directory_path() / "ageha_ut_translation_cache_gemini.json").u8string();
}

std::string scratch_purge_cache() {
    return (fs::temp_directory_path() / "ageha_ut_cache_purge_gemini.json").u8string();
}

std::string file_bytes(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

class FakeGeminiClient final : public gemini::Client {
public:
    std::string last_model;
    bj::value last_body;
    std::string canned_text = "1. \"Ah...\"\n2. Morning came.\n";

    bj::value generate_content(const std::string& model, const bj::value& body) override {
        last_model = model;
        last_body = body;
        bj::object text_part{{"text", canned_text}};
        bj::object content{{"parts", bj::array{std::move(text_part)}}};
        bj::object candidate{{"content", std::move(content)}};
        bj::object usage{
            {"promptTokenCount", kFakePromptTokenCount},
            {"candidatesTokenCount", kFakeCandidatesTokenCount},
            {"cachedContentTokenCount", kFakeCachedContentTokenCount},
        };
        return bj::object{
            {"candidates", bj::array{std::move(candidate)}},
            {"usageMetadata", std::move(usage)},
        };
    }
};

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

TEST(CacheMerge, extract_speaker_requires_quote_bracket_and_maps_ageha_speakers) {
    EXPECT_EQ(tr::extract_speaker("アゲハ\r\n「あ」"), "Ageha");
    EXPECT_EQ(tr::extract_speaker("シュミ\r\n『ええ』"), "Shumi");
    EXPECT_EQ(tr::extract_speaker("その一心から、\r\nアゲハは"), "NARRATION");
    EXPECT_EQ(tr::extract_speaker("あ"), "NARRATION");
    EXPECT_EQ(tr::extract_speaker("未知\r\n「x」"), "未知");
}

TEST(CacheMerge, sanitize_ascii_replaces_smart_punctuation) {
    EXPECT_EQ(tr::sanitize_ascii("a—b"), "a--b");
    EXPECT_EQ(tr::sanitize_ascii("“hi”"), "\"hi\"");
    EXPECT_EQ(tr::sanitize_ascii("…"), "...");
    EXPECT_EQ(tr::sanitize_ascii("a　b"), "a b");
}

TEST(CacheMerge, postprocess_reattaches_english_speaker_plate_and_trailing_crlf) {
    const std::string original = "アゲハ\r\n「あ」";
    EXPECT_EQ(tr::postprocess(original, "\"Ah\""), "Ageha\r\n\"Ah\"");
    EXPECT_EQ(tr::postprocess(original, "アゲハ\r\n\"Ah\""), "Ageha\r\n\"Ah\"");
    EXPECT_EQ(tr::postprocess(original, "Ageha\r\n\"Ah\""), "Ageha\r\n\"Ah\"");

    EXPECT_EQ(tr::postprocess("あ\r\n", "Ah"), "Ah\r\n");
    EXPECT_EQ(tr::postprocess("あ\r\n", "Ah\r\n"), "Ah\r\n");
    EXPECT_EQ(tr::postprocess("あ", ""), "");
    EXPECT_EQ(tr::postprocess("あ", "あ"), "あ");
}

TEST(CacheMerge, parses_numbered_response_and_unescapes) {
    auto t = tr::parse_numbered_response("1. Ageha\\r\\n\"Ah\"\n2) Second\n");
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[1], "Ageha\r\n\"Ah\"");
    EXPECT_EQ(t[2], "Second");
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

TEST(CacheMerge, call_gemini_sends_gemini_4_argon_payload_and_parses_candidates) {
    FakeGeminiClient fake;
    const std::vector<std::pair<std::string, std::string>> batch = {
        {"Ageha", "アゲハ\r\n「あ……」"},
        {"NARRATION", "朝が来た。"},
    };
    const auto result = tr::call_gemini(fake, batch, {}, {});
    EXPECT_EQ(fake.last_model, "gemini-4-argon");
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

    const std::string path = scratch_purge_cache();
    const std::string expected = scratch_purge_cache() + ".expected";

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

    fs::remove(fs::u8path(path));
    fs::remove(fs::u8path(expected));
}

TEST(CacheMerge, disk_cache_io_handles_missing_and_malformed_files) {
    const std::string path = scratch_cache();
    fs::remove(fs::u8path(path));
    EXPECT_EQ(tr::load_cache(path).size(), 0u);
    EXPECT_EQ(tr::cache_entry_count(path), 0u);

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
    fs::remove(fs::u8path(path));
}
