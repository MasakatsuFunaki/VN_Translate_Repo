// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for cache_core and glossary: prompt assembly, response parsing,
// and the speaker state-machine invariants the gate depends on.
#include "cache/cache_core.h"

#include "cache/glossary.h"
#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bj = boost::json;
namespace tr = ama::translate;
using namespace ama;

namespace {

constexpr int kLineZero = 0;
constexpr int kLineOne = 1;
constexpr int kLineTwo = 2;
constexpr int kLineThree = 3;

constexpr int kFirstNumberedLine = 1;
constexpr int kSecondNumberedLine = 2;
constexpr int kThirdNumberedLine = 3;

constexpr std::size_t kExpectedTwoItems = 2;
constexpr std::size_t kExpectedThreeItems = 3;

constexpr int kExtraContextLines = 25;
constexpr std::size_t kPaidCacheLineCount = 56103;
constexpr int kUnansweredLineIdx = 7;
constexpr int kAnsweredLineIdx = 8;

bj::object mk_line(int idx, const char* type, const char* content) {
    bj::object o;
    o["idx"] = idx;
    o["type"] = type;
    o["content"] = content;
    return o;
}

bj::object mk_script(bj::array lines) {
    bj::object s;
    s["lines"] = std::move(lines);
    return s;
}

}  // namespace

TEST(Translate, needs_translation_detects_kana_and_kanji) {
    EXPECT_TRUE(tr::needs_translation("あ"));
    EXPECT_TRUE(tr::needs_translation("日"));
    EXPECT_FALSE(tr::needs_translation("plain ascii"));
    EXPECT_FALSE(tr::needs_translation(""));
    // `$var` placeholders are engine variables, never translatable text.
    EXPECT_FALSE(tr::needs_translation("$str20"));
    EXPECT_FALSE(tr::needs_translation("$str20あ"));
}

TEST(Translate, speaker_tag_resolution) {
    EXPECT_EQ(tr::get_speaker_tag("ちとせ"), "Chitose");
    EXPECT_EQ(tr::get_speaker_tag("玲"), "Rei");
    EXPECT_EQ(tr::get_speaker_tag("$str20"), "NARRATION");
    EXPECT_EQ(tr::get_speaker_tag(""), "NARRATION");
    // Unknown JP names pass through verbatim rather than becoming NARRATION.
    EXPECT_EQ(tr::get_speaker_tag("未知"), "未知");
}

// The state machine is the whole speaker pipeline for this game.
TEST(Translate, message_runs_attribute_bracketed_dialogue_only) {
    const bj::object script = mk_script(bj::array{
        mk_line(kLineZero, "NAME", "ちとせ"),
        // Unbracketed line right after a NAME is narration, not her speech.
        mk_line(kLineOne, "MESSAGE", "ああ"),
        // Bracketed line IS her speech.
        mk_line(kLineTwo, "MESSAGE", "「あ」"),
    });
    const auto runs = tr::extract_message_runs(script);
    ASSERT_EQ(runs.size(), kExpectedTwoItems);
    EXPECT_EQ(runs[0].line_idx, kLineOne);
    EXPECT_EQ(runs[0].speaker, "NARRATION");
    EXPECT_EQ(runs[1].line_idx, kLineTwo);
    EXPECT_EQ(runs[1].speaker, "Chitose");
}

TEST(Translate, message_runs_fw0_resets_to_narration) {
    const bj::object script = mk_script(bj::array{
        mk_line(kLineZero, "NAME", "玲"),
        mk_line(kLineOne, "MESSAGE", "「あ」"),
        mk_line(kLineTwo, "COMMAND", "fw 0"),
        mk_line(kLineThree, "MESSAGE", "「い」"),
    });
    const auto runs = tr::extract_message_runs(script);
    ASSERT_EQ(runs.size(), kExpectedTwoItems);
    EXPECT_EQ(runs[0].speaker, "Rei");
    EXPECT_EQ(runs[1].speaker, "NARRATION") << "fw 0 must hide the face window";
}

// Dialogue spanning a \@ page break: the open bracket keeps the speaker
// attached across the following unbracketed continuation line.
TEST(Translate, message_runs_dialogue_spans_page_break) {
    const bj::object script = mk_script(bj::array{
        mk_line(kLineZero, "NAME", "玲"),
        mk_line(kLineOne, "MESSAGE", "「あ\\@"),
        mk_line(kLineTwo, "MESSAGE", "い」"),
        mk_line(kLineThree, "MESSAGE", "う"),
    });
    const auto runs = tr::extract_message_runs(script);
    ASSERT_EQ(runs.size(), kExpectedThreeItems);
    EXPECT_EQ(runs[0].speaker, "Rei");
    EXPECT_EQ(runs[1].speaker, "Rei") << "continuation stays with the speaker";
    EXPECT_EQ(runs[2].speaker, "NARRATION") << "closing bracket ended the dialogue";
}

// Leading \n is a soft wrap, not a new utterance -- it must not hide 「.
TEST(Translate, message_runs_see_bracket_behind_soft_wrap) {
    const bj::object script = mk_script(bj::array{
        mk_line(kLineZero, "NAME", "玲"),
        mk_line(kLineOne, "MESSAGE", "\\n\\n「あ」"),
    });
    const auto runs = tr::extract_message_runs(script);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].speaker, "Rei");
}

TEST(Translate, fix_split_dialogue_dedupes_boundary_quotes) {
    // Open-only line: drop the trailing quote the model added, then rstrip.
    EXPECT_EQ(tr::fix_split_dialogue("「あ", "\"Hello \""), "\"Hello");
    // Only a REAL trailing quote triggers it -- anything else passes through.
    EXPECT_EQ(tr::fix_split_dialogue("「あ", "\"Hello "), "\"Hello ");
    // Close-only line: drop the leading quote, then lstrip.
    EXPECT_EQ(tr::fix_split_dialogue("あ」", "\" world\""), "world\"");
    // Balanced line: untouched.
    EXPECT_EQ(tr::fix_split_dialogue("「あ」", "\"Hi\""), "\"Hi\"");
    // No brackets at all: untouched.
    EXPECT_EQ(tr::fix_split_dialogue("あ", "\"Hi\""), "\"Hi\"");
}

TEST(Translate, strip_speaker_tags_removes_echoed_tags) {
    EXPECT_EQ(tr::strip_speaker_tags("[NARRATION] Snow fell."), "Snow fell.");
    EXPECT_EQ(tr::strip_speaker_tags("[Chitose] Good morning."), "Good morning.");
    EXPECT_EQ(tr::strip_speaker_tags("  Good morning.  "), "Good morning.");
}

TEST(Translate, strip_covers_unicode_whitespace) {
    // U+3000 IDEOGRAPHIC SPACE pads name-window strings and must come off.
    EXPECT_EQ(tr::strip("　Rei　"), "Rei");
    EXPECT_EQ(tr::strip("  spaced \t\n"), "spaced");
    // U+200B ZERO WIDTH SPACE is not whitespace -- must survive.
    EXPECT_EQ(tr::strip("\u200B"), "\u200B");
}

TEST(Translate, user_prompt_shape) {
    const std::string p =
        tr::build_user_prompt({{"Chitose", "あ"}, {"NARRATION", "い"}},
                              {{"Rei", "Previous line."}});
    EXPECT_NE(p.find("<previous_context>\n[Rei] Previous line.\n</previous_context>"),
              std::string::npos);
    EXPECT_NE(p.find("1. [Chitose] あ\n2. [NARRATION] い\n"),
              std::string::npos);
    EXPECT_NE(p.find("Output EXACTLY 2 numbered translations"), std::string::npos);
}

TEST(Translate, user_prompt_omits_empty_context_block) {
    const std::string p = tr::build_user_prompt({{"NARRATION", "あ"}}, {});
    EXPECT_EQ(p.find("</previous_context>"), std::string::npos);
    EXPECT_EQ(p.rfind("<lines_to_translate>", 0), 0u);
}

TEST(Translate, context_window_is_capped) {
    const int total_context_lines = tr::CONTEXT_WINDOW + kExtraContextLines;
    const int newest_line_index = total_context_lines - 1;
    std::vector<std::pair<std::string, std::string>> ctx;
    for (int i = 0; i < total_context_lines; ++i) {
        ctx.emplace_back("S", "line" + std::to_string(i));
    }
    const std::string p = tr::build_user_prompt({{"S", "x"}}, ctx);
    EXPECT_EQ(p.find("[S] line0\n"), std::string::npos) << "oldest line leaked in";
    EXPECT_NE(p.find("[S] line" + std::to_string(kExtraContextLines) + "\n"),
              std::string::npos)
        << "window start missing";
    EXPECT_NE(p.find("[S] line" + std::to_string(newest_line_index) + "\n"),
              std::string::npos)
        << "newest line missing";
}

TEST(Translate, parses_numbered_response) {
    auto t = tr::parse_numbered_response("1. First.\n2) Second.\n3: Third.\n");
    ASSERT_EQ(t.size(), kExpectedThreeItems);
    EXPECT_EQ(t[kFirstNumberedLine], "First.");
    EXPECT_EQ(t[kSecondNumberedLine], "Second.");
    EXPECT_EQ(t[kThirdNumberedLine], "Third.");
}

TEST(Translate, parses_multiline_continuation) {
    auto t = tr::parse_numbered_response("1. First part\ncontinued here\n2. Second\n");
    ASSERT_EQ(t.size(), kExpectedTwoItems);
    EXPECT_EQ(t[kFirstNumberedLine], "First part continued here");
}

TEST(Translate, cache_preserves_insertion_order) {
    tr::Cache c;
    c.set("b", "B");
    c.set("a", "A");
    c.set("b", "B2");
    ASSERT_EQ(c.size(), kExpectedTwoItems);
    EXPECT_EQ(c.items()[0].first, "b");
    EXPECT_EQ(c.items()[0].second, "B2");
    EXPECT_EQ(c.items()[1].first, "a");
}

TEST(Translate, glossary_entries_are_well_formed) {
    const auto& ordered = tr::name_translations_ordered();
    EXPECT_FALSE(ordered.empty());
    for (const auto& [jp, en] : ordered) {
        EXPECT_FALSE(jp.empty());
        EXPECT_FALSE(tr::strip(en).empty()) << "empty EN for " << jp;
    }
    EXPECT_EQ(ordered.size(), tr::name_translations().size())
        << "duplicate JP key in the glossary";
}

// The guard refuses flags that would throw away a paid cache until
// --discard-cache confirms the intent.
TEST(Translate, a_flag_that_deletes_a_paid_cache_is_refused_until_it_is_meant) {
    EXPECT_FALSE(tr::refuse_cache_discard(0, "--test", false).has_value());
    EXPECT_FALSE(tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD, "--test",
                                          false).has_value());

    for (const char* flag : {"--test", "--retranslate", "--clean"}) {
        const auto why = tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD + 1, flag,
                                                  false);
        ASSERT_TRUE(why.has_value()) << flag;
        EXPECT_NE(why->find(flag), std::string::npos) << *why;
        EXPECT_NE(why->find("--discard-cache"), std::string::npos) << *why;
    }

    const auto why = tr::refuse_cache_discard(kPaidCacheLineCount, "--test", false);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find(std::to_string(kPaidCacheLineCount)), std::string::npos) << *why;

    EXPECT_FALSE(
        tr::refuse_cache_discard(kPaidCacheLineCount, "--test", true).has_value());
}

// An echo -- the model handing back the line it was given -- is a failure only
// when the line is Japanese.  An English string that survives translation
// unchanged is a correct answer, so pure ASCII is never a failure.
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

// The purge takes out exactly the echoes and leaves everything else where it
// was, key order included -- that order is the cache file's key order.
TEST(Translate, purge_removes_failed_entries_and_keeps_the_rest) {
    tr::Cache cache;
    cache.set("朝が来た。", "Morning came.");
    cache.set("こんにちは", "こんにちは");
    cache.set("OK", "OK");
    cache.set("Eスbg152n", "Eスbg152n");
    cache.set("bg152n", "bg152n");

    EXPECT_EQ(tr::purge_failed_entries(cache), kExpectedTwoItems);
    EXPECT_EQ(cache.size(), kExpectedThreeItems);
    EXPECT_FALSE(cache.contains("こんにちは"));
    EXPECT_FALSE(cache.contains("Eスbg152n"));
    ASSERT_TRUE(cache.get("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    EXPECT_TRUE(cache.contains("OK"));
    EXPECT_TRUE(cache.contains("bg152n"));

    const auto& items = cache.items();
    ASSERT_EQ(items.size(), kExpectedThreeItems);
    EXPECT_EQ(items[0].first, "朝が来た。");
    EXPECT_EQ(items[1].first, "OK");
    EXPECT_EQ(items[2].first, "bg152n");

    EXPECT_EQ(tr::purge_failed_entries(cache), 0u);
    EXPECT_EQ(cache.size(), kExpectedThreeItems);
}

// A batch that failed leaves its lines out of the cache AND out of the results
// map the run log is written from, so the next run asks for exactly those lines
// again and no log shows the Japanese as its own English.
TEST(Translate, an_answer_that_never_arrived_is_neither_cached_nor_logged) {
    tr::Cache cache;
    std::map<int, std::string> results;
    std::vector<std::pair<std::string, std::string>> context;

    tr::apply_line_result(cache, results, context, kUnansweredLineIdx,
                          "[NARRATION]", "「おはよう」", "", "");
    EXPECT_FALSE(cache.contains("「おはよう」"));
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(results.count(kUnansweredLineIdx), 0u);
    ASSERT_EQ(context.size(), 1u);
    EXPECT_EQ(context.front().second, "「おはよう」");

    tr::apply_line_result(cache, results, context, kAnsweredLineIdx,
                          "[NARRATION]", "朝が来た。", "\\@", "Morning came.");
    ASSERT_TRUE(cache.contains("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.\\@");
    ASSERT_EQ(results.count(kAnsweredLineIdx), 1u);
    EXPECT_EQ(results[kAnsweredLineIdx], "Morning came.\\@");
    EXPECT_EQ(context.back().second, "Morning came.");
}
