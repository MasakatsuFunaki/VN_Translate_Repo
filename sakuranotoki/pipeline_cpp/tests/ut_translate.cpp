// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-2 tests: the speaker state machine (the whole speaker pipeline for
// this game), prompt shape, response parsing, the guards that decide whether
// an answer can be applied at all, and the option validation that stands in
// front of the spend.
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "apps/pipeline_steps.h"
#include "common/coverage.h"
#include "common/util.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"
#include "translate/translate_core.h"

namespace bj = boost::json;
namespace fs = std::filesystem;
namespace tr = snt::translate;
using namespace snt;

namespace {

bj::object mk(const char* type, const char* text) {
    bj::object o;
    o["type"] = type;
    o["text"] = text;
    return o;
}

// 直哉 / 心鈴 in UTF-8.
constexpr const char* NAOYA = "\xE7\x9B\xB4\xE5\x93\x89";
constexpr const char* MISUZU = "\xE5\xBF\x83\xE9\x88\xB4";

// Written as bytes, so an escape cannot be read as part of what follows it.
constexpr const char* U3000 = "\xE3\x80\x80";           // IDEOGRAPHIC SPACE
constexpr const char* FULLWIDTH_ONE = "\xEF\xBC\x91";   // U+FF11
constexpr const char* FULLWIDTH_STOP = "\xEF\xBC\x8E";  // U+FF0E

// One batch of `n` lines, all the same, as translate_batch takes them.
std::vector<std::pair<std::string, std::string>> batch_of(std::size_t n) {
    std::vector<std::pair<std::string, std::string>> out;
    for (std::size_t i = 0; i < n; ++i)
        out.emplace_back("Naoya", "\xE6\x9C\x9D\xE3\x81\x8C" + std::to_string(i));
    return out;
}

// The response body the API returns for one numbered answer.  With adaptive
// thinking the thinking block comes first and carries no text, so it is here
// too: the reader has to walk past it.
bj::value api_response(const std::string& answer, const char* stop_reason) {
    bj::object thinking;
    thinking["type"] = "thinking";
    thinking["thinking"] = "";
    bj::object text;
    text["type"] = "text";
    text["text"] = answer;
    bj::object root;
    root["content"] = bj::array{std::move(thinking), std::move(text)};
    root["stop_reason"] = stop_reason;
    return bj::value(std::move(root));
}

// 「あ」, the one spoken text every synthetic document below uses.
constexpr const char* SPOKEN = "\xE3\x80\x8C\xE3\x81\x82\xE3\x80\x8D";

// A document shaped like the extraction: one script, `n` dialogue entries,
// each attributed to `speaker` unless it is empty.
bj::object dialogue_document(std::size_t n, const std::string& speaker) {
    bj::array strings;
    for (std::size_t i = 0; i < n; ++i) {
        bj::object e = mk("dialogue", SPOKEN);
        if (!speaker.empty()) e["speaker"] = speaker;
        strings.push_back(std::move(e));
    }
    bj::object script;
    script["strings"] = std::move(strings);
    bj::object doc;
    doc["script\\a.ast"] = std::move(script);
    return doc;
}

// A document the whole gate accepts: every label of the name table appears as a
// plate and speaks a line, which is the bidirectional coverage check 5 asks
// for, and the largest speaker carries enough lines for check 6 to have a
// dossier row to test.  Every text in it is either a name-table key or SPOKEN,
// so a run over it needs nothing the cache cannot answer.
bj::object gate_passing_document() {
    bj::array strings;
    auto speak = [&](const std::string& label) {
        bj::object e = mk("dialogue", SPOKEN);
        e["speaker"] = label;
        strings.push_back(std::move(e));
    };
    for (const auto& [jp, en] : tr::name_translations_ordered()) {
        strings.push_back(mk("name", jp.c_str()));
        speak(jp);
    }
    // Enough lines to put one label over the dossier threshold, so check 6
    // measures a real row instead of finding nothing to measure.
    for (std::size_t i = 0; i < snt::translate::DOSSIER_LINE_THRESHOLD; ++i)
        speak(NAOYA);

    bj::object script;
    script["strings"] = std::move(strings);
    bj::object doc;
    doc["script\\a.ast"] = std::move(script);
    return doc;
}

}  // namespace

TEST(Translate, needs_translation_detects_kana_and_kanji) {
    EXPECT_TRUE(tr::needs_translation("\xE3\x81\x82"));
    EXPECT_TRUE(tr::needs_translation(NAOYA));
    EXPECT_FALSE(tr::needs_translation("plain ascii"));
    EXPECT_FALSE(tr::needs_translation(""));
}

// A dialogue entry inherits the most recent name entry's romanization.
TEST(Translate, state_machine_attributes_dialogue_to_preceding_name) {
    bj::array entries{mk("name", NAOYA), mk("dialogue", "\xE3\x80\x8C\xE3\x81\x82\xE3\x80\x8D"),
                      mk("narrative", "\xE3\x81\x84"),
                      mk("dialogue", "\xE3\x80\x8C\xE3\x81\x86\xE3\x80\x8D")};
    auto rows = tr::iter_entries_with_speakers(entries);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0].first, "NAME");
    EXPECT_EQ(rows[1].first, "Naoya");
    EXPECT_EQ(rows[2].first, "NARRATION") << "narrative is never attributed";
    EXPECT_EQ(rows[3].first, "Naoya") << "speaker persists until the next name";
}

// A chapter title resets the speaker: the next dialogue must not inherit the
// name from the previous chapter.
TEST(Translate, title_resets_the_speaker) {
    bj::array entries{mk("name", NAOYA),
                      mk("dialogue", "\xE3\x80\x8C\xE3\x81\x82\xE3\x80\x8D"),
                      mk("title", "\xE7\xAC\xAC\xE4\xB8\x80\xE7\xAB\xA0"),
                      mk("dialogue", "\xE3\x80\x8C\xE3\x81\x84\xE3\x80\x8D")};
    auto rows = tr::iter_entries_with_speakers(entries);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[1].first, "Naoya");
    EXPECT_EQ(rows[2].first, "TITLE");
    EXPECT_EQ(rows[3].first, "NARRATION");
}

TEST(Translate, dialogue_before_any_name_is_narration) {
    bj::array entries{mk("dialogue", "\xE3\x80\x8C\xE3\x81\x82\xE3\x80\x8D")};
    auto rows = tr::iter_entries_with_speakers(entries);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].first, "NARRATION");
}

// Unknown JP names pass through verbatim rather than collapsing to NARRATION.
TEST(Translate, unknown_name_passes_through) {
    bj::array entries{mk("name", "\xE6\x9C\xAA\xE7\x9F\xA5"),
                      mk("dialogue", "\xE3\x80\x8C\xE3\x81\x82\xE3\x80\x8D")};
    auto rows = tr::iter_entries_with_speakers(entries);
    EXPECT_EQ(rows[1].first, "\xE6\x9C\xAA\xE7\x9F\xA5");
}

TEST(Translate, glossary_names_resolve) {
    bj::array entries{mk("name", MISUZU), mk("dialogue", "x")};
    EXPECT_EQ(tr::iter_entries_with_speakers(entries)[1].first, "Misuzu");
}

// The extractor saw the script; the state machine only sees the entries it
// produced.  Where the two answer differently the extractor's field wins.
TEST(Translate, the_speaker_field_wins_over_the_state_machine) {
    bj::object fragment = mk("narrative", "\xE5\x86\x86\xE6\x9C\xA8");  // 円木
    fragment["speaker"] = MISUZU;
    bj::array entries{mk("name", NAOYA), std::move(fragment)};

    tr::SpeakerSource source;
    auto rows = tr::iter_entries_with_speakers(entries, &source);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[1].first, "Misuzu");
    EXPECT_EQ(source.from_field, 1u);
    EXPECT_EQ(source.disagreements, 1u);
}

// A field that is present and null says the line is narration.  Falling back
// to the state machine here would file it under whoever last had a plate.
TEST(Translate, a_null_speaker_field_is_narration_not_a_fallback) {
    bj::object line = mk("dialogue", "\xE3\x80\x8C\xE3\x81\x82\xE3\x80\x8D");
    line["speaker"] = nullptr;
    bj::array entries{mk("name", NAOYA), std::move(line)};
    EXPECT_EQ(tr::iter_entries_with_speakers(entries)[1].first, "NARRATION");
}

// A document written before the field existed still resolves.
TEST(Translate, entries_without_the_field_fall_back_to_the_state_machine) {
    bj::array entries{mk("name", NAOYA), mk("dialogue", "x")};
    tr::SpeakerSource source;
    auto rows = tr::iter_entries_with_speakers(entries, &source);
    EXPECT_EQ(rows[1].first, "Naoya");
    EXPECT_EQ(source.from_field, 0u);
    EXPECT_EQ(source.disagreements, 0u);
}

// The field carries the Japanese plate, so it goes through the same table the
// state machine uses and an unpinned label still reaches the prompt.
TEST(Translate, an_unpinned_speaker_field_passes_through_verbatim) {
    bj::object line = mk("dialogue", "x");
    line["speaker"] = "\xE6\x9C\xAA\xE7\x9F\xA5";  // 未知
    bj::array entries{std::move(line)};
    EXPECT_EQ(tr::iter_entries_with_speakers(entries)[0].first, "\xE6\x9C\xAA\xE7\x9F\xA5");
}

TEST(Translate, strip_speaker_tags_removes_echoed_tags) {
    EXPECT_EQ(tr::strip_speaker_tags("[NARRATION] The cherry trees."), "The cherry trees.");
    EXPECT_EQ(tr::strip_speaker_tags("[Naoya] Good morning."), "Good morning.");
    EXPECT_EQ(tr::strip_speaker_tags("  Good morning.  "), "Good morning.");
    EXPECT_EQ(tr::strip_speaker_tags("No tags here"), "No tags here");
}

// Only a tag the model echoed in front of the translation goes.  A bracketed
// group inside the line is content the source asked for, and deleting it would
// silently drop words from a paid translation.
TEST(Translate, strip_speaker_tags_leaves_brackets_inside_the_line) {
    EXPECT_EQ(tr::strip_speaker_tags("He read the sign [EXIT] and stopped."),
              "He read the sign [EXIT] and stopped.");
    EXPECT_EQ(tr::strip_speaker_tags("\"Wait,\" he said [NARRATION] later."),
              "\"Wait,\" he said [NARRATION] later.");
    // A leading tag is still removed, and only the leading one.
    EXPECT_EQ(tr::strip_speaker_tags("[Shizuru] the sign said [EXIT]"),
              "the sign said [EXIT]");
}

TEST(Translate, strip_trims_unicode_whitespace) {
    // U+3000 IDEOGRAPHIC SPACE must go: the engine pads name-window strings
    // with it, and an ASCII-only trim would key the TSV on a string the
    // engine never emits.
    EXPECT_EQ(tr::strip("\xE3\x80\x80Naoya\xE3\x80\x80"), "Naoya");
    EXPECT_EQ(tr::strip("  spaced \t\n"), "spaced");
    // U+200B ZERO WIDTH SPACE is a formatting character, not whitespace --
    // trimming it would change the key.  Must survive.
    EXPECT_EQ(tr::strip("\xE2\x80\x8B"), "\xE2\x80\x8B");
}

TEST(Translate, user_prompt_shape) {
    std::string p = tr::build_user_prompt({{"Naoya", "\xE3\x81\x82"}, {"NARRATION", "\xE3\x81\x84"}},
                                          {{"Misuzu", "Previous line."}});
    EXPECT_NE(p.find("<previous_context>\n[Misuzu] Previous line.\n</previous_context>"),
              std::string::npos);
    EXPECT_NE(p.find("1. [Naoya] \xE3\x81\x82\n2. [NARRATION] \xE3\x81\x84\n"),
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
    c.set_default("a", "IGNORED");
    EXPECT_EQ(*c.get("a"), "A");
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

// The prompt's name list is a view of the speaker table, not a second copy of
// it, so no edit can give the model one spelling and the runtime another.
TEST(Translate, the_prompt_name_list_is_read_from_the_speaker_table) {
    const auto& names = tr::prompt_name_translations();
    EXPECT_FALSE(names.empty());
    const auto& map = tr::name_translations();
    for (const auto& [jp, en] : names) {
        const auto it = map.find(jp);
        ASSERT_NE(it, map.end()) << jp << " is not in the speaker table";
        EXPECT_EQ(it->second, en) << "the prompt spells " << jp << " differently";
    }
    // A subset: the roles and walk-on plates stay out of the prompt.
    EXPECT_LT(names.size(), tr::name_translations_ordered().size());
}

TEST(Translate, the_system_prompt_carries_the_derived_name_list) {
    const std::string& prompt = tr::system_prompt();
    ASSERT_NE(prompt.find("<character_names>"), std::string::npos);
    for (const auto& [jp, en] : tr::prompt_name_translations())
        EXPECT_NE(prompt.find("- " + jp + " = " + en), std::string::npos)
            << jp << " is missing from the rendered name list";
}

// The answer writes a soft break as a literal backslash-n, and the cache holds
// what the game renders, so the two characters become one break before the
// translation is stored.  There is no escaping counterpart: no Japanese string
// in this corpus carries a break (0 of 92,774), so the prompt never sends one.
TEST(Translate, an_escaped_break_in_the_answer_becomes_a_real_break) {
    EXPECT_EQ(tr::unescape_line_breaks("first\\nsecond"), "first\nsecond");
    EXPECT_EQ(tr::unescape_line_breaks("one\\ntwo\\nthree"), "one\ntwo\nthree");
    EXPECT_EQ(tr::unescape_line_breaks("no break here"), "no break here");

    // A blank written against the escape belongs to neither side of the break.
    EXPECT_EQ(tr::unescape_line_breaks("first \\n second"), "first\nsecond");
    // U+3000 is the paragraph indent the prompt asks the answer to copy, so it
    // is content rather than a blank.
    EXPECT_EQ(tr::unescape_line_breaks(std::string("first\\n") + U3000 + "second"),
              std::string("first\n") + U3000 + "second");
    // A backslash in front of anything else stays exactly where it is.
    EXPECT_EQ(tr::unescape_line_breaks("a\\tb"), "a\\tb");
    EXPECT_EQ(tr::unescape_line_breaks("trailing backslash\\"), "trailing backslash\\");
}

TEST(Translate, a_parsed_answer_reaches_the_cache_with_its_break_intact) {
    const auto t = tr::parse_numbered_response("1. First half\\nsecond half\n2. Plain\n", 2);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t.at(1), "First half\nsecond half");
    EXPECT_EQ(t.at(2), "Plain");

    // A break the answer left hanging off the end is trimmed, not cached.
    const auto tail = tr::parse_numbered_response("1. Ends here\\n\n", 1);
    ASSERT_EQ(tail.size(), 1u);
    EXPECT_EQ(tail.at(1), "Ends here");
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
        EXPECT_NE(why->find("--max-batches"), std::string::npos) << *why;
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

// A batch that fails leaves its lines out of the cache, so the next run asks
// for exactly those lines again.  Writing the Japanese in as its own English
// would mark them done for good.
TEST(Translate, an_answer_that_never_arrived_is_not_cached) {
    tr::Cache cache;
    std::vector<std::pair<std::string, std::string>> context;

    tr::apply_line_result(cache, context, "Naoya", "「おはよう」", "");
    EXPECT_FALSE(cache.contains("「おはよう」"));
    EXPECT_EQ(cache.size(), 0u);
    // The context window still reads in sequence.
    ASSERT_EQ(context.size(), 1u);
    EXPECT_EQ(context.front().second, "「おはよう」");

    tr::apply_line_result(cache, context, "Rin", "朝が来た。", "Morning came.");
    ASSERT_TRUE(cache.contains("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    EXPECT_EQ(context.back().second, "Morning came.");
}

// --------------------------------------------------------------------------- //
// max_tokens and option validation -- the two guards in front of the spend
// --------------------------------------------------------------------------- //

// A request asking for more output than the model can emit is rejected
// outright, so a batch big enough to compute past the ceiling has to clamp
// rather than send the raw product.
TEST(Translate, max_tokens_stays_inside_the_model_ceiling) {
    EXPECT_EQ(tr::compute_max_tokens(500), tr::MAX_OUTPUT_TOKENS);
    EXPECT_LE(tr::compute_max_tokens(500), 128000);
    EXPECT_EQ(tr::compute_max_tokens(150), 150 * tr::OUTPUT_TOKENS_PER_LINE);
    EXPECT_EQ(tr::compute_max_tokens(1), tr::MIN_OUTPUT_TOKENS);   // floor
    EXPECT_EQ(tr::compute_max_tokens(0), tr::MIN_OUTPUT_TOKENS);
    EXPECT_EQ(tr::compute_max_tokens(-5), tr::MIN_OUTPUT_TOKENS);  // no overflow
    for (int batch : {1, 50, 150, 426, 427, 1000, 100000})
        EXPECT_LE(tr::compute_max_tokens(batch), tr::MAX_OUTPUT_TOKENS) << batch;
}

// The batch cap is a spend limit, so every value that is not a limit has to be
// refused.  A negative one reads as "no limit" wherever it is compared against
// zero, which is the opposite of what it says.
TEST(Translate, option_validation_refuses_values_that_would_overspend) {
    EXPECT_FALSE(tr::validate_options(150, 0, 0));   // full run
    EXPECT_FALSE(tr::validate_options(150, 0, 1));   // one bounded request
    EXPECT_FALSE(tr::validate_options(1, 3, 0));     // smoke run

    EXPECT_TRUE(tr::validate_options(150, 0, -1));   // the fail-open case
    EXPECT_TRUE(tr::validate_options(150, 0, -999));
    EXPECT_TRUE(tr::validate_options(150, -1, 0));
    EXPECT_TRUE(tr::validate_options(0, 0, 0));      // batch 0 never advances
    EXPECT_TRUE(tr::validate_options(-5, 0, 0));

    // The message names the offending flag, since it is printed and nothing else.
    EXPECT_NE(tr::validate_options(150, 0, -1)->find("--max-batches"), std::string::npos);
    EXPECT_NE(tr::validate_options(0, 0, 0)->find("--batch"), std::string::npos);
}

// The banner must describe the request that is actually built.  Both read the
// same function, so this pins them together.
TEST(Translate, reasoning_banner_matches_the_transmitted_field) {
    EXPECT_EQ(tr::reasoning_control(tr::MODEL_FLASH),
              tr::ReasoningControl::AdaptiveThinking);
    EXPECT_EQ(tr::reasoning_control(tr::MODEL_FLASH_25), tr::ReasoningControl::Effort);
    EXPECT_EQ(tr::reasoning_control(tr::MODEL_PRO), tr::ReasoningControl::Effort);
    EXPECT_EQ(tr::reasoning_control(tr::MODEL_FLASH_LITE), tr::ReasoningControl::None);

    // Whatever the configured model is, the label may mention effort only when
    // effort is what goes into the body.
    const std::string label = tr::reasoning_control_label(tr::MODEL);
    const bool mentions_effort = label.find("effort") != std::string::npos;
    EXPECT_EQ(mentions_effort,
              tr::reasoning_control(tr::MODEL) == tr::ReasoningControl::Effort);
}

// --------------------------------------------------------------------------- //
// Numbering: which lines open an entry, and which are prose
// --------------------------------------------------------------------------- //

// A backward-pointing number on a continuation line would silently replace an
// already-parsed translation.  It is read as prose instead.
TEST(Translate, parse_numbered_response_keeps_a_backward_number_as_prose) {
    const std::string resp =
        "1. He came back at dawn.\n"
        "2. \"It has been a while,\" she said.\n"
        "3. The war ended without a sound.\n"
        "2 days later, the rain came.\n"
        "4. Nothing was left of the garden.\n";
    auto t = tr::parse_numbered_response(resp, 4);
    ASSERT_EQ(t.size(), 4u);
    EXPECT_EQ(t.at(1), "He came back at dawn.");
    EXPECT_EQ(t.at(2), "\"It has been a while,\" she said.")
        << "an already-parsed translation was overwritten";
    EXPECT_EQ(t.at(3), "The war ended without a sound. 2 days later, the rain came.");
    EXPECT_EQ(t.at(4), "Nothing was left of the garden.");

    // The ordering rule alone catches this one, so it holds without a count.
    EXPECT_EQ(tr::parse_numbered_response(resp).at(2),
              "\"It has been a while,\" she said.");
}

// A forward-pointing number would file text under a line nobody asked for.
// The batch size bounds it, so the parser reads it as prose.
TEST(Translate, parse_numbered_response_keeps_a_forward_number_as_prose) {
    auto t = tr::parse_numbered_response(
        "1. He never spoke of the painting again.\n"
        "1985 came and went, and still he said nothing.\n"
        "2. The gallery closed that spring.\n",
        2);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t.at(1),
              "He never spoke of the painting again. 1985 came and went, and still "
              "he said nothing.");
    EXPECT_EQ(t.at(2), "The gallery closed that spring.");
    EXPECT_EQ(t.count(1985), 0u) << "a line number nobody asked for was invented";

    // A digit run too long to be a line number is prose whatever the count is.
    auto huge = tr::parse_numbered_response("1. Priced at\n999999999999 yen.\n", 1);
    ASSERT_EQ(huge.size(), 1u);
    EXPECT_EQ(huge.at(1), "Priced at 999999999999 yen.");
}

// A year written with a separator reads as a line number nobody asked for, so
// the numbering cannot be trusted and the batch is discarded whole.
TEST(Translate, parse_numbered_response_rejects_a_year_that_reads_as_a_line_number) {
    int rejected = -1;
    auto t = tr::parse_numbered_response(
        "1. He never spoke of the painting again.\n"
        "1985. That was the year, he said.\n"
        "2. The gallery closed that spring.\n",
        2, &rejected);
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(rejected, 1);
}

// The number equal to the count asked for is the last legal one, and all three
// separators the prompt names still open an entry.
TEST(Translate, parse_numbered_response_accepts_every_requested_separator) {
    auto t = tr::parse_numbered_response("1. First line\n2) Second line\n3: Third line\n", 3);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t.at(1), "First line");
    EXPECT_EQ(t.at(2), "Second line");
    EXPECT_EQ(t.at(3), "Third line") << "the number equal to the count was refused";
}

// Numbering that is out of order, duplicated or past the count cannot be lined
// up with the batch, so the whole answer goes and the batch re-queues.
TEST(Translate, parse_numbered_response_discards_a_batch_it_cannot_trust) {
    struct Case {
        const char* what;
        const char* body;
        int expected;
    };
    const Case cases[] = {
        {"out of order", "1. A\n3. C\n2. B\n4. D\n", 4},
        {"duplicate number", "2. B\n2. B-prime\n", 2},
        {"renumbered from 1 midway", "1. A\n2. B\n1. C\n2. D\n", 4},
        {"past the count asked for", "1. A\n2. B\n5. E\n", 2},
        {"model-invented sublist",
         "1. First line\n2. Second line\n   1. an aside\n   2. another\n3. Third line\n", 3},
        {"number alone on its line", "1. First\n2.\nthe text came a line late\n", 2},
    };
    for (const auto& c : cases) {
        int rejected = 0;
        auto t = tr::parse_numbered_response(c.body, c.expected, &rejected);
        EXPECT_TRUE(t.empty()) << c.what << ": kept " << t.size()
                               << " entries instead of re-queuing the batch";
        EXPECT_GT(rejected, 0) << c.what << ": the unusable line was not counted";
    }

    // The ordering rule holds without a count too.
    EXPECT_TRUE(tr::parse_numbered_response("1. A\n3. C\n2. B\n").empty());
}

// A clean answer reports nothing rejected, so the count separates "the model
// answered short" from "the answer was thrown away".
TEST(Translate, parse_numbered_response_reports_the_rejected_line_count) {
    int rejected = -1;
    auto good = tr::parse_numbered_response("1. A\n2. B\n", 2, &rejected);
    EXPECT_EQ(good.size(), 2u);
    EXPECT_EQ(rejected, 0);

    rejected = -1;
    auto bad = tr::parse_numbered_response("1. A\n2. B\n1. C\n1. D\n", 4, &rejected);
    EXPECT_TRUE(bad.empty());
    EXPECT_EQ(rejected, 2);

    // The out-parameters are optional; the shorter forms still work.
    EXPECT_EQ(tr::parse_numbered_response("1. A\n", 1).size(), 1u);
    EXPECT_EQ(tr::parse_numbered_response("1. A\n").size(), 1u);
}

// Every numbering shape the model writes has to be recognised as an opener; a
// missed one silently glues two translations together.
TEST(Translate, parse_numbered_response_reads_every_numbering_the_model_writes) {
    struct Case {
        const char* what;
        std::string body;
    };
    const std::vector<Case> cases = {
        {"one blank", "1. Alpha"},
        {"two blanks", "1.  Alpha"},
        {"no blank at all", "1.Alpha"},
        {"fullwidth space for the blank", std::string("1.") + U3000 + "Alpha"},
        {"blank before the separator", "1 . Alpha"},
        {"paren separator", "1) Alpha"},
        {"colon separator", "1: Alpha"},
        {"fullwidth digit", std::string(FULLWIDTH_ONE) + ". Alpha"},
        {"fullwidth separator", std::string("1") + FULLWIDTH_STOP + " Alpha"},
        {"markdown emphasis", "**1.** Alpha"},
        {"quote marker", "> 1. Alpha"},
        {"ascii indent", "   1. Alpha"},
        {"fullwidth indent", std::string(U3000) + "1. Alpha"},
    };
    for (const auto& c : cases) {
        int rejected = -1;
        auto t = tr::parse_numbered_response(c.body + "\n", 1, &rejected);
        ASSERT_EQ(t.size(), 1u) << c.what;
        EXPECT_EQ(t.at(1), "Alpha") << c.what;
        EXPECT_EQ(rejected, 0) << c.what;
    }
}

// A digit after the separator is what tells a line number from ordinary
// English, so a date, a time, a decimal and a sub-point all stay prose.
TEST(Translate, parse_numbered_response_keeps_a_prose_opener_as_prose) {
    auto with_third_line = [](const char* third) {
        return std::string("1. He came back at dawn.\n") + "2. The house was empty.\n" +
               third + "\n3. She never wrote back.\n4. Nothing was left of the garden.\n";
    };
    for (const char* prose : {"3 days later, the rain stopped.", "3:00 sharp, he left.",
                              "3.5 cm across."}) {
        auto t = tr::parse_numbered_response(with_third_line(prose), 4);
        ASSERT_EQ(t.size(), 4u) << prose;
        EXPECT_EQ(t.at(2), std::string("The house was empty. ") + prose) << prose;
        EXPECT_EQ(t.at(3), "She never wrote back.")
            << "the real entry 3 was swallowed by: " << prose;
    }

    auto sub = tr::parse_numbered_response(
        "1. The list was short.\n"
        "1.1 sub point, he wrote in the margin.\n"
        "2. Nothing else was on the page.\n",
        2);
    ASSERT_EQ(sub.size(), 2u);
    EXPECT_EQ(sub.at(1), "The list was short. 1.1 sub point, he wrote in the margin.");
}

// Numbering nobody can read is reported and rejected; folding it in silently
// would glue two translations together.
TEST(Translate, parse_numbered_response_refuses_numbering_it_cannot_read) {
    struct Case {
        const char* what;
        const char* body;
    };
    const Case cases[] = {
        {"hyphen for a mark", "1. Alpha\n2- Bravo\n3. Charlie\n"},
        {"colon spaced away from the number", "1. Alpha\n7 : 30 he arrived\n2. Bravo\n"},
        {"paren spaced away from the number", "1. Alpha\n3 ) whatever\n2. Bravo\n"},
        {"the mark alone on its line", "1. Alpha\n2..\n"},
    };
    for (const auto& c : cases) {
        int rejected = -1;
        auto t = tr::parse_numbered_response(c.body, 3, &rejected);
        EXPECT_TRUE(t.empty()) << c.what << ": folded into the entry before it";
        EXPECT_GT(rejected, 0) << c.what << ": no signal at all";
    }

    // Reading a hyphen as numbering would steal an entry that is in range and
    // in order, so a range and a measurement stay prose.
    int rejected = -1;
    auto prose = tr::parse_numbered_response(
        "1. He waited.\n1985-1990 was the whole of it.\n2. Then he left.\n", 2, &rejected);
    ASSERT_EQ(prose.size(), 2u);
    EXPECT_EQ(rejected, 0);
    EXPECT_EQ(prose.at(1), "He waited. 1985-1990 was the whole of it.");
}

// Emphasis the model wrapped the numbering in is numbering.  Emphasis the
// translation opens for itself is text.
TEST(Translate, parse_numbered_response_keeps_emphasis_out_of_the_numbering) {
    struct Case {
        const char* body;
        const char* want;
    };
    const Case cases[] = {
        {"**1. Alpha**", "Alpha"}, {"**1.** Bravo", "Bravo"},
        {"**1.** *Charlie*", "*Charlie*"}, {"1.*sigh*", "*sigh*"},
        {"1.. Delta", "Delta"}, {"1.Echo", "Echo"},
    };
    for (const auto& c : cases) {
        int rejected = -1;
        auto t = tr::parse_numbered_response(std::string(c.body) + "\n", 1, &rejected);
        ASSERT_EQ(t.size(), 1u) << c.body;
        EXPECT_EQ(t.at(1), c.want) << c.body;
        EXPECT_EQ(rejected, 0) << c.body;
    }
}

// Japanese numbering marks must be recognised, or the line folds into the
// entry before it.
TEST(Translate, parse_numbered_response_reads_a_japanese_numbering_mark) {
    const char* const IDEOGRAPHIC_STOP = "\xE3\x80\x82";   // U+3002
    const char* const IDEOGRAPHIC_COMMA = "\xE3\x80\x81";  // U+3001
    for (const char* mark : {IDEOGRAPHIC_STOP, IDEOGRAPHIC_COMMA}) {
        int rejected = -1;
        auto t = tr::parse_numbered_response(
            "1" + std::string(mark) + " Alpha\n2" + mark + " Bravo\n", 2, &rejected);
        ASSERT_EQ(t.size(), 2u) << mark;
        EXPECT_EQ(t.at(1), "Alpha") << mark;
        EXPECT_EQ(t.at(2), "Bravo") << mark;
        EXPECT_EQ(rejected, 0) << mark;
    }
}

// Decoration is skipped to FIND the number, never removed from the text, so a
// translation that genuinely opens with one of those characters is kept whole.
TEST(Translate, parse_numbered_response_leaves_decoration_inside_a_translation_alone) {
    auto t = tr::parse_numbered_response(
        "1. *softly, from the doorway*\n"
        "> and then nothing at all\n"
        "2. #1 in her year, she said.\n",
        2);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t.at(1), "*softly, from the doorway* > and then nothing at all");
    EXPECT_EQ(t.at(2), "#1 in her year, she said.");
}

TEST(Translate, parse_numbered_response_reads_crlf_line_endings) {
    auto t = tr::parse_numbered_response("1. Alpha\r\n2. Bravo\r\n", 2);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t.at(1), "Alpha");
    EXPECT_EQ(t.at(2), "Bravo");
}

// Anything before the first number belongs to no entry, so it is dropped
// rather than filed under one.
TEST(Translate, parse_numbered_response_drops_anything_before_the_first_entry) {
    auto prose = tr::parse_numbered_response(
        "Here are the translations:\n\n1. Alpha\n2. Bravo\n", 2);
    ASSERT_EQ(prose.size(), 2u);
    EXPECT_EQ(prose.at(1), "Alpha");

    // A preamble that starts with a number carries no separator, so it cannot
    // be read as a line number either.
    auto numbered = tr::parse_numbered_response(
        "2 lines follow, translated below\n1. Alpha\n2. Bravo\n", 2);
    ASSERT_EQ(numbered.size(), 2u);
    EXPECT_EQ(numbered.at(2), "Bravo");

    int rejected = -1;
    EXPECT_TRUE(tr::parse_numbered_response("", 2, &rejected).empty());
    EXPECT_EQ(rejected, 0);
    EXPECT_TRUE(tr::parse_numbered_response("  \n\t\n \r\n", 2, &rejected).empty());
    EXPECT_EQ(rejected, 0);
}

// A remark after the last entry is reported, not rejected: nothing tells it
// from a translation that ran onto a second line.
TEST(Translate, parse_numbered_response_reports_text_hanging_off_the_last_entry) {
    std::string tail = "unset";
    auto clean = tr::parse_numbered_response("1. Alpha\n2. Bravo\n", 2, nullptr, &tail);
    ASSERT_EQ(clean.size(), 2u);
    EXPECT_TRUE(tail.empty()) << "a single-line last entry reported a tail";

    auto noted = tr::parse_numbered_response(
        "1. Alpha\n2. Bravo\nNote: I kept the ruby base words only.\n", 2, nullptr, &tail);
    ASSERT_EQ(noted.size(), 2u) << "the count still matches, so nothing else warns";
    EXPECT_EQ(noted.at(2), "Bravo Note: I kept the ruby base words only.");
    EXPECT_EQ(tail, "Note: I kept the ruby base words only.");

    // Only the LAST entry is reported: an earlier one running over two lines is
    // ordinary and says nothing about trailing commentary.
    auto wrapped = tr::parse_numbered_response(
        "1. Alpha\nand the rest of it\n2. Bravo\n", 2, nullptr, &tail);
    ASSERT_EQ(wrapped.size(), 2u);
    EXPECT_EQ(wrapped.at(1), "Alpha and the rest of it");
    EXPECT_TRUE(tail.empty());
}

// A skipped number leaves a gap the caller re-queues, and the production batch
// size is the scale the guards actually run at.
TEST(Translate, parse_numbered_response_holds_over_a_production_batch) {
    auto gapped = tr::parse_numbered_response(
        "1. The first line\nruns on for a second physical line\n"
        "2. Second line\n4. Fourth line -- the third never came back\n",
        4);
    ASSERT_EQ(gapped.size(), 3u);
    EXPECT_EQ(gapped.at(1), "The first line runs on for a second physical line");
    EXPECT_EQ(gapped.count(3), 0u);

    const int kLines = 300;
    std::string body;
    for (int i = 1; i <= kLines; ++i)
        body += std::to_string(i) + ". Line " + std::to_string(i) + "\n";

    int rejected = -1;
    std::string tail = "unset";
    auto t = tr::parse_numbered_response(body, kLines, &rejected, &tail);
    ASSERT_EQ(t.size(), static_cast<std::size_t>(kLines));
    EXPECT_EQ(t.at(1), "Line 1");
    EXPECT_EQ(t.at(kLines), "Line 300");
    EXPECT_EQ(rejected, 0);
    EXPECT_TRUE(tail.empty());

    auto strayed = tr::parse_numbered_response(body + "That's all of them.\n", kLines,
                                               &rejected, &tail);
    ASSERT_EQ(strayed.size(), static_cast<std::size_t>(kLines));
    EXPECT_EQ(strayed.at(kLines), "Line 300 That's all of them.");
    EXPECT_EQ(rejected, 0) << "a stray last line is not a numbering fault";
    EXPECT_EQ(tail, "That's all of them.");
}

// Known blind spot, pinned so it stays a decision rather than a surprise: a
// skipped number whose continuation opens with exactly that number passes
// every guard there is.
TEST(Translate, parse_numbered_response_cannot_tell_a_skipped_number_from_a_real_one) {
    auto t = tr::parse_numbered_response(
        "1. He came back at dawn.\n"
        "2. The house was empty, and\n"
        "3. days later it was still empty.\n"
        "4. Nothing was left of the garden.\n",
        4);
    ASSERT_EQ(t.size(), 4u);
    EXPECT_EQ(t.at(2), "The house was empty, and") << "the entry lost its tail";
    EXPECT_EQ(t.at(3), "days later it was still empty.") << "and the tail became an entry";
}

// --------------------------------------------------------------------------- //
// Answer guards
// --------------------------------------------------------------------------- //

// A self-stopped answer with fewer entries than asked for may have merged two
// lines, and nothing says which two, so the count is the only guard there is.
TEST(Translate, a_short_answer_that_stopped_on_its_own_cannot_be_applied) {
    EXPECT_TRUE(tr::answer_count_is_usable(150, 150, "end_turn"));
    EXPECT_FALSE(tr::answer_count_is_usable(150, 149, "end_turn"));
    EXPECT_FALSE(tr::answer_count_is_usable(150, 149, ""));  // stop_reason absent
    EXPECT_FALSE(tr::answer_count_is_usable(150, 0, "end_turn"));
    EXPECT_FALSE(tr::answer_count_is_usable(150, 151, "end_turn"));

    // A cut answer is short because the budget ran out, and the entry the cut
    // landed in has already been dropped.  Gating on the count there would
    // throw away the paid prefix and re-request a batch that cuts again.
    EXPECT_TRUE(tr::answer_count_is_usable(150, 90, "max_tokens"));
    EXPECT_TRUE(tr::answer_count_is_usable(150, 90, "model_context_window_exceeded"));

    // The merge itself: nothing in the numbering is wrong.
    int rejected = -1;
    auto merged = tr::parse_numbered_response("1. A\n2. B and C together\n3. D\n", 4,
                                              &rejected);
    ASSERT_EQ(merged.size(), 3u);
    EXPECT_EQ(rejected, 0) << "the numbering is in order and in range";
    EXPECT_FALSE(tr::answer_count_is_usable(4, merged.size(), "end_turn"));
}

// An answer cut at the token limit ends mid-sentence.  That fragment is not
// empty, so it would cache as a finished translation and the line would never
// come back.
TEST(Translate, truncated_answer_loses_only_its_last_entry) {
    const std::map<int, std::string> full = {
        {1, "He turned and left."}, {2, "The door stayed open."},
        {3, "He turned and said, \"I have always"}};

    auto t = full;
    EXPECT_TRUE(tr::drop_truncated_entry(t, "max_tokens"));
    EXPECT_EQ(t.size(), 2u);
    EXPECT_EQ(t.count(3), 0u) << "the half-written entry was kept";
    EXPECT_EQ(t.at(1), "He turned and left.");

    // Running out of context window cuts the answer the same way.
    auto ctx = full;
    EXPECT_TRUE(tr::drop_truncated_entry(ctx, "model_context_window_exceeded"));
    EXPECT_EQ(ctx.size(), 2u);

    // A finished answer is left alone, whatever else the model reports.
    for (const char* reason : {"end_turn", "stop_sequence", ""}) {
        auto keep = full;
        EXPECT_FALSE(tr::drop_truncated_entry(keep, reason)) << reason;
        EXPECT_EQ(keep.size(), 3u) << reason;
    }

    // The cut lands in the highest entry PRESENT, which a gap makes different
    // from the number of lines that were asked for.
    std::map<int, std::string> sparse = {{1, "One."}, {2, "Two."}, {7, "Seven and a ha"}};
    EXPECT_TRUE(tr::drop_truncated_entry(sparse, "max_tokens"));
    EXPECT_EQ(sparse.count(7), 0u);
    EXPECT_EQ(sparse.size(), 2u);

    // Nothing to drop when the batch was already discarded.
    std::map<int, std::string> none;
    EXPECT_FALSE(tr::drop_truncated_entry(none, "max_tokens"));
}

// A refusal discards the whole batch: dropping only the last entry would cache
// its fragmented neighbours for good.
TEST(Translate, a_refused_answer_is_discarded_whole) {
    const std::map<int, std::string> full = {
        {1, "He turned and left."}, {2, "The door stayed open."},
        {3, "He turned and said, \"I have always"}};

    auto refused = full;
    EXPECT_TRUE(tr::discard_refused_answer(refused, "refusal"));
    EXPECT_TRUE(refused.empty()) << "entries survived a refusal and would be cached";

    // Dropping the last entry is the wrong repair here, so the two must not be
    // confused: this is the pair the caller applies, in this order.
    auto both = full;
    ASSERT_TRUE(tr::discard_refused_answer(both, "refusal"));
    EXPECT_FALSE(tr::drop_truncated_entry(both, "refusal"));
    EXPECT_TRUE(both.empty());

    for (const char* reason : {"end_turn", "max_tokens", "stop_sequence",
                               "model_context_window_exceeded", ""}) {
        auto keep = full;
        EXPECT_FALSE(tr::discard_refused_answer(keep, reason)) << reason;
        EXPECT_EQ(keep.size(), 3u) << reason;
    }

    // A refusal before any text is still a rejected batch, so the caller can
    // report it instead of reading an empty answer as "the model said nothing".
    std::map<int, std::string> none;
    EXPECT_TRUE(tr::discard_refused_answer(none, "refusal"));
}

// With adaptive thinking the first content block is thinking and carries no
// text, so the reader has to walk past it to find the translations.
TEST(Translate, the_answer_is_read_past_the_thinking_block) {
    const tr::Answer answer = tr::read_answer(api_response("1. Alpha\n", "end_turn"));
    EXPECT_EQ(answer.text, "1. Alpha");
    EXPECT_EQ(answer.stop_reason, "end_turn");

    // A response with no content and no stop_reason is not a crash.
    const tr::Answer empty = tr::read_answer(bj::value(bj::object{}));
    EXPECT_TRUE(empty.text.empty());
    EXPECT_TRUE(empty.stop_reason.empty());
}

// The three guards applied to a whole answer, in the order the caller uses.
TEST(Translate, an_answer_that_cannot_be_lined_up_yields_nothing) {
    // One line short of the batch, stopped on its own: unusable.
    EXPECT_TRUE(tr::usable_translations(
                    tr::read_answer(api_response("1. Alpha\n2. Bravo\n", "end_turn")), 3)
                    .empty());

    // The same shortfall after a cut keeps the prefix and drops the fragment.
    auto cut = tr::usable_translations(
        tr::read_answer(api_response("1. Alpha\n2. Bravo\n3. Charlie and th", "max_tokens")),
        150);
    ASSERT_EQ(cut.size(), 2u);
    EXPECT_EQ(cut.at(2), "Bravo");
    EXPECT_EQ(cut.count(3), 0u);

    // A refusal keeps nothing at all.
    EXPECT_TRUE(tr::usable_translations(
                    tr::read_answer(api_response("1. Alpha\n2. Bravo\n", "refusal")), 2)
                    .empty());

    // A complete answer passes through whole.
    auto whole = tr::usable_translations(
        tr::read_answer(api_response("1. Alpha\n2. Bravo\n", "end_turn")), 2);
    ASSERT_EQ(whole.size(), 2u);
    EXPECT_EQ(whole.at(1), "Alpha");
}

// --------------------------------------------------------------------------- //
// What a failed batch leaves behind
// --------------------------------------------------------------------------- //

// A client that fails leaves the cache exactly as it was, so the next run asks
// for the same lines again.  Writing the Japanese in as its own English would
// mark them done for good, and nothing in this pipeline can rebuild them.
TEST(Translate, a_batch_whose_client_fails_leaves_the_cache_untouched) {
    const auto batch = batch_of(3);
    for (const bool rate_limited : {false, true}) {
        tr::Cache cache;
        cache.set("朝が来た。", "Morning came.");
        std::vector<std::pair<std::string, std::string>> context;

        tr::translate_batch(cache, context, batch,
                            [&]() -> std::map<int, std::string> {
                                if (rate_limited)
                                    throw snt::gemini::RateLimitError("429");
                                throw snt::gemini::ApiError("500");
                            },
                            std::chrono::seconds(0));

        EXPECT_EQ(cache.size(), 1u) << "rate_limited=" << rate_limited;
        for (const auto& [speaker, jp] : batch)
            EXPECT_FALSE(cache.contains(jp)) << jp;
        // The Japanese still reaches the context window, so the next request
        // reads in sequence.
        ASSERT_EQ(context.size(), batch.size());
        EXPECT_EQ(context.front().second, batch.front().second);
    }
}

// An answer one line short of the batch is discarded before it reaches the
// cache, because nothing says which line the numbering slipped on.
TEST(Translate, an_answer_short_of_the_batch_caches_nothing) {
    const auto batch = batch_of(3);
    tr::Cache cache;
    std::vector<std::pair<std::string, std::string>> context;

    tr::translate_batch(cache, context, batch, [&] {
        return tr::usable_translations(
            tr::read_answer(api_response("1. Alpha\n2. Bravo\n", "end_turn")), batch.size());
    });

    EXPECT_EQ(cache.size(), 0u) << "a batch that cannot be lined up was cached anyway";
    ASSERT_EQ(context.size(), batch.size());

    // The same three lines, answered in full, do reach the cache.
    tr::Cache good;
    std::vector<std::pair<std::string, std::string>> good_context;
    tr::translate_batch(good, good_context, batch, [&] {
        return tr::usable_translations(
            tr::read_answer(api_response("1. Alpha\n2. Bravo\n3. Charlie\n", "end_turn")),
            batch.size());
    });
    ASSERT_EQ(good.size(), 3u);
    ASSERT_TRUE(good.get(batch[2].second));
    EXPECT_EQ(*good.get(batch[2].second), "Charlie");
}

// A batch answered in part caches the lines that came back and leaves the rest
// queued, rather than filling the gap with the Japanese.
TEST(Translate, a_gap_in_the_answer_leaves_that_one_line_queued) {
    const auto batch = batch_of(3);
    tr::Cache cache;
    std::vector<std::pair<std::string, std::string>> context;

    // A cut answer is the shape that legitimately arrives short.
    tr::translate_batch(cache, context, batch, [&] {
        return tr::usable_translations(
            tr::read_answer(api_response("1. Alpha\n3. Charlie\n", "max_tokens")),
            batch.size());
    });

    ASSERT_EQ(cache.size(), 1u);
    ASSERT_TRUE(cache.get(batch[0].second));
    EXPECT_EQ(*cache.get(batch[0].second), "Alpha");
    EXPECT_FALSE(cache.contains(batch[1].second)) << "the gap was filled with Japanese";
    EXPECT_FALSE(cache.contains(batch[2].second)) << "the cut fragment was cached";
}

// The side log sits beside the document it belongs to, and stays off unless a
// run asks for it.
TEST(Translate, the_raw_answer_log_sits_beside_the_run_it_belongs_to) {
    const std::string sibling =
        (fs::temp_directory_path() / "script_output" / "translated_text.json").u8string();
    const std::string log = tr::raw_answer_log_path(sibling);
    EXPECT_EQ(fs::u8path(log).parent_path(), fs::u8path(sibling).parent_path());
    EXPECT_EQ(fs::u8path(log).filename().u8string(), "gemini_raw_answers.log");

    const std::string was = tr::raw_answer_log();
    tr::set_raw_answer_log(log);
    EXPECT_EQ(tr::raw_answer_log(), log);
    tr::set_raw_answer_log(was);
}

// --------------------------------------------------------------------------- //
// The gate in front of the spend
// --------------------------------------------------------------------------- //

// The regression the gate exists for: the walker stops finding the plate,
// every dialogue line reads [NARRATION], and the answer comes back fluent and
// wrong in every voice.
TEST(Translate, the_speaker_gate_refuses_a_document_that_lost_its_speakers) {
    const tr::SpeakerCheckReport lost = tr::run_speaker_checks(dialogue_document(50, ""));
    EXPECT_FALSE(lost.passed);
    EXPECT_EQ(lost.failed_check, 1);
    EXPECT_NE(lost.reason.find("NARRATION"), std::string::npos);

    // An extraction with almost nothing in it fails at the same check rather
    // than passing on a sample too small to mean anything.
    const tr::SpeakerCheckReport thin = tr::run_speaker_checks(dialogue_document(4, NAOYA));
    EXPECT_FALSE(thin.passed);
    EXPECT_EQ(thin.failed_check, 1);

    // Speakers alone are not enough: the table and the scripts have to cover
    // each other, which no partial document can satisfy.
    const tr::SpeakerCheckReport partial =
        tr::run_speaker_checks(dialogue_document(50, NAOYA));
    EXPECT_FALSE(partial.passed);
    EXPECT_EQ(partial.failed_check, 5);
}

// The gate over the deployed extraction, which is the document the paid run
// will read.  Skips when the extraction has not been produced yet.
TEST(Translate, the_speaker_gate_passes_over_the_current_extraction) {
    const fs::path extracted = fs::u8path(std::string(SNT_PROJECT_DIR) +
                                          "\\script_output\\extracted_text.json");
    if (!fs::exists(extracted)) GTEST_SKIP() << "extracted_text.json not built yet";

    const bj::value root = snt::json_parse_file(extracted.u8string());
    const tr::SpeakerCheckReport report = tr::run_speaker_checks(root.get_object());
    EXPECT_TRUE(report.passed) << "check " << report.failed_check << ": " << report.reason;
    EXPECT_EQ(report.failed_check, 0);
    EXPECT_EQ(report.speakers_in_data, 84u);
    EXPECT_EQ(report.glossary_entries, 84u)
        << "the table and the scripts have to cover each other exactly";
    EXPECT_EQ(report.dialogue_sampled, 50u);
}

// A refused gate stops the step before a key is read, before a client is built
// and before the fresh-run delete, so it costs nothing and loses nothing.
TEST(Translate, a_failed_gate_stops_the_translation_step_before_it_spends) {
    const fs::path dir = fs::temp_directory_path() / "snt_ut_gate";
    fs::remove_all(dir);
    fs::create_directories(dir);

    tr::TranslateOptions opt;
    opt.input_file = (dir / "extracted_text.json").u8string();
    opt.cache_file = (dir / "translation_cache_gemini.json").u8string();
    opt.output_file = (dir / "translated_text.json").u8string();
    opt.tsv_file = (dir / "translations.tsv").u8string();
    // A cache to lose, so "nothing was written" is a claim about this run
    // rather than about an empty folder.
    tr::Cache seeded;
    seeded.set("朝が来た。", "Morning came.");
    tr::save_cache(seeded, opt.cache_file);
    opt.fresh_run = true;

    snt::write_file(opt.input_file, snt::json_dump(bj::value(dialogue_document(50, ""))));

    EXPECT_EQ(tr::translate_all(opt), 2) << "a refused gate has to fail the step";
    EXPECT_FALSE(fs::exists(fs::u8path(opt.output_file)));
    EXPECT_FALSE(fs::exists(fs::u8path(opt.tsv_file)));
    EXPECT_TRUE(fs::exists(fs::u8path(opt.cache_file)))
        << "the run stopped before it could drop work already paid for";
    EXPECT_EQ(tr::load_cache(opt.cache_file).size(), 1u);

    fs::remove_all(dir);
}

// --gate-only answers over a document the gate accepts, then stops: it writes
// no document and no table, so it can be run at any time to ask whether the
// corpus is fit to buy.  The document is fully covered by the cache and the
// seeded name table, so this case sends no request whether or not the early
// return is there -- and without it the run would write the document, which is
// what the assertions below catch.
TEST(Translate, gate_only_answers_a_passing_gate_and_writes_nothing) {
    const fs::path dir = fs::temp_directory_path() / "snt_ut_gate_only";
    fs::remove_all(dir);
    fs::create_directories(dir);

    tr::TranslateOptions opt;
    opt.input_file = (dir / "extracted_text.json").u8string();
    opt.cache_file = (dir / "translation_cache_gemini.json").u8string();
    opt.output_file = (dir / "translated_text.json").u8string();
    opt.tsv_file = (dir / "translations.tsv").u8string();
    opt.gate_only = true;

    tr::Cache seeded;
    seeded.set(SPOKEN, "\"Ah.\"");
    tr::save_cache(seeded, opt.cache_file);
    snt::write_file(opt.input_file, snt::json_dump(bj::value(gate_passing_document())));

    EXPECT_EQ(tr::translate_all(opt), 0) << "the gate accepted the document";
    EXPECT_FALSE(fs::exists(fs::u8path(opt.output_file)))
        << "--gate-only wrote the document it was asked only to check";
    EXPECT_FALSE(fs::exists(fs::u8path(opt.tsv_file)));
    EXPECT_FALSE(fs::exists(fs::u8path(snt::coverage_report_path(opt.output_file))));
    ASSERT_TRUE(fs::exists(fs::u8path(opt.cache_file)));
    EXPECT_EQ(tr::load_cache(opt.cache_file).size(), 1u);

    fs::remove_all(dir);
}

// Three pipeline steps.  The speaker gate and the table build are folded into
// the translation step so they cannot be skipped, and the caps the
// orchestrator forwards are validated by the same rule the step applies.
TEST(Translate, the_pipeline_runs_three_steps_in_order) {
    const auto& steps = snt::apps::pipeline_steps();
    ASSERT_EQ(steps.size(), 3u);
    EXPECT_STREQ(steps[0].name, "01_extract");
    EXPECT_STREQ(steps[1].name, "02_translate");
    EXPECT_STREQ(steps[2].name, "deploy");
    for (const auto& step : steps) {
        EXPECT_EQ(std::string(step.name).find("speaker_gate"), std::string::npos);
        EXPECT_EQ(std::string(step.name).find("build_translations"), std::string::npos);
        EXPECT_FALSE(std::string(step.what).empty());
    }

    EXPECT_TRUE(tr::validate_options(0, 0, 0).has_value()) << "--batch 0";
    EXPECT_TRUE(tr::validate_options(150, 0, -1).has_value()) << "--max-batches -1";
    EXPECT_TRUE(tr::validate_options(150, -1, 0).has_value()) << "--test -1";
    EXPECT_FALSE(tr::validate_options(150, 0, 0).has_value());
}
