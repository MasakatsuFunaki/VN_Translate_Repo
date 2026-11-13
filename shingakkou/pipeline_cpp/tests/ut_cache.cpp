// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Translation cache tests: the translatability predicates, the speaker lookup
// pattern, and response parsing.  Several of these exist specifically to catch
// a copy-paste from the BLACKCyc ports, whose equivalents differ.
#include "cache/cache_core.h"
#include "cache/glossary.h"
#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace bj = boost::json;
using namespace shin;
namespace tr = shin::translate;

namespace {

constexpr int kControlCodeMaxJpChars = 48;
constexpr std::size_t kControlCodeMaxAllowedLen = 49u;
constexpr std::size_t kControlCodeRejectedLen = 50u;
constexpr int kTagSampleNum3 = 3;
constexpr int kTagSampleNum5 = 5;
constexpr int kTagSampleNum12 = 12;
constexpr int kFirstLineNumber = 1;
constexpr std::size_t kSingleEntryCount = 1u;
constexpr std::size_t kEscapedCrLfLiteralLen = 6u;
constexpr std::size_t kReseedEntryCount = 3u;
constexpr std::size_t kExpectedGlossaryNameCount = 75u;
constexpr std::size_t kExpectedStoryScriptCount = 91u;
constexpr std::size_t kEmptyCacheCount = 0u;
constexpr std::size_t kOnePastThresholdOffset = 1u;
constexpr std::size_t kPaidCacheLineCount = 56103u;
constexpr std::size_t kExpectedPurgedEchoCount = 2u;
constexpr std::size_t kExpectedSurvivingCacheCount = 3u;
constexpr std::size_t kIdx0 = 0u;
constexpr std::size_t kIdx1 = 1u;
constexpr std::size_t kIdx2 = 2u;

bj::object rec(const char* type, const char* speaker, const char* text = "x") {
    bj::object o;
    o["type"] = type;
    o["speaker"] = speaker;
    o["text"] = text;
    return o;
}

}  // namespace

TEST(Translate, is_control_code_rules) {
    EXPECT_TRUE(tr::is_control_code("何か@です"));            // '@' anywhere wins
    EXPECT_TRUE(tr::is_control_code("chr_a_02"));            // short, no JP punctuation
    EXPECT_FALSE(tr::is_control_code("chr_a_02。"));          // 。 disqualifies
    EXPECT_FALSE(tr::is_control_code("chr_a_02「x」"));
    EXPECT_FALSE(tr::is_control_code("chr_a_02……"));
    EXPECT_FALSE(tr::is_control_code("chr_a_02―"));  // U+2015
    EXPECT_FALSE(tr::is_control_code("chr_a_02　"));  // U+3000
    EXPECT_FALSE(tr::is_control_code("no underscore here"));

    // The length bound counts CODEPOINTS.  49 Japanese characters plus '_' is
    // 150 bytes; a byte test would make this false and change the cache keys.
    std::string jp49;
    for (int i = 0; i < kControlCodeMaxJpChars; ++i) {
        jp49 += "あ";
    }
    EXPECT_EQ(char_len(jp49 + "_"), kControlCodeMaxAllowedLen);
    EXPECT_TRUE(tr::is_control_code(jp49 + "_"));
    const std::string jp50 = jp49 + "あ";
    EXPECT_EQ(char_len(jp50 + "_"), kControlCodeRejectedLen);
    EXPECT_FALSE(tr::is_control_code(jp50 + "_"));
}

TEST(Translate, needs_translation_order_and_ideographic_space) {
    // The strip removes U+3000 as well as ASCII whitespace, so an
    // all-ideographic-space line is rejected rather than becoming a junk key.
    EXPECT_FALSE(tr::needs_translation("　　"));
    EXPECT_FALSE(tr::needs_translation(""));
    EXPECT_TRUE(tr::needs_translation("こんにちは"));
    EXPECT_FALSE(tr::needs_translation("Hello"));
    EXPECT_FALSE(tr::needs_translation("……"));
    EXPECT_FALSE(tr::needs_translation("あ@"));  // control-code gate runs first
}

// PATTERN A.  Copying the BLACKCyc extract_speaker (which parses the text)
// would return NARRATION for 100% of lines -- the exact regression the gate
// exists to catch.
TEST(Translate, extract_speaker_reads_the_field_not_the_text) {
    EXPECT_EQ(tr::extract_speaker(rec("dialogue", "マイケル")), "Michael");
    EXPECT_EQ(tr::extract_speaker(rec("narration", "マイケル")), "NARRATION");
    EXPECT_EQ(tr::extract_speaker(rec("dialogue", "")), "NARRATION");
    EXPECT_EQ(tr::extract_speaker(rec("dialogue", "知らない人")), "知らない人");
}

TEST(Translate, parse_numbered_response_strips_a_leading_speaker_tag) {
    // The '&' in the character class is shingakkou-specific; without it the
    // tag stays glued to the front of the translation.
    auto t = tr::parse_numbered_response("[Michael & Gabby] 3. text");
    ASSERT_EQ(t.count(kTagSampleNum3), kSingleEntryCount);
    EXPECT_EQ(t[kTagSampleNum3], "text");

    EXPECT_EQ(tr::parse_numbered_response("[NARRATION] 5. hi")[kTagSampleNum5], "hi");
    EXPECT_EQ(tr::parse_numbered_response("12) foo")[kTagSampleNum12], "foo");

    // The substitution is ^-anchored and runs BEFORE the number match, so a
    // tag AFTER the number is deliberately kept -- there it is part of the
    // translated line, not a wrapper around it.
    EXPECT_EQ(
        tr::parse_numbered_response("3. [NARRATION] text")[kTagSampleNum3],
        "[NARRATION] text");
}

// The BLACKCyc pipelines un-escape \r\n here; shingakkou must NOT, because a
// real CR/LF in the value would break the TSV row it ends up in.
TEST(Translate, parse_numbered_response_does_not_unescape_newlines) {
    const auto t = tr::parse_numbered_response("1. a\\r\\nb");
    ASSERT_EQ(t.count(kFirstLineNumber), kSingleEntryCount);
    // Six characters: a, backslash, r, backslash, n, b -- no CR and no LF.
    EXPECT_EQ(t.at(kFirstLineNumber), "a\\r\\nb");
    EXPECT_EQ(t.at(kFirstLineNumber).size(), kEscapedCrLfLiteralLen);
    EXPECT_EQ(t.at(kFirstLineNumber).find('\r'), std::string::npos);
    EXPECT_EQ(t.at(kFirstLineNumber).find('\n'), std::string::npos);
}

TEST(Translate, sanitize_ascii_eight_replacements) {
    EXPECT_EQ(tr::sanitize_ascii("a—b"), "a--b");   // em dash
    EXPECT_EQ(tr::sanitize_ascii("a–b"), "a--b");   // en dash
    EXPECT_EQ(tr::sanitize_ascii("a…b"), "a...b");  // ellipsis
    EXPECT_EQ(tr::sanitize_ascii("‘x’"), "'x'");
    EXPECT_EQ(tr::sanitize_ascii("“x”"), "\"x\"");
    EXPECT_EQ(tr::sanitize_ascii("a　b"), "a b");   // U+3000
}

// Re-attaching a name plate here (as the BLACKCyc ports do) would double it:
// the TSV builder already prepends one, giving "Michael\nMichael\nHello" at runtime.
TEST(Translate, postprocess_does_not_reattach_a_speaker) {
    EXPECT_EQ(tr::postprocess("マイケル\\n「x」", "Hello"), "Hello");
    EXPECT_EQ(tr::postprocess("あ", "あ"), "あ");   // identical -> untouched
    EXPECT_EQ(tr::postprocess("あ", ""), "");
}

// This is what lets the 75 glossary names be re-seeded at the top of every run
// without hoisting them to the front of the 6.6 MB cache file.
TEST(Translate, cache_preserves_insertion_order_across_reseed) {
    tr::Cache c;
    c.set("A", "1");
    c.set("B", "2");
    c.set("C", "3");
    c.set("A", "9");
    ASSERT_EQ(c.size(), kReseedEntryCount);
    EXPECT_EQ(c.items()[kIdx0].first, "A");
    EXPECT_EQ(c.items()[kIdx0].second, "9");
    EXPECT_EQ(c.items()[kIdx1].first, "B");
    EXPECT_EQ(c.items()[kIdx2].first, "C");
}

TEST(Translate, glossary_has_the_expected_shape) {
    EXPECT_EQ(tr::name_translations_ordered().size(), kExpectedGlossaryNameCount);
    EXPECT_EQ(tr::name_translations().size(), kExpectedGlossaryNameCount);
    EXPECT_EQ(tr::story_order().size(), kExpectedStoryScriptCount);
    EXPECT_EQ(tr::name_translations().at("マイケル・ガビィ"), "Michael & Gabby");
}

// The guard refuses flags that would throw away a paid cache until
// --discard-cache confirms the intent.
TEST(Translate, a_flag_that_deletes_a_paid_cache_is_refused_until_it_is_meant) {
    // An empty or nearly empty cache is what a from-scratch run was written for.
    EXPECT_FALSE(tr::refuse_cache_discard(kEmptyCacheCount, "--test", false).has_value());
    EXPECT_FALSE(tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD, "--test",
                                          false).has_value());

    // One entry past the threshold, every flag is refused.
    for (const char* flag : {"--test", "--retranslate", "--clean"}) {
        auto why = tr::refuse_cache_discard(
            tr::CACHE_DISCARD_THRESHOLD + kOnePastThresholdOffset, flag, false);
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

    EXPECT_EQ(tr::purge_failed_entries(cache), kExpectedPurgedEchoCount);
    EXPECT_EQ(cache.size(), kExpectedSurvivingCacheCount);
    EXPECT_FALSE(cache.contains("こんにちは"));
    EXPECT_FALSE(cache.contains("Eスbg152n"));
    ASSERT_TRUE(cache.get("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    EXPECT_TRUE(cache.contains("OK"));
    EXPECT_TRUE(cache.contains("bg152n"));

    const auto& items = cache.items();
    ASSERT_EQ(items.size(), kExpectedSurvivingCacheCount);
    EXPECT_EQ(items[kIdx0].first, "朝が来た。");
    EXPECT_EQ(items[kIdx1].first, "OK");
    EXPECT_EQ(items[kIdx2].first, "bg152n");

    // Nothing left to remove on a second pass.
    EXPECT_EQ(tr::purge_failed_entries(cache), kEmptyCacheCount);
    EXPECT_EQ(cache.size(), kExpectedSurvivingCacheCount);
}
