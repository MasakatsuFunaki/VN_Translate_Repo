// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Translation cache, speaker parsing, name-plate postprocessing, and response
// parser tests.
#include "cache/cache_core.h"

#include "cache/glossary.h"
#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace tr = mgi::translate;
using namespace mgi;

namespace {

constexpr const char* kMiyakaJp = "美弥香";
constexpr const char* kOpenQuoteJp = "「";
constexpr const char* kCloseQuoteJp = "」";

constexpr std::size_t kPaidCacheLineCount = 56103u;
constexpr std::size_t kExpectedPurgedEchoes = 2u;
constexpr std::size_t kExpectedSurvivingEntries = 3u;

}  // namespace

TEST(Translate, has_real_japanese_excludes_fullwidth) {
    EXPECT_TRUE(tr::has_real_japanese("あ"));
    EXPECT_TRUE(tr::has_real_japanese(kMiyakaJp));
    // Fullwidth Latin and CJK punctuation are NOT "real Japanese" here, even
    // though the extractor's wider test accepts them.
    EXPECT_FALSE(tr::has_real_japanese("Ａ"));
    EXPECT_FALSE(tr::has_real_japanese("「"));
}

TEST(Translate, needs_translation_rules) {
    EXPECT_TRUE(tr::needs_translation("あい"));
    EXPECT_FALSE(tr::needs_translation(""));
    EXPECT_FALSE(tr::needs_translation("   "));
    EXPECT_FALSE(tr::needs_translation("abc"));
    EXPECT_FALSE(tr::needs_translation("abcd"));  // no real Japanese at all
}

TEST(Translate, extract_speaker_needs_a_quote_bracket) {
    // Name + bracketed dialogue -> resolved speaker.
    EXPECT_EQ(tr::extract_speaker(std::string(kMiyakaJp) + "\r\n" + kOpenQuoteJp + "あ" +
                                  kCloseQuoteJp),
              "Miyaka");
    // Two-line narrative without dialogue brackets is classified as narration.
    EXPECT_EQ(tr::extract_speaker("その一心から、\r\n美弥香は"), "NARRATION");
    // No CRLF at all.
    EXPECT_EQ(tr::extract_speaker("あ"), "NARRATION");
    // Unknown JP name passes through verbatim.
    EXPECT_EQ(tr::extract_speaker(std::string("未知") + "\r\n" + kOpenQuoteJp + "x" +
                                  kCloseQuoteJp),
              "未知");
}

TEST(Translate, sanitize_ascii_replaces_smart_punctuation) {
    EXPECT_EQ(tr::sanitize_ascii("a—b"), "a--b");
    EXPECT_EQ(tr::sanitize_ascii("“hi”"), "\"hi\"");
    EXPECT_EQ(tr::sanitize_ascii("…"), "...");
    EXPECT_EQ(tr::sanitize_ascii("a　b"), "a b");
}

// The engine renders the text before the first CRLF as the name plate, so the
// English name has to be re-attached there.
TEST(Translate, postprocess_reattaches_english_name_plate) {
    const std::string original =
        std::string(kMiyakaJp) + "\r\n" + kOpenQuoteJp + "あ" + kCloseQuoteJp;
    EXPECT_EQ(tr::postprocess(original, "\"Ah\""), "Miyaka\r\n\"Ah\"");
    // If the model kept the Japanese plate, it is swapped rather than duplicated.
    EXPECT_EQ(tr::postprocess(original, std::string(kMiyakaJp) + "\r\n\"Ah\""),
              "Miyaka\r\n\"Ah\"");
    // Already-correct output is left alone.
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

// The response parser un-escapes \r\n so the engine gets real line breaks.
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

// The guard refuses flags that would throw away a paid cache until
// --discard-cache confirms the intent.
TEST(Translate, a_flag_that_discards_a_paid_cache_is_refused_until_it_is_meant) {
    // An empty or nearly empty cache is what a from-scratch run was written for.
    EXPECT_FALSE(tr::refuse_cache_discard(0, "--test", false).has_value());
    EXPECT_FALSE(tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD, "--test",
                                          false).has_value());

    // One entry past the threshold, every flag is refused.
    for (const char* flag : {"--test", "--retranslate", "--clean"}) {
        auto why = tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD + 1, flag,
                                            false);
        ASSERT_TRUE(why.has_value()) << flag;
        EXPECT_NE(why->find(flag), std::string::npos) << *why;
        EXPECT_NE(why->find("--discard-cache"), std::string::npos) << *why;
    }

    // The count is in the message, because "some lines" is not a reason to
    // stop and "56103 lines" is.
    auto why = tr::refuse_cache_discard(kPaidCacheLineCount, "--test", false);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("56103"), std::string::npos) << *why;

    // --discard-cache is the caller saying they meant it.
    EXPECT_FALSE(tr::refuse_cache_discard(kPaidCacheLineCount, "--test", true).has_value());
}

// An echo -- the model handing back the line it was given -- is a failure only
// when the line is Japanese.  An English string that survives translation
// unchanged is a correct answer, so pure ASCII is never a failure.
TEST(Translate, failed_entry_predicate_only_fires_on_japanese_identity) {
    EXPECT_TRUE(tr::is_failed_entry("こんにちは", "こんにちは"));
    EXPECT_TRUE(tr::is_failed_entry("……", "……"));  // non-ASCII, still an echo
    // A resource name carrying one stray Japanese byte is an echo like any
    // other: the run re-requests it and pays for it again.
    EXPECT_TRUE(tr::is_failed_entry("Eスbg152n", "Eスbg152n"));

    EXPECT_FALSE(tr::is_failed_entry("こんにちは", "Hello"));
    EXPECT_FALSE(tr::is_failed_entry("朝が来た。", "Morning came."));
    EXPECT_FALSE(tr::is_failed_entry("OK", "OK"));          // ASCII identity is right
    EXPECT_FALSE(tr::is_failed_entry("bg152n", "bg152n"));  // ASCII identifier
    EXPECT_FALSE(tr::is_failed_entry("", ""));
}

// The purge takes out exactly the echoes and leaves everything else where it
// was, key order included -- that order is the cache file's key order.
TEST(Translate, purge_removes_failed_entries_and_keeps_the_rest) {
    tr::Cache cache;
    cache.set("朝が来た。", "Morning came.");
    cache.set("こんにちは", "こんにちは");
    cache.set("OK", "OK");
    cache.set("Eスbg152n", "Eスbg152n");
    cache.set("bg152n", "bg152n");

    EXPECT_EQ(tr::purge_failed_entries(cache), kExpectedPurgedEchoes);
    EXPECT_EQ(cache.size(), kExpectedSurvivingEntries);
    EXPECT_FALSE(cache.contains("こんにちは"));
    EXPECT_FALSE(cache.contains("Eスbg152n"));
    ASSERT_TRUE(cache.get("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    EXPECT_TRUE(cache.contains("OK"));
    EXPECT_TRUE(cache.contains("bg152n"));

    const auto& items = cache.items();
    ASSERT_EQ(items.size(), kExpectedSurvivingEntries);
    EXPECT_EQ(items[0].first, "朝が来た。");
    EXPECT_EQ(items[1].first, "OK");
    EXPECT_EQ(items[2].first, "bg152n");

    // Nothing left to remove on a second pass.
    EXPECT_EQ(tr::purge_failed_entries(cache), 0u);
    EXPECT_EQ(cache.size(), kExpectedSurvivingEntries);
}
