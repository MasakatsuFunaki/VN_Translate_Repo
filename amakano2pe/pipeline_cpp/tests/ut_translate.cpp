// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-2 tests: prompt assembly, response parsing, and the speaker
// state-machine invariants the gate depends on.
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "common/util.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

namespace bj = boost::json;
namespace tr = ama::translate;
using namespace ama;

namespace {

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

// 「 and 」 in UTF-8.
constexpr const char* OPEN = "\xE3\x80\x8C";
constexpr const char* CLOSE = "\xE3\x80\x8D";

}  // namespace

TEST(Translate, needs_translation_detects_kana_and_kanji) {
    EXPECT_TRUE(tr::needs_translation("\xE3\x81\x82"));  // あ
    EXPECT_TRUE(tr::needs_translation("\xE6\x97\xA5"));  // 日
    EXPECT_FALSE(tr::needs_translation("plain ascii"));
    EXPECT_FALSE(tr::needs_translation(""));
    // `$var` placeholders are engine variables, never translatable text.
    EXPECT_FALSE(tr::needs_translation("$str20"));
    EXPECT_FALSE(tr::needs_translation("$str20\xE3\x81\x82"));
}

TEST(Translate, speaker_tag_resolution) {
    EXPECT_EQ(tr::get_speaker_tag("\xE3\x81\xA1\xE3\x81\xA8\xE3\x81\x9B"), "Chitose");
    EXPECT_EQ(tr::get_speaker_tag("\xE7\x8E\xB2"), "Rei");
    EXPECT_EQ(tr::get_speaker_tag("$str20"), "NARRATION");
    EXPECT_EQ(tr::get_speaker_tag(""), "NARRATION");
    // Unknown JP names pass through verbatim rather than becoming NARRATION.
    EXPECT_EQ(tr::get_speaker_tag("\xE6\x9C\xAA\xE7\x9F\xA5"), "\xE6\x9C\xAA\xE7\x9F\xA5");
}

// The state machine is the whole speaker pipeline for this game.
TEST(Translate, message_runs_attribute_bracketed_dialogue_only) {
    bj::object script = mk_script(bj::array{
        mk_line(0, "NAME", "\xE3\x81\xA1\xE3\x81\xA8\xE3\x81\x9B"),  // ちとせ
        // Unbracketed line right after a NAME is narration, not her speech.
        mk_line(1, "MESSAGE", "\xE3\x81\x82\xE3\x81\x82"),
        // Bracketed line IS her speech.
        mk_line(2, "MESSAGE", (std::string(OPEN) + "\xE3\x81\x82" + CLOSE).c_str()),
    });
    auto runs = tr::extract_message_runs(script);
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].line_idx, 1);
    EXPECT_EQ(runs[0].speaker, "NARRATION");
    EXPECT_EQ(runs[1].line_idx, 2);
    EXPECT_EQ(runs[1].speaker, "Chitose");
}

TEST(Translate, message_runs_fw0_resets_to_narration) {
    bj::object script = mk_script(bj::array{
        mk_line(0, "NAME", "\xE7\x8E\xB2"),  // 玲
        mk_line(1, "MESSAGE", (std::string(OPEN) + "\xE3\x81\x82" + CLOSE).c_str()),
        mk_line(2, "COMMAND", "fw 0"),
        mk_line(3, "MESSAGE", (std::string(OPEN) + "\xE3\x81\x84" + CLOSE).c_str()),
    });
    auto runs = tr::extract_message_runs(script);
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].speaker, "Rei");
    EXPECT_EQ(runs[1].speaker, "NARRATION") << "fw 0 must hide the face window";
}

// Dialogue spanning a \@ page break: the open bracket keeps the speaker
// attached across the following unbracketed continuation line.
TEST(Translate, message_runs_dialogue_spans_page_break) {
    bj::object script = mk_script(bj::array{
        mk_line(0, "NAME", "\xE7\x8E\xB2"),
        mk_line(1, "MESSAGE", (std::string(OPEN) + "\xE3\x81\x82\\@").c_str()),
        mk_line(2, "MESSAGE", (std::string("\xE3\x81\x84") + CLOSE).c_str()),
        mk_line(3, "MESSAGE", "\xE3\x81\x86"),
    });
    auto runs = tr::extract_message_runs(script);
    ASSERT_EQ(runs.size(), 3u);
    EXPECT_EQ(runs[0].speaker, "Rei");
    EXPECT_EQ(runs[1].speaker, "Rei") << "continuation stays with the speaker";
    EXPECT_EQ(runs[2].speaker, "NARRATION") << "closing bracket ended the dialogue";
}

// Leading \n is a soft wrap, not a new utterance -- it must not hide 「.
TEST(Translate, message_runs_see_bracket_behind_soft_wrap) {
    bj::object script = mk_script(bj::array{
        mk_line(0, "NAME", "\xE7\x8E\xB2"),
        mk_line(1, "MESSAGE", (std::string("\\n\\n") + OPEN + "\xE3\x81\x82" + CLOSE).c_str()),
    });
    auto runs = tr::extract_message_runs(script);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].speaker, "Rei");
}

TEST(Translate, fix_split_dialogue_dedupes_boundary_quotes) {
    // Open-only line: drop the trailing quote Claude added, then rstrip.
    EXPECT_EQ(tr::fix_split_dialogue(std::string(OPEN) + "\xE3\x81\x82", "\"Hello \""),
              "\"Hello");
    // Only a REAL trailing quote triggers it -- anything else passes through.
    EXPECT_EQ(tr::fix_split_dialogue(std::string(OPEN) + "\xE3\x81\x82", "\"Hello "),
              "\"Hello ");
    // Close-only line: drop the leading quote, then lstrip.
    EXPECT_EQ(tr::fix_split_dialogue(std::string("\xE3\x81\x82") + CLOSE, "\" world\""),
              "world\"");
    // Balanced line: untouched.
    EXPECT_EQ(tr::fix_split_dialogue(std::string(OPEN) + "\xE3\x81\x82" + CLOSE, "\"Hi\""),
              "\"Hi\"");
    // No brackets at all: untouched.
    EXPECT_EQ(tr::fix_split_dialogue("\xE3\x81\x82", "\"Hi\""), "\"Hi\"");
}

TEST(Translate, strip_speaker_tags_removes_echoed_tags) {
    EXPECT_EQ(tr::strip_speaker_tags("[NARRATION] Snow fell."), "Snow fell.");
    EXPECT_EQ(tr::strip_speaker_tags("[Chitose] Good morning."), "Good morning.");
    EXPECT_EQ(tr::strip_speaker_tags("  Good morning.  "), "Good morning.");
}

TEST(Translate, strip_covers_unicode_whitespace) {
    // U+3000 IDEOGRAPHIC SPACE pads name-window strings and must come off.
    EXPECT_EQ(tr::strip("\xE3\x80\x80Rei\xE3\x80\x80"), "Rei");
    EXPECT_EQ(tr::strip("  spaced \t\n"), "spaced");
    // U+200B ZERO WIDTH SPACE is not whitespace -- must survive.
    EXPECT_EQ(tr::strip("\xE2\x80\x8B"), "\xE2\x80\x8B");
}

TEST(Translate, user_prompt_shape) {
    std::string p = tr::build_user_prompt({{"Chitose", "\xE3\x81\x82"}, {"NARRATION", "\xE3\x81\x84"}},
                                          {{"Rei", "Previous line."}});
    EXPECT_NE(p.find("<previous_context>\n[Rei] Previous line.\n</previous_context>"),
              std::string::npos);
    EXPECT_NE(p.find("1. [Chitose] \xE3\x81\x82\n2. [NARRATION] \xE3\x81\x84\n"),
              std::string::npos);
    EXPECT_NE(p.find("Output EXACTLY 2 numbered translations"), std::string::npos);
}

TEST(Translate, user_prompt_omits_empty_context_block) {
    std::string p = tr::build_user_prompt({{"NARRATION", "\xE3\x81\x82"}}, {});
    EXPECT_EQ(p.find("</previous_context>"), std::string::npos);
    EXPECT_EQ(p.rfind("<lines_to_translate>", 0), 0u);
}

TEST(Translate, context_window_is_capped) {
    std::vector<std::pair<std::string, std::string>> ctx;
    for (int i = 0; i < tr::CONTEXT_WINDOW + 25; ++i)
        ctx.emplace_back("S", "line" + std::to_string(i));
    std::string p = tr::build_user_prompt({{"S", "x"}}, ctx);
    EXPECT_EQ(p.find("[S] line0\n"), std::string::npos) << "oldest line leaked in";
    EXPECT_NE(p.find("[S] line25\n"), std::string::npos) << "window start missing";
    EXPECT_NE(p.find("[S] line74\n"), std::string::npos) << "newest line missing";
}

TEST(Translate, parses_numbered_response) {
    auto t = tr::parse_numbered_response("1. First.\n2) Second.\n3: Third.\n");
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[1], "First.");
    EXPECT_EQ(t[2], "Second.");
    EXPECT_EQ(t[3], "Third.");
}

TEST(Translate, parses_multiline_continuation) {
    auto t = tr::parse_numbered_response("1. First part\ncontinued here\n2. Second\n");
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[1], "First part continued here");
}

TEST(Translate, cache_preserves_insertion_order) {
    tr::Cache c;
    c.set("b", "B");
    c.set("a", "A");
    c.set("b", "B2");
    ASSERT_EQ(c.size(), 2u);
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
    auto why = tr::refuse_cache_discard(56103, "--test", false);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("56103"), std::string::npos) << *why;

    // --discard-cache is the caller saying they meant it.
    EXPECT_FALSE(tr::refuse_cache_discard(56103, "--test", true).has_value());
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

    // Nothing left to remove on a second pass.
    EXPECT_EQ(tr::purge_failed_entries(cache), 0u);
    EXPECT_EQ(cache.size(), 3u);
}

// A batch that failed leaves its lines out of the cache AND out of the results
// map the run log is written from, so the next run asks for exactly those lines
// again and no log shows the Japanese as its own English.
TEST(Translate, an_answer_that_never_arrived_is_neither_cached_nor_logged) {
    tr::Cache cache;
    std::map<int, std::string> results;
    std::vector<std::pair<std::string, std::string>> context;

    tr::apply_line_result(cache, results, context, 7, "[NARRATION]", "「おはよう」",
                          "", "");
    EXPECT_FALSE(cache.contains("「おはよう」"));
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(results.count(7), 0u);
    // The context window still reads in sequence.
    ASSERT_EQ(context.size(), 1u);
    EXPECT_EQ(context.front().second, "「おはよう」");

    tr::apply_line_result(cache, results, context, 8, "[NARRATION]", "朝が来た。",
                          "\\@", "Morning came.");
    ASSERT_TRUE(cache.contains("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.\\@");
    ASSERT_EQ(results.count(8), 1u);
    EXPECT_EQ(results[8], "Morning came.\\@");
    EXPECT_EQ(context.back().second, "Morning came.");
}
