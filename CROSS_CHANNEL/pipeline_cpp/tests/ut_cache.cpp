// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Translation cache and postprocessing unit tests: speaker resolution off the
// JSON `speaker` field, name-plate re-attachment, response parsing, cache
// ordering, and glossary tables.
#include "cache/cache_core.h"
#include "cache/glossary.h"
#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace bj = boost::json;
namespace tr = crc::translate;
using namespace crc;

namespace {

constexpr const char* kTaichi = "太一";
constexpr const char* kOpenQuote = "「";
constexpr const char* kCloseQuote = "」";
constexpr int kMaxSpeakerCodepoints = 20;
constexpr int kTooLongSpeakerCodepoints = 21;
constexpr int kLineOne = 1;
constexpr int kLineTwo = 2;
constexpr int kLineThree = 3;
constexpr std::size_t kExpectedParsedTwo = 2u;
constexpr std::size_t kExpectedParsedOne = 1u;
constexpr std::size_t kExpectedCacheTwo = 2u;
constexpr std::size_t kExpectedCacheThree = 3u;
constexpr std::size_t kExpectedGlossarySize = 75u;
constexpr std::size_t kExpectedStoryFiles = 1u;
constexpr std::size_t kPaidCacheCount = 56103u;
constexpr std::size_t kExpectedPurgedCount = 2u;
constexpr std::size_t kSecondItemIndex = 2u;

bj::object entry(const std::string& speaker, const std::string& text,
                 const std::string& type = "dialogue") {
    bj::object o;
    o["speaker"] = speaker;
    o["text"] = text;
    o["type"] = type;
    return o;
}

}  // namespace

TEST(Translate, NeedsTranslationTable) {
    EXPECT_TRUE(tr::needs_translation("こんにちは"));
    EXPECT_TRUE(tr::needs_translation("カタカナ"));
    EXPECT_TRUE(tr::needs_translation("漢字"));
    EXPECT_FALSE(tr::needs_translation("Hello"));
    EXPECT_FALSE(tr::needs_translation(""));
    EXPECT_FALSE(tr::needs_translation("   "));
    EXPECT_FALSE(tr::needs_translation("……"));
    EXPECT_FALSE(tr::needs_translation("ABC123"));
}

TEST(Translate, ExtractSpeakerReadsSpeakerFieldNotText) {
    EXPECT_EQ(tr::extract_speaker(entry(kTaichi, std::string(kOpenQuote) + "…" + kCloseQuote)),
              "Taichi");
    EXPECT_EQ(tr::extract_speaker(
                  entry("", std::string(kTaichi) + "\r\n" + kOpenQuote + "…" + kCloseQuote)),
              "Taichi");
    EXPECT_EQ(tr::extract_speaker(entry("未知", "x")), "未知");

    std::string long_name;
    for (int i = 0; i < kTooLongSpeakerCodepoints; ++i) {
        long_name += "あ";
    }
    EXPECT_EQ(tr::extract_speaker(entry(long_name, "x")), "NARRATION");

    std::string max_len_name;
    for (int i = 0; i < kMaxSpeakerCodepoints; ++i) {
        max_len_name += "あ";
    }
    EXPECT_EQ(tr::extract_speaker(entry(max_len_name, "x")), max_len_name);
    EXPECT_EQ(tr::extract_speaker(entry("", "no crlf here")), "NARRATION");
}

TEST(Translate, PostprocessReattachesSpeakerAndSanitises) {
    const std::string original =
        std::string(kTaichi) + "\r\n" + kOpenQuote + "ふん" + kCloseQuote;
    const std::string got = tr::postprocess(original, "Hmph.");
    EXPECT_EQ(got, "Taichi\r\nHmph.");

    EXPECT_EQ(tr::postprocess(original, std::string(kTaichi) + "\r\nHmph."), "Taichi\r\nHmph.");
    EXPECT_EQ(tr::postprocess(original, "Taichi\r\nHmph."), "Taichi\r\nHmph.");

    EXPECT_EQ(tr::postprocess("日本語", "“Hello” — nope…"), "\"Hello\" -- nope...");

    EXPECT_EQ(tr::postprocess("日本語\r\n", "Japanese  "), "Japanese\r\n");
    EXPECT_EQ(tr::postprocess("same", "same"), "same");
    EXPECT_EQ(tr::postprocess("x", ""), "");
}

TEST(Translate, SanitizeAsciiReplacesSmartPunctuation) {
    EXPECT_EQ(tr::sanitize_ascii("a—b"), "a--b");
    EXPECT_EQ(tr::sanitize_ascii("a–b"), "a--b");
    EXPECT_EQ(tr::sanitize_ascii("‘x’"), "'x'");
    EXPECT_EQ(tr::sanitize_ascii("a　b"), "a b");
}

TEST(Translate, ParseNumberedResponseStripsSpeakerTagsAndUnescapes) {
    auto t = tr::parse_numbered_response("1. [Taichi] Hello\\r\\nWorld\n\n[NARRATION] 2) Foo\n");
    ASSERT_EQ(t.size(), kExpectedParsedTwo);
    EXPECT_EQ(t[kLineOne], "[Taichi] Hello\r\nWorld");
    EXPECT_EQ(t[kLineTwo], "Foo");

    auto t2 = tr::parse_numbered_response("1. a\\r\\nb\\nc");
    EXPECT_EQ(t2[kLineOne], "a\r\nb\nc");

    auto t3 = tr::parse_numbered_response("\n  \nSure, here you go:\n3: Bar");
    ASSERT_EQ(t3.size(), kExpectedParsedOne);
    EXPECT_EQ(t3[kLineThree], "Bar");
}

TEST(Translate, CacheIsInsertionOrderedAndUpdateKeepsPosition) {
    tr::Cache c;
    c.set("a", "1");
    c.set("b", "2");
    c.set("a", "3");
    ASSERT_EQ(c.size(), kExpectedCacheTwo);
    EXPECT_EQ(c.items()[0].first, "a");
    EXPECT_EQ(c.items()[0].second, "3");
    EXPECT_EQ(c.items()[1].first, "b");
}

TEST(Translate, GlossaryHasSeventyFivePairsInSourceOrder) {
    const auto& g = tr::name_translations_ordered();
    ASSERT_EQ(g.size(), kExpectedGlossarySize);
    EXPECT_EQ(g.front().first, kTaichi);
    EXPECT_EQ(g.front().second, "Taichi");

    const auto& m = tr::name_translations();
    EXPECT_EQ(m.size(), kExpectedGlossarySize);
    EXPECT_EQ(m.at("桜庭・友貴"), "Sakuraba & Tomoki");
    EXPECT_EQ(m.at("太一（妄想）"), "Taichi (Delusion)");
    EXPECT_EQ(m.at("＊＊"), "**");

    ASSERT_EQ(tr::story_order().size(), kExpectedStoryFiles);
    EXPECT_EQ(tr::story_order()[0], "sn.bin");
}

TEST(Translate, AFlagThatDeletesAPaidCacheIsRefusedUntilItIsMeant) {
    EXPECT_FALSE(tr::refuse_cache_discard(0, "--test", false).has_value());
    EXPECT_FALSE(tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD, "--test",
                                          false).has_value());

    for (const char* flag : {"--test", "--clean"}) {
        const auto why = tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD + 1, flag,
                                                  false);
        ASSERT_TRUE(why.has_value()) << flag;
        EXPECT_NE(why->find(flag), std::string::npos) << *why;
        EXPECT_NE(why->find("--discard-cache"), std::string::npos) << *why;
    }

    const auto why = tr::refuse_cache_discard(kPaidCacheCount, "--test", false);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("56103"), std::string::npos) << *why;

    EXPECT_FALSE(tr::refuse_cache_discard(kPaidCacheCount, "--test", true).has_value());
}

TEST(Translate, FailedEntryPredicateOnlyFiresOnJapaneseIdentity) {
    EXPECT_TRUE(tr::is_failed_entry("こんにちは", "こんにちは"));
    EXPECT_TRUE(tr::is_failed_entry("……", "……"));
    EXPECT_TRUE(tr::is_failed_entry("Eスbg152n", "Eスbg152n"));

    EXPECT_FALSE(tr::is_failed_entry("こんにちは", "Hello"));
    EXPECT_FALSE(tr::is_failed_entry("朝が来た。", "Morning came."));
    EXPECT_FALSE(tr::is_failed_entry("OK", "OK"));
    EXPECT_FALSE(tr::is_failed_entry("bg152n", "bg152n"));
    EXPECT_FALSE(tr::is_failed_entry("", ""));
}

TEST(Translate, PurgeRemovesFailedEntriesAndKeepsTheRest) {
    tr::Cache cache;
    cache.set("朝が来た。", "Morning came.");
    cache.set("こんにちは", "こんにちは");
    cache.set("OK", "OK");
    cache.set("Eスbg152n", "Eスbg152n");
    cache.set("bg152n", "bg152n");

    EXPECT_EQ(tr::purge_failed_entries(cache), kExpectedPurgedCount);
    EXPECT_EQ(cache.size(), kExpectedCacheThree);
    EXPECT_FALSE(cache.contains("こんにちは"));
    EXPECT_FALSE(cache.contains("Eスbg152n"));
    ASSERT_TRUE(cache.get("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    EXPECT_TRUE(cache.contains("OK"));
    EXPECT_TRUE(cache.contains("bg152n"));

    const auto& items = cache.items();
    ASSERT_EQ(items.size(), kExpectedCacheThree);
    EXPECT_EQ(items[0].first, "朝が来た。");
    EXPECT_EQ(items[1].first, "OK");
    EXPECT_EQ(items[kSecondItemIndex].first, "bg152n");

    EXPECT_EQ(tr::purge_failed_entries(cache), 0u);
    EXPECT_EQ(cache.size(), kExpectedCacheThree);
}
