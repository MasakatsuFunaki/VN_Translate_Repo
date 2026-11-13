// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for cache_core: speaker parsing off raw line text, postprocess
// name-plate re-attachment, response parsing, and echo purging.
#include "cache/cache_core.h"
#include "cache/glossary.h"
#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace tr = exm::translate;
using namespace exm;

namespace {

constexpr const char* kMiyaka = "美弥香";
constexpr const char* kOpenBracket = "「";
constexpr const char* kCloseBracket = "」";
constexpr std::size_t kLargePaidCacheSize = 56103;

}  // namespace

TEST(Translate, has_real_japanese_excludes_fullwidth) {
    EXPECT_TRUE(tr::has_real_japanese("あ"));
    EXPECT_TRUE(tr::has_real_japanese(kMiyaka));
    EXPECT_FALSE(tr::has_real_japanese("Ａ"));
    EXPECT_FALSE(tr::has_real_japanese("「"));
}

TEST(Translate, needs_translation_rules) {
    EXPECT_TRUE(tr::needs_translation("あい"));
    EXPECT_FALSE(tr::needs_translation(""));
    EXPECT_FALSE(tr::needs_translation("   "));
    EXPECT_FALSE(tr::needs_translation("abc"));
    EXPECT_FALSE(tr::needs_translation("abcd"));
}

TEST(Translate, extract_speaker_needs_a_quote_bracket) {
    EXPECT_EQ(tr::extract_speaker(std::string(kMiyaka) + "\r\n" + kOpenBracket + "あ" +
                                  kCloseBracket),
              "Miyaka");
    EXPECT_EQ(tr::extract_speaker("その一心から、\r\n美弥香は"), "NARRATION");
    EXPECT_EQ(tr::extract_speaker("あ"), "NARRATION");
    EXPECT_EQ(tr::extract_speaker(std::string("未知") + "\r\n" + kOpenBracket + "x" +
                                  kCloseBracket),
              "未知");
}

TEST(Translate, sanitize_ascii_replaces_smart_punctuation) {
    EXPECT_EQ(tr::sanitize_ascii("a—b"), "a--b");
    EXPECT_EQ(tr::sanitize_ascii("“hi”"), "\"hi\"");
    EXPECT_EQ(tr::sanitize_ascii("…"), "...");
    EXPECT_EQ(tr::sanitize_ascii("a　b"), "a b");
}

TEST(Translate, postprocess_reattaches_english_name_plate) {
    const std::string original =
        std::string(kMiyaka) + "\r\n" + kOpenBracket + "あ" + kCloseBracket;
    EXPECT_EQ(tr::postprocess(original, "\"Ah\""), "Miyaka\r\n\"Ah\"");
    EXPECT_EQ(tr::postprocess(original, std::string(kMiyaka) + "\r\n\"Ah\""),
              "Miyaka\r\n\"Ah\"");
    EXPECT_EQ(tr::postprocess(original, "Miyaka\r\n\"Ah\""), "Miyaka\r\n\"Ah\"");
}

TEST(Translate, postprocess_restores_trailing_crlf) {
    const std::string original = "あ\r\n";
    EXPECT_EQ(tr::postprocess(original, "Ah"), "Ah\r\n");
    EXPECT_EQ(tr::postprocess(original, "Ah\r\n"), "Ah\r\n");
}

TEST(Translate, postprocess_passes_empty_and_identical_through) {
    EXPECT_EQ(tr::postprocess("あ", ""), "");
    EXPECT_EQ(tr::postprocess("あ", "あ"), "あ");
}

TEST(Translate, parses_numbered_response_and_unescapes) {
    auto t = tr::parse_numbered_response("1. Miyaka\\r\\n\"Ah\"\n2) Second\n");
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[1], "Miyaka\r\n\"Ah\"");
    EXPECT_EQ(t[2], "Second");
}

TEST(Translate, parses_and_strips_leaked_speaker_tag) {
    auto t = tr::parse_numbered_response("[NARRATION] 1. Snow fell.\n");
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[1], "Snow fell.");
}

TEST(Translate, glossary_and_story_order_are_well_formed) {
    const auto& ordered = tr::name_translations_ordered();
    EXPECT_FALSE(ordered.empty());
    for (const auto& [jp, en] : ordered) {
        EXPECT_FALSE(jp.empty());
    }
    EXPECT_EQ(ordered.size(), tr::name_translations().size())
        << "duplicate JP key in the glossary";
    EXPECT_FALSE(tr::story_order().empty());
}

TEST(Translate, a_flag_that_discards_a_paid_cache_is_refused_until_it_is_meant) {
    EXPECT_FALSE(tr::refuse_cache_discard(0, "--test", false).has_value());
    EXPECT_FALSE(
        tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD, "--test", false).has_value());

    for (const char* flag : {"--test", "--retranslate", "--clean"}) {
        auto why = tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD + 1, flag, false);
        ASSERT_TRUE(why.has_value()) << flag;
        EXPECT_NE(why->find(flag), std::string::npos) << *why;
        EXPECT_NE(why->find("--max-batches"), std::string::npos) << *why;
        EXPECT_NE(why->find("--discard-cache"), std::string::npos) << *why;
    }

    auto why = tr::refuse_cache_discard(kLargePaidCacheSize, "--test", false);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("56103"), std::string::npos) << *why;

    EXPECT_FALSE(tr::refuse_cache_discard(kLargePaidCacheSize, "--test", true).has_value());
}

TEST(Translate, failed_entry_predicate_only_fires_on_japanese_identity) {
    EXPECT_TRUE(tr::is_failed_entry("こんにちは", "こんにちは"));
    EXPECT_TRUE(tr::is_failed_entry("……", "……"));
    EXPECT_TRUE(tr::is_failed_entry("Eスbg152n", "Eスbg152n"));

    EXPECT_FALSE(tr::is_failed_entry("こんにちは", "Hello"));
    EXPECT_FALSE(tr::is_failed_entry("朝が来た。", "Morning came."));
    EXPECT_FALSE(tr::is_failed_entry("OK", "OK"));
    EXPECT_FALSE(tr::is_failed_entry("bg152n", "bg152n"));
    EXPECT_FALSE(tr::is_failed_entry("", ""));
}

TEST(Translate, purge_removes_failed_entries_and_keeps_the_rest) {
    tr::Cache cache;
    cache.set("朝が来た。", "Morning came.");
    cache.set("こんにちは", "こんにちは");
    cache.set("OK", "OK");
    cache.set("Eスbg152n", "Eスbg152n");
    cache.set("bg152n", "bg152n");

    EXPECT_EQ(tr::purge_failed_entries(cache), 2u);
    EXPECT_EQ(cache.size(), 3u);
    EXPECT_FALSE(cache.contains("こんにちは"));
    EXPECT_FALSE(cache.contains("Eスbg152n"));
    ASSERT_TRUE(cache.get("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    EXPECT_TRUE(cache.contains("OK"));
    EXPECT_TRUE(cache.contains("bg152n"));

    const auto& items = cache.items();
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].first, "朝が来た。");
    EXPECT_EQ(items[1].first, "OK");
    EXPECT_EQ(items[2].first, "bg152n");

    EXPECT_EQ(tr::purge_failed_entries(cache), 0u);
    EXPECT_EQ(cache.size(), 3u);
}
