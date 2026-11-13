// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Translator unit tests: the speaker state machine, prompt shape, response
// parsing, the guards that decide whether an answer can be applied at all, and
// the option validation that stands in front of the spend.
#include "apps/pipeline_steps.h"
#include "common/coverage.h"
#include "common/util.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"
#include "translate/translate_core.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/json.hpp>
#include <gtest/gtest.h>

namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;
namespace tr = snt::translate;

constexpr const char* kNaoya = "直哉";
constexpr const char* kMisuzu = "心鈴";
constexpr const char* kIdeographicSpace = "　";
constexpr const char* kFullwidthOne = "１";
constexpr const char* kFullwidthStop = "．";
constexpr const char* kSpokenLine = "「あ」";

constexpr int kContextOverflowLines = 25;
constexpr std::size_t kPaidCacheLines = 56103;
constexpr int kLargeBatchSize = 500;
constexpr int kModelOutputCeiling = 128000;
constexpr int kStandardBatchSize = 150;
constexpr int kNegativeBatchSize = -5;
constexpr int kSmokeTestCount = 3;
constexpr int kInvalidMaxBatchesLarge = -999;
constexpr int kProductionScaleLines = 300;
constexpr std::size_t kStandardRequestedCount = 150;
constexpr std::size_t kShortCount = 149;
constexpr std::size_t kOverCount = 151;
constexpr std::size_t kTruncatedCount = 90;
constexpr int kSparseCutIndex = 7;
constexpr std::size_t kSampleBatchSize = 3;
constexpr std::size_t kGateSampleDialogues = 50;
constexpr std::size_t kThinSampleDialogues = 4;
constexpr int kFailedFirstCheck = 1;
constexpr int kFailedBidirectionalCheck = 5;
constexpr int kRefusedGateExitCode = 2;
constexpr std::size_t kExpectedPipelineEntryCount = 3;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view utf8_name)
        : path_(fs::temp_directory_path() / fs::u8path(utf8_name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

bj::object mk(const char* type, const char* text) {
    bj::object o;
    o["type"] = type;
    o["text"] = text;
    return o;
}

// One batch of `n` lines, as translate_batch takes them.
std::vector<std::pair<std::string, std::string>> batch_of(std::size_t n) {
    std::vector<std::pair<std::string, std::string>> out;
    for (std::size_t i = 0; i < n; ++i) {
        out.emplace_back("Naoya", "朝が" + std::to_string(i));
    }
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

// A document shaped like the extraction: one script, `n` dialogue entries,
// each attributed to `speaker` unless it is empty.
bj::object dialogue_document(std::size_t n, const std::string& speaker) {
    bj::array strings;
    for (std::size_t i = 0; i < n; ++i) {
        bj::object e = mk("dialogue", kSpokenLine);
        if (!speaker.empty()) {
            e["speaker"] = speaker;
        }
        strings.push_back(std::move(e));
    }
    bj::object script;
    script["strings"] = std::move(strings);
    bj::object doc;
    doc["script\\a.ast"] = std::move(script);
    return doc;
}

// A document the whole gate accepts: every label of the name table appears as a
// plate and speaks a line, and the largest speaker carries enough lines to cross
// the dossier threshold.
bj::object gate_passing_document() {
    bj::array strings;
    auto speak = [&](const std::string& label) {
        bj::object e = mk("dialogue", kSpokenLine);
        e["speaker"] = label;
        strings.push_back(std::move(e));
    };
    for (const auto& [jp, en] : tr::name_translations_ordered()) {
        strings.push_back(mk("name", jp.c_str()));
        speak(jp);
    }
    for (std::size_t i = 0; i < snt::translate::DOSSIER_LINE_THRESHOLD; ++i) {
        speak(kNaoya);
    }

    bj::object script;
    script["strings"] = std::move(strings);
    bj::object doc;
    doc["script\\a.ast"] = std::move(script);
    return doc;
}

}  // namespace

TEST(Translate, needs_translation_detects_kana_and_kanji) {
    EXPECT_TRUE(tr::needs_translation("あ"));
    EXPECT_TRUE(tr::needs_translation(kNaoya));
    EXPECT_FALSE(tr::needs_translation("plain ascii"));
    EXPECT_FALSE(tr::needs_translation(""));
}

// A dialogue entry inherits the most recent name entry's romanization.
TEST(Translate, state_machine_attributes_dialogue_to_preceding_name) {
    const bj::array entries{
        mk("name", kNaoya),
        mk("dialogue", "「あ」"),
        mk("narrative", "い"),
        mk("dialogue", "「う」"),
    };
    const auto rows = tr::iter_entries_with_speakers(entries);
    ASSERT_EQ(rows.size(), 4U);
    EXPECT_EQ(rows[0].first, "NAME");
    EXPECT_EQ(rows[1].first, "Naoya");
    EXPECT_EQ(rows[2].first, "NARRATION") << "narrative is never attributed";
    EXPECT_EQ(rows[3].first, "Naoya") << "speaker persists until the next name";
}

// A chapter title resets the speaker: the next dialogue must not inherit the
// name from the previous chapter.
TEST(Translate, title_resets_the_speaker) {
    const bj::array entries{
        mk("name", kNaoya),
        mk("dialogue", "「あ」"),
        mk("title", "第一章"),
        mk("dialogue", "「い」"),
    };
    const auto rows = tr::iter_entries_with_speakers(entries);
    ASSERT_EQ(rows.size(), 4U);
    EXPECT_EQ(rows[1].first, "Naoya");
    EXPECT_EQ(rows[2].first, "TITLE");
    EXPECT_EQ(rows[3].first, "NARRATION");
}

TEST(Translate, dialogue_before_any_name_is_narration) {
    const bj::array entries{mk("dialogue", "「あ」")};
    const auto rows = tr::iter_entries_with_speakers(entries);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].first, "NARRATION");
}

// Unknown JP names pass through verbatim rather than collapsing to NARRATION.
TEST(Translate, unknown_name_passes_through) {
    const bj::array entries{mk("name", "未知"), mk("dialogue", "「あ」")};
    const auto rows = tr::iter_entries_with_speakers(entries);
    EXPECT_EQ(rows[1].first, "未知");
}

TEST(Translate, glossary_names_resolve) {
    const bj::array entries{mk("name", kMisuzu), mk("dialogue", "x")};
    EXPECT_EQ(tr::iter_entries_with_speakers(entries)[1].first, "Misuzu");
}

// The extractor saw the script; the state machine only sees the entries it
// produced.  Where the two answer differently the extractor's field wins.
TEST(Translate, the_speaker_field_wins_over_the_state_machine) {
    bj::object fragment = mk("narrative", "円木");
    fragment["speaker"] = kMisuzu;
    const bj::array entries{mk("name", kNaoya), std::move(fragment)};

    tr::SpeakerSource source;
    const auto rows = tr::iter_entries_with_speakers(entries, &source);
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[1].first, "Misuzu");
    EXPECT_EQ(source.from_field, 1U);
    EXPECT_EQ(source.disagreements, 1U);
}

// A field that is present and null says the line is narration.  Falling back
// to the state machine here would file it under whoever last had a plate.
TEST(Translate, a_null_speaker_field_is_narration_not_a_fallback) {
    bj::object line = mk("dialogue", "「あ」");
    line["speaker"] = nullptr;
    const bj::array entries{mk("name", kNaoya), std::move(line)};
    EXPECT_EQ(tr::iter_entries_with_speakers(entries)[1].first, "NARRATION");
}

// A document written before the field existed still resolves.
TEST(Translate, entries_without_the_field_fall_back_to_the_state_machine) {
    const bj::array entries{mk("name", kNaoya), mk("dialogue", "x")};
    tr::SpeakerSource source;
    const auto rows = tr::iter_entries_with_speakers(entries, &source);
    EXPECT_EQ(rows[1].first, "Naoya");
    EXPECT_EQ(source.from_field, 0U);
    EXPECT_EQ(source.disagreements, 0U);
}

// The field carries the Japanese plate, so it goes through the same table the
// state machine uses and an unpinned label still reaches the prompt.
TEST(Translate, an_unpinned_speaker_field_passes_through_verbatim) {
    bj::object line = mk("dialogue", "x");
    line["speaker"] = "未知";
    const bj::array entries{std::move(line)};
    EXPECT_EQ(tr::iter_entries_with_speakers(entries)[0].first, "未知");
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
    EXPECT_EQ(tr::strip_speaker_tags("[Shizuru] the sign said [EXIT]"),
              "the sign said [EXIT]");
}

TEST(Translate, strip_trims_unicode_whitespace) {
    EXPECT_EQ(tr::strip("　Naoya　"), "Naoya");
    EXPECT_EQ(tr::strip("  spaced \t\n"), "spaced");
    // U+200B ZERO WIDTH SPACE is a formatting character, not whitespace --
    // trimming it would change the key.  Must survive.
    EXPECT_EQ(tr::strip("\u200B"), "\u200B");
}

TEST(Translate, user_prompt_shape) {
    const std::string p = tr::build_user_prompt(
        {{"Naoya", "あ"}, {"NARRATION", "い"}},
        {{"Misuzu", "Previous line."}});
    EXPECT_NE(p.find("<previous_context>\n[Misuzu] Previous line.\n</previous_context>"),
              std::string::npos);
    EXPECT_NE(p.find("1. [Naoya] あ\n2. [NARRATION] い\n"),
              std::string::npos);
    EXPECT_NE(p.find("Output EXACTLY 2 numbered translations"), std::string::npos);
}

TEST(Translate, user_prompt_omits_empty_context_block) {
    const std::string p = tr::build_user_prompt({{"NARRATION", "あ"}}, {});
    EXPECT_EQ(p.find("</previous_context>"), std::string::npos);
    EXPECT_EQ(p.rfind("<lines_to_translate>", 0), 0U);
}

TEST(Translate, context_window_is_capped) {
    std::vector<std::pair<std::string, std::string>> ctx;
    for (int i = 0; i < tr::CONTEXT_WINDOW + kContextOverflowLines; ++i) {
        ctx.emplace_back("S", "line" + std::to_string(i));
    }
    const std::string p = tr::build_user_prompt({{"S", "x"}}, ctx);
    EXPECT_EQ(p.find("[S] line0\n"), std::string::npos) << "oldest line leaked in";
    EXPECT_NE(p.find("[S] line25\n"), std::string::npos) << "window start missing";
    EXPECT_NE(p.find("[S] line74\n"), std::string::npos) << "newest line missing";
}

TEST(Translate, parses_numbered_response) {
    auto t = tr::parse_numbered_response("1. First.\n2) Second.\n3: Third.\n");
    ASSERT_EQ(t.size(), 3U);
    EXPECT_EQ(t[1], "First.");
    EXPECT_EQ(t[3], "Third.");
}

TEST(Translate, parses_multiline_continuation) {
    auto t = tr::parse_numbered_response("1. First part\ncontinued here\n2. Second\n");
    ASSERT_EQ(t.size(), 2U);
    EXPECT_EQ(t[1], "First part continued here");
}

TEST(Translate, cache_preserves_insertion_order) {
    tr::Cache c;
    c.set("b", "B");
    c.set("a", "A");
    c.set("b", "B2");
    ASSERT_EQ(c.size(), 2U);
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
    EXPECT_LT(names.size(), tr::name_translations_ordered().size());
}

TEST(Translate, the_system_prompt_carries_the_derived_name_list) {
    const std::string& prompt = tr::system_prompt();
    ASSERT_NE(prompt.find("<character_names>"), std::string::npos);
    for (const auto& [jp, en] : tr::prompt_name_translations()) {
        EXPECT_NE(prompt.find("- " + jp + " = " + en), std::string::npos)
            << jp << " is missing from the rendered name list";
    }
}

// The answer writes a soft break as a literal backslash-n, and the cache holds
// what the game renders, so the two characters become one break before the
// translation is stored.
TEST(Translate, an_escaped_break_in_the_answer_becomes_a_real_break) {
    EXPECT_EQ(tr::unescape_line_breaks("first\\nsecond"), "first\nsecond");
    EXPECT_EQ(tr::unescape_line_breaks("one\\ntwo\\nthree"), "one\ntwo\nthree");
    EXPECT_EQ(tr::unescape_line_breaks("no break here"), "no break here");
    EXPECT_EQ(tr::unescape_line_breaks("first \\n second"), "first\nsecond");
    EXPECT_EQ(tr::unescape_line_breaks(std::string("first\\n") + kIdeographicSpace + "second"),
              std::string("first\n") + kIdeographicSpace + "second");
    EXPECT_EQ(tr::unescape_line_breaks("a\\tb"), "a\\tb");
    EXPECT_EQ(tr::unescape_line_breaks("trailing backslash\\"), "trailing backslash\\");
}

TEST(Translate, a_parsed_answer_reaches_the_cache_with_its_break_intact) {
    const auto t = tr::parse_numbered_response("1. First half\\nsecond half\n2. Plain\n", 2);
    ASSERT_EQ(t.size(), 2U);
    EXPECT_EQ(t.at(1), "First half\nsecond half");
    EXPECT_EQ(t.at(2), "Plain");

    const auto tail = tr::parse_numbered_response("1. Ends here\\n\n", 1);
    ASSERT_EQ(tail.size(), 1U);
    EXPECT_EQ(tail.at(1), "Ends here");
}

// The guard refuses flags that would throw away a paid cache until
// --discard-cache confirms the intent.
TEST(Translate, a_flag_that_deletes_a_paid_cache_is_refused_until_it_is_meant) {
    EXPECT_FALSE(tr::refuse_cache_discard(0, "--test", false).has_value());
    EXPECT_FALSE(tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD, "--test",
                                          false)
                     .has_value());

    for (const char* flag : {"--test", "--retranslate", "--clean"}) {
        const auto why = tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD + 1, flag,
                                                  false);
        ASSERT_TRUE(why.has_value()) << flag;
        EXPECT_NE(why->find(flag), std::string::npos) << *why;
        EXPECT_NE(why->find("--max-batches"), std::string::npos) << *why;
        EXPECT_NE(why->find("--discard-cache"), std::string::npos) << *why;
    }

    const auto why = tr::refuse_cache_discard(kPaidCacheLines, "--test", false);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("56103"), std::string::npos) << *why;

    EXPECT_FALSE(tr::refuse_cache_discard(kPaidCacheLines, "--test", true).has_value());
}

// An echo -- the model handing back the line it was given -- is a failure only
// when the line is Japanese.  Pure ASCII identity is a valid answer.
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
// was, key order included.
TEST(Translate, purge_removes_failed_entries_and_keeps_the_rest) {
    tr::Cache cache;
    cache.set("朝が来た。", "Morning came.");
    cache.set("こんにちは", "こんにちは");
    cache.set("OK", "OK");
    cache.set("Eスbg152n", "Eスbg152n");
    cache.set("bg152n", "bg152n");

    EXPECT_EQ(tr::purge_failed_entries(cache), 2U);
    EXPECT_EQ(cache.size(), 3U);
    EXPECT_FALSE(cache.contains("こんにちは"));
    EXPECT_FALSE(cache.contains("Eスbg152n"));
    ASSERT_TRUE(cache.get("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    EXPECT_TRUE(cache.contains("OK"));
    EXPECT_TRUE(cache.contains("bg152n"));

    const auto& items = cache.items();
    ASSERT_EQ(items.size(), 3U);
    EXPECT_EQ(items[0].first, "朝が来た。");
    EXPECT_EQ(items[1].first, "OK");
    EXPECT_EQ(items[2].first, "bg152n");

    EXPECT_EQ(tr::purge_failed_entries(cache), 0U);
    EXPECT_EQ(cache.size(), 3U);
}

// A batch that fails leaves its lines out of the cache, so the next run asks
// for exactly those lines again.
TEST(Translate, an_answer_that_never_arrived_is_not_cached) {
    tr::Cache cache;
    std::vector<std::pair<std::string, std::string>> context;

    tr::apply_line_result(cache, context, "Naoya", "「おはよう」", "");
    EXPECT_FALSE(cache.contains("「おはよう」"));
    EXPECT_EQ(cache.size(), 0U);
    ASSERT_EQ(context.size(), 1U);
    EXPECT_EQ(context.front().second, "「おはよう」");

    tr::apply_line_result(cache, context, "Rin", "朝が来た。", "Morning came.");
    ASSERT_TRUE(cache.contains("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    EXPECT_EQ(context.back().second, "Morning came.");
}

// A request asking for more output than the model can emit is rejected
// outright, so a batch big enough to compute past the ceiling has to clamp.
TEST(Translate, max_tokens_stays_inside_the_model_ceiling) {
    EXPECT_EQ(tr::compute_max_tokens(kLargeBatchSize), tr::MAX_OUTPUT_TOKENS);
    EXPECT_LE(tr::compute_max_tokens(kLargeBatchSize), kModelOutputCeiling);
    EXPECT_EQ(tr::compute_max_tokens(kStandardBatchSize),
              kStandardBatchSize * tr::OUTPUT_TOKENS_PER_LINE);
    EXPECT_EQ(tr::compute_max_tokens(1), tr::MIN_OUTPUT_TOKENS);
    EXPECT_EQ(tr::compute_max_tokens(0), tr::MIN_OUTPUT_TOKENS);
    EXPECT_EQ(tr::compute_max_tokens(kNegativeBatchSize), tr::MIN_OUTPUT_TOKENS);
    for (int batch : {1, 50, 150, 426, 427, 1000, 100000}) {
        EXPECT_LE(tr::compute_max_tokens(batch), tr::MAX_OUTPUT_TOKENS) << batch;
    }
}

// The batch cap is a spend limit, so every value that is not a limit has to be
// refused.
TEST(Translate, option_validation_refuses_values_that_would_overspend) {
    EXPECT_FALSE(tr::validate_options(kStandardBatchSize, 0, 0));
    EXPECT_FALSE(tr::validate_options(kStandardBatchSize, 0, 1));
    EXPECT_FALSE(tr::validate_options(1, kSmokeTestCount, 0));

    EXPECT_TRUE(tr::validate_options(kStandardBatchSize, 0, -1));
    EXPECT_TRUE(tr::validate_options(kStandardBatchSize, 0, kInvalidMaxBatchesLarge));
    EXPECT_TRUE(tr::validate_options(kStandardBatchSize, -1, 0));
    EXPECT_TRUE(tr::validate_options(0, 0, 0));
    EXPECT_TRUE(tr::validate_options(kNegativeBatchSize, 0, 0));

    EXPECT_NE(tr::validate_options(kStandardBatchSize, 0, -1)->find("--max-batches"),
              std::string::npos);
    EXPECT_NE(tr::validate_options(0, 0, 0)->find("--batch"), std::string::npos);
}

// The banner must describe the request that is actually built.
TEST(Translate, reasoning_banner_matches_the_transmitted_field) {
    EXPECT_EQ(tr::reasoning_control(tr::MODEL),
              tr::ReasoningControl::AdaptiveThinking);

    const std::string label = tr::reasoning_control_label(tr::MODEL);
    const bool mentions_effort = label.find("effort") != std::string::npos;
    EXPECT_EQ(mentions_effort,
              tr::reasoning_control(tr::MODEL) == tr::ReasoningControl::Effort);
}

// A backward-pointing number on a continuation line would silently replace an
// already-parsed translation.  It is read as prose instead.
TEST(Translate, parse_numbered_response_keeps_a_backward_number_as_prose) {
    const std::string resp =
        "1. He came back at dawn.\n"
        "2. \"It has been a while,\" she said.\n"
        "3. The war ended without a sound.\n"
        "2 days later, the rain came.\n"
        "4. Nothing was left of the garden.\n";
    const auto t = tr::parse_numbered_response(resp, 4);
    ASSERT_EQ(t.size(), 4U);
    EXPECT_EQ(t.at(1), "He came back at dawn.");
    EXPECT_EQ(t.at(2), "\"It has been a while,\" she said.")
        << "an already-parsed translation was overwritten";
    EXPECT_EQ(t.at(3), "The war ended without a sound. 2 days later, the rain came.");
    EXPECT_EQ(t.at(4), "Nothing was left of the garden.");

    EXPECT_EQ(tr::parse_numbered_response(resp).at(2),
              "\"It has been a while,\" she said.");
}

// A forward-pointing number would file text under a line nobody asked for.
TEST(Translate, parse_numbered_response_keeps_a_forward_number_as_prose) {
    const auto t = tr::parse_numbered_response(
        "1. He never spoke of the painting again.\n"
        "1985 came and went, and still he said nothing.\n"
        "2. The gallery closed that spring.\n",
        2);
    ASSERT_EQ(t.size(), 2U);
    EXPECT_EQ(t.at(1),
              "He never spoke of the painting again. 1985 came and went, and still "
              "he said nothing.");
    EXPECT_EQ(t.at(2), "The gallery closed that spring.");
    EXPECT_EQ(t.count(1985), 0U) << "a line number nobody asked for was invented";

    const auto huge = tr::parse_numbered_response("1. Priced at\n999999999999 yen.\n", 1);
    ASSERT_EQ(huge.size(), 1U);
    EXPECT_EQ(huge.at(1), "Priced at 999999999999 yen.");
}

// A year written with a separator reads as a line number nobody asked for, so
// the numbering cannot be trusted and the batch is discarded whole.
TEST(Translate, parse_numbered_response_rejects_a_year_that_reads_as_a_line_number) {
    int rejected = -1;
    const auto t = tr::parse_numbered_response(
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
    const auto t =
        tr::parse_numbered_response("1. First line\n2) Second line\n3: Third line\n", 3);
    ASSERT_EQ(t.size(), 3U);
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
        const auto t = tr::parse_numbered_response(c.body, c.expected, &rejected);
        EXPECT_TRUE(t.empty()) << c.what << ": kept " << t.size()
                               << " entries instead of re-queuing the batch";
        EXPECT_GT(rejected, 0) << c.what << ": the unusable line was not counted";
    }

    EXPECT_TRUE(tr::parse_numbered_response("1. A\n3. C\n2. B\n").empty());
}

// A clean answer reports nothing rejected, so the count separates "the model
// answered short" from "the answer was thrown away".
TEST(Translate, parse_numbered_response_reports_the_rejected_line_count) {
    int rejected = -1;
    const auto good = tr::parse_numbered_response("1. A\n2. B\n", 2, &rejected);
    EXPECT_EQ(good.size(), 2U);
    EXPECT_EQ(rejected, 0);

    rejected = -1;
    const auto bad = tr::parse_numbered_response("1. A\n2. B\n1. C\n1. D\n", 4, &rejected);
    EXPECT_TRUE(bad.empty());
    EXPECT_EQ(rejected, 2);

    EXPECT_EQ(tr::parse_numbered_response("1. A\n", 1).size(), 1U);
    EXPECT_EQ(tr::parse_numbered_response("1. A\n").size(), 1U);
}

// Every numbering shape the model writes has to be recognised as an opener.
TEST(Translate, parse_numbered_response_reads_every_numbering_the_model_writes) {
    struct Case {
        const char* what;
        std::string body;
    };
    const std::vector<Case> cases = {
        {"one blank", "1. Alpha"},
        {"two blanks", "1.  Alpha"},
        {"no blank at all", "1.Alpha"},
        {"fullwidth space for the blank", std::string("1.") + kIdeographicSpace + "Alpha"},
        {"blank before the separator", "1 . Alpha"},
        {"paren separator", "1) Alpha"},
        {"colon separator", "1: Alpha"},
        {"fullwidth digit", std::string(kFullwidthOne) + ". Alpha"},
        {"fullwidth separator", std::string("1") + kFullwidthStop + " Alpha"},
        {"markdown emphasis", "**1.** Alpha"},
        {"quote marker", "> 1. Alpha"},
        {"ascii indent", "   1. Alpha"},
        {"fullwidth indent", std::string(kIdeographicSpace) + "1. Alpha"},
    };
    for (const auto& c : cases) {
        int rejected = -1;
        const auto t = tr::parse_numbered_response(c.body + "\n", 1, &rejected);
        ASSERT_EQ(t.size(), 1U) << c.what;
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
        const auto t = tr::parse_numbered_response(with_third_line(prose), 4);
        ASSERT_EQ(t.size(), 4U) << prose;
        EXPECT_EQ(t.at(2), std::string("The house was empty. ") + prose) << prose;
        EXPECT_EQ(t.at(3), "She never wrote back.")
            << "the real entry 3 was swallowed by: " << prose;
    }

    const auto sub = tr::parse_numbered_response(
        "1. The list was short.\n"
        "1.1 sub point, he wrote in the margin.\n"
        "2. Nothing else was on the page.\n",
        2);
    ASSERT_EQ(sub.size(), 2U);
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
        const auto t = tr::parse_numbered_response(c.body, 3, &rejected);
        EXPECT_TRUE(t.empty()) << c.what << ": folded into the entry before it";
        EXPECT_GT(rejected, 0) << c.what << ": no signal at all";
    }

    int rejected = -1;
    const auto prose = tr::parse_numbered_response(
        "1. He waited.\n1985-1990 was the whole of it.\n2. Then he left.\n", 2, &rejected);
    ASSERT_EQ(prose.size(), 2U);
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
        const auto t = tr::parse_numbered_response(std::string(c.body) + "\n", 1, &rejected);
        ASSERT_EQ(t.size(), 1U) << c.body;
        EXPECT_EQ(t.at(1), c.want) << c.body;
        EXPECT_EQ(rejected, 0) << c.body;
    }
}

// Japanese numbering marks must be recognised, or the line folds into the
// entry before it.
TEST(Translate, parse_numbered_response_reads_a_japanese_numbering_mark) {
    constexpr const char* kIdeographicStop = "。";
    constexpr const char* kIdeographicComma = "、";
    for (const char* mark : {kIdeographicStop, kIdeographicComma}) {
        int rejected = -1;
        const auto t = tr::parse_numbered_response(
            "1" + std::string(mark) + " Alpha\n2" + mark + " Bravo\n", 2, &rejected);
        ASSERT_EQ(t.size(), 2U) << mark;
        EXPECT_EQ(t.at(1), "Alpha") << mark;
        EXPECT_EQ(t.at(2), "Bravo") << mark;
        EXPECT_EQ(rejected, 0) << mark;
    }
}

// Decoration is skipped to find the number, never removed from the text.
TEST(Translate, parse_numbered_response_leaves_decoration_inside_a_translation_alone) {
    const auto t = tr::parse_numbered_response(
        "1. *softly, from the doorway*\n"
        "> and then nothing at all\n"
        "2. #1 in her year, she said.\n",
        2);
    ASSERT_EQ(t.size(), 2U);
    EXPECT_EQ(t.at(1), "*softly, from the doorway* > and then nothing at all");
    EXPECT_EQ(t.at(2), "#1 in her year, she said.");
}

TEST(Translate, parse_numbered_response_reads_crlf_line_endings) {
    const auto t = tr::parse_numbered_response("1. Alpha\r\n2. Bravo\r\n", 2);
    ASSERT_EQ(t.size(), 2U);
    EXPECT_EQ(t.at(1), "Alpha");
    EXPECT_EQ(t.at(2), "Bravo");
}

// Anything before the first number belongs to no entry, so it is dropped.
TEST(Translate, parse_numbered_response_drops_anything_before_the_first_entry) {
    const auto prose = tr::parse_numbered_response(
        "Here are the translations:\n\n1. Alpha\n2. Bravo\n", 2);
    ASSERT_EQ(prose.size(), 2U);
    EXPECT_EQ(prose.at(1), "Alpha");

    const auto numbered = tr::parse_numbered_response(
        "2 lines follow, translated below\n1. Alpha\n2. Bravo\n", 2);
    ASSERT_EQ(numbered.size(), 2U);
    EXPECT_EQ(numbered.at(2), "Bravo");

    int rejected = -1;
    EXPECT_TRUE(tr::parse_numbered_response("", 2, &rejected).empty());
    EXPECT_EQ(rejected, 0);
    EXPECT_TRUE(tr::parse_numbered_response("  \n\t\n \r\n", 2, &rejected).empty());
    EXPECT_EQ(rejected, 0);
}

// A remark after the last entry is reported, not rejected.
TEST(Translate, parse_numbered_response_reports_text_hanging_off_the_last_entry) {
    std::string tail = "unset";
    const auto clean = tr::parse_numbered_response("1. Alpha\n2. Bravo\n", 2, nullptr, &tail);
    ASSERT_EQ(clean.size(), 2U);
    EXPECT_TRUE(tail.empty()) << "a single-line last entry reported a tail";

    const auto noted = tr::parse_numbered_response(
        "1. Alpha\n2. Bravo\nNote: I kept the ruby base words only.\n", 2, nullptr, &tail);
    ASSERT_EQ(noted.size(), 2U) << "the count still matches, so nothing else warns";
    EXPECT_EQ(noted.at(2), "Bravo Note: I kept the ruby base words only.");
    EXPECT_EQ(tail, "Note: I kept the ruby base words only.");

    const auto wrapped = tr::parse_numbered_response(
        "1. Alpha\nand the rest of it\n2. Bravo\n", 2, nullptr, &tail);
    ASSERT_EQ(wrapped.size(), 2U);
    EXPECT_EQ(wrapped.at(1), "Alpha and the rest of it");
    EXPECT_TRUE(tail.empty());
}

// A skipped number leaves a gap the caller re-queues, and production batch size
// holds cleanly.
TEST(Translate, parse_numbered_response_holds_over_a_production_batch) {
    const auto gapped = tr::parse_numbered_response(
        "1. The first line\nruns on for a second physical line\n"
        "2. Second line\n4. Fourth line -- the third never came back\n",
        4);
    ASSERT_EQ(gapped.size(), 3U);
    EXPECT_EQ(gapped.at(1), "The first line runs on for a second physical line");
    EXPECT_EQ(gapped.count(3), 0U);

    std::string body;
    for (int i = 1; i <= kProductionScaleLines; ++i) {
        body += std::to_string(i) + ". Line " + std::to_string(i) + "\n";
    }

    int rejected = -1;
    std::string tail = "unset";
    const auto t = tr::parse_numbered_response(body, kProductionScaleLines, &rejected, &tail);
    ASSERT_EQ(t.size(), static_cast<std::size_t>(kProductionScaleLines));
    EXPECT_EQ(t.at(1), "Line 1");
    EXPECT_EQ(t.at(kProductionScaleLines), "Line 300");
    EXPECT_EQ(rejected, 0);
    EXPECT_TRUE(tail.empty());

    const auto strayed = tr::parse_numbered_response(
        body + "That's all of them.\n", kProductionScaleLines, &rejected, &tail);
    ASSERT_EQ(strayed.size(), static_cast<std::size_t>(kProductionScaleLines));
    EXPECT_EQ(strayed.at(kProductionScaleLines), "Line 300 That's all of them.");
    EXPECT_EQ(rejected, 0) << "a stray last line is not a numbering fault";
    EXPECT_EQ(tail, "That's all of them.");
}

// Known blind spot, pinned so it stays an explicit decision: a skipped number
// whose continuation opens with exactly that number passes every guard.
TEST(Translate, parse_numbered_response_cannot_tell_a_skipped_number_from_a_real_one) {
    const auto t = tr::parse_numbered_response(
        "1. He came back at dawn.\n"
        "2. The house was empty, and\n"
        "3. days later it was still empty.\n"
        "4. Nothing was left of the garden.\n",
        4);
    ASSERT_EQ(t.size(), 4U);
    EXPECT_EQ(t.at(2), "The house was empty, and") << "the entry lost its tail";
    EXPECT_EQ(t.at(3), "days later it was still empty.") << "and the tail became an entry";
}

// A self-stopped answer with fewer entries than asked for may have merged two
// lines, and nothing says which two, so the count is the only guard there is.
TEST(Translate, a_short_answer_that_stopped_on_its_own_cannot_be_applied) {
    EXPECT_TRUE(tr::answer_count_is_usable(
        kStandardRequestedCount, kStandardRequestedCount, "end_turn"));
    EXPECT_FALSE(tr::answer_count_is_usable(
        kStandardRequestedCount, kShortCount, "end_turn"));
    EXPECT_FALSE(tr::answer_count_is_usable(
        kStandardRequestedCount, kShortCount, ""));
    EXPECT_FALSE(tr::answer_count_is_usable(
        kStandardRequestedCount, 0, "end_turn"));
    EXPECT_FALSE(tr::answer_count_is_usable(
        kStandardRequestedCount, kOverCount, "end_turn"));

    EXPECT_TRUE(tr::answer_count_is_usable(
        kStandardRequestedCount, kTruncatedCount, "max_tokens"));
    EXPECT_TRUE(tr::answer_count_is_usable(
        kStandardRequestedCount, kTruncatedCount, "model_context_window_exceeded"));

    int rejected = -1;
    const auto merged = tr::parse_numbered_response("1. A\n2. B and C together\n3. D\n", 4,
                                                    &rejected);
    ASSERT_EQ(merged.size(), 3U);
    EXPECT_EQ(rejected, 0) << "the numbering is in order and in range";
    EXPECT_FALSE(tr::answer_count_is_usable(4, merged.size(), "end_turn"));
}

// An answer cut at the token limit ends mid-sentence and loses only its last
// entry.
TEST(Translate, truncated_answer_loses_only_its_last_entry) {
    const std::map<int, std::string> full = {
        {1, "He turned and left."},
        {2, "The door stayed open."},
        {3, "He turned and said, \"I have always"},
    };

    auto t = full;
    EXPECT_TRUE(tr::drop_truncated_entry(t, "max_tokens"));
    EXPECT_EQ(t.size(), 2U);
    EXPECT_EQ(t.count(3), 0U) << "the half-written entry was kept";
    EXPECT_EQ(t.at(1), "He turned and left.");

    auto ctx = full;
    EXPECT_TRUE(tr::drop_truncated_entry(ctx, "model_context_window_exceeded"));
    EXPECT_EQ(ctx.size(), 2U);

    for (const char* reason : {"end_turn", "stop_sequence", ""}) {
        auto keep = full;
        EXPECT_FALSE(tr::drop_truncated_entry(keep, reason)) << reason;
        EXPECT_EQ(keep.size(), 3U) << reason;
    }

    std::map<int, std::string> sparse = {
        {1, "One."}, {2, "Two."}, {kSparseCutIndex, "Seven and a ha"}};
    EXPECT_TRUE(tr::drop_truncated_entry(sparse, "max_tokens"));
    EXPECT_EQ(sparse.count(kSparseCutIndex), 0U);
    EXPECT_EQ(sparse.size(), 2U);

    std::map<int, std::string> none;
    EXPECT_FALSE(tr::drop_truncated_entry(none, "max_tokens"));
}

// A refusal discards the whole batch.
TEST(Translate, a_refused_answer_is_discarded_whole) {
    const std::map<int, std::string> full = {
        {1, "He turned and left."},
        {2, "The door stayed open."},
        {3, "He turned and said, \"I have always"},
    };

    auto refused = full;
    EXPECT_TRUE(tr::discard_refused_answer(refused, "refusal"));
    EXPECT_TRUE(refused.empty()) << "entries survived a refusal and would be cached";

    auto both = full;
    ASSERT_TRUE(tr::discard_refused_answer(both, "refusal"));
    EXPECT_FALSE(tr::drop_truncated_entry(both, "refusal"));
    EXPECT_TRUE(both.empty());

    for (const char* reason : {"end_turn", "max_tokens", "stop_sequence",
                               "model_context_window_exceeded", ""}) {
        auto keep = full;
        EXPECT_FALSE(tr::discard_refused_answer(keep, reason)) << reason;
        EXPECT_EQ(keep.size(), 3U) << reason;
    }

    std::map<int, std::string> none;
    EXPECT_TRUE(tr::discard_refused_answer(none, "refusal"));
}

// With adaptive thinking the first content block is thinking and carries no
// text, so the reader has to walk past it to find the translations.
TEST(Translate, the_answer_is_read_past_the_thinking_block) {
    const tr::Answer answer = tr::read_answer(api_response("1. Alpha\n", "end_turn"));
    EXPECT_EQ(answer.text, "1. Alpha");
    EXPECT_EQ(answer.stop_reason, "end_turn");

    const tr::Answer empty = tr::read_answer(bj::value(bj::object{}));
    EXPECT_TRUE(empty.text.empty());
    EXPECT_TRUE(empty.stop_reason.empty());
}

// The three guards applied to a whole answer, in the order the caller uses.
TEST(Translate, an_answer_that_cannot_be_lined_up_yields_nothing) {
    EXPECT_TRUE(tr::usable_translations(
                    tr::read_answer(api_response("1. Alpha\n2. Bravo\n", "end_turn")), 3)
                    .empty());

    const auto cut = tr::usable_translations(
        tr::read_answer(api_response("1. Alpha\n2. Bravo\n3. Charlie and th", "max_tokens")),
        kStandardRequestedCount);
    ASSERT_EQ(cut.size(), 2U);
    EXPECT_EQ(cut.at(2), "Bravo");
    EXPECT_EQ(cut.count(3), 0U);

    EXPECT_TRUE(tr::usable_translations(
                    tr::read_answer(api_response("1. Alpha\n2. Bravo\n", "refusal")), 2)
                    .empty());

    const auto whole = tr::usable_translations(
        tr::read_answer(api_response("1. Alpha\n2. Bravo\n", "end_turn")), 2);
    ASSERT_EQ(whole.size(), 2U);
    EXPECT_EQ(whole.at(1), "Alpha");
}

// A client that fails leaves the cache untouched, so the next run asks for the
// same lines again.
TEST(Translate, a_batch_whose_client_fails_leaves_the_cache_untouched) {
    const auto batch = batch_of(kSampleBatchSize);
    for (const bool rate_limited : {false, true}) {
        tr::Cache cache;
        cache.set("朝が来た。", "Morning came.");
        std::vector<std::pair<std::string, std::string>> context;

        tr::translate_batch(cache, context, batch,
                            [&]() -> std::map<int, std::string> {
                                if (rate_limited) {
                                    throw snt::gemini::RateLimitError("429");
                                }
                                throw snt::gemini::ApiError("500");
                            },
                            std::chrono::seconds(0));

        EXPECT_EQ(cache.size(), 1U) << "rate_limited=" << rate_limited;
        for (const auto& [speaker, jp] : batch) {
            EXPECT_FALSE(cache.contains(jp)) << jp;
        }
        ASSERT_EQ(context.size(), batch.size());
        EXPECT_EQ(context.front().second, batch.front().second);
    }
}

// An answer one line short of the batch is discarded before it reaches the
// cache, because nothing says which line the numbering slipped on.
TEST(Translate, an_answer_short_of_the_batch_caches_nothing) {
    const auto batch = batch_of(kSampleBatchSize);
    tr::Cache cache;
    std::vector<std::pair<std::string, std::string>> context;

    tr::translate_batch(cache, context, batch, [&] {
        return tr::usable_translations(
            tr::read_answer(api_response("1. Alpha\n2. Bravo\n", "end_turn")), batch.size());
    });

    EXPECT_EQ(cache.size(), 0U) << "a batch that cannot be lined up was cached anyway";
    ASSERT_EQ(context.size(), batch.size());

    tr::Cache good;
    std::vector<std::pair<std::string, std::string>> good_context;
    tr::translate_batch(good, good_context, batch, [&] {
        return tr::usable_translations(
            tr::read_answer(api_response("1. Alpha\n2. Bravo\n3. Charlie\n", "end_turn")),
            batch.size());
    });
    ASSERT_EQ(good.size(), kSampleBatchSize);
    ASSERT_TRUE(good.get(batch[2].second));
    EXPECT_EQ(*good.get(batch[2].second), "Charlie");
}

// A batch answered in part caches the lines that came back and leaves the rest
// queued, rather than filling the gap with the Japanese.
TEST(Translate, a_gap_in_the_answer_leaves_that_one_line_queued) {
    const auto batch = batch_of(kSampleBatchSize);
    tr::Cache cache;
    std::vector<std::pair<std::string, std::string>> context;

    tr::translate_batch(cache, context, batch, [&] {
        return tr::usable_translations(
            tr::read_answer(api_response("1. Alpha\n3. Charlie\n", "max_tokens")),
            batch.size());
    });

    ASSERT_EQ(cache.size(), 1U);
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

// The regression the gate exists for: the walker stops finding the plate,
// every dialogue line reads [NARRATION], and the answer comes back fluent and
// wrong in every voice.
TEST(Translate, the_speaker_gate_refuses_a_document_that_lost_its_speakers) {
    const tr::SpeakerCheckReport lost =
        tr::run_speaker_checks(dialogue_document(kGateSampleDialogues, ""));
    EXPECT_FALSE(lost.passed);
    EXPECT_EQ(lost.failed_check, kFailedFirstCheck);
    EXPECT_NE(lost.reason.find("NARRATION"), std::string::npos);

    const tr::SpeakerCheckReport thin =
        tr::run_speaker_checks(dialogue_document(kThinSampleDialogues, kNaoya));
    EXPECT_FALSE(thin.passed);
    EXPECT_EQ(thin.failed_check, kFailedFirstCheck);

    const tr::SpeakerCheckReport partial =
        tr::run_speaker_checks(dialogue_document(kGateSampleDialogues, kNaoya));
    EXPECT_FALSE(partial.passed);
    EXPECT_EQ(partial.failed_check, kFailedBidirectionalCheck);
}

// A refused gate stops the run before a key is read, before a client is built
// and before the fresh-run delete, so it costs nothing and loses nothing.
TEST(Translate, a_failed_gate_stops_translation_before_it_spends) {
    ScopedTempDir temp("snt_ut_gate");
    const fs::path& dir = temp.path();

    tr::TranslateOptions opt;
    opt.input_file = (dir / fs::u8path("extracted_text.json")).u8string();
    opt.cache_file = (dir / fs::u8path("translation_cache_gemini.json")).u8string();
    opt.output_file = (dir / fs::u8path("translated_text.json")).u8string();
    opt.tsv_file = (dir / fs::u8path("translations.tsv")).u8string();
    tr::Cache seeded;
    seeded.set("朝が来た。", "Morning came.");
    tr::save_cache(seeded, opt.cache_file);
    opt.fresh_run = true;

    snt::write_file(
        opt.input_file,
        snt::json_dump(bj::value(dialogue_document(kGateSampleDialogues, ""))));

    EXPECT_EQ(tr::translate_all(opt), kRefusedGateExitCode)
        << "a refused gate has to abort before spending";
    EXPECT_FALSE(fs::exists(fs::u8path(opt.output_file)));
    EXPECT_FALSE(fs::exists(fs::u8path(opt.tsv_file)));
    EXPECT_TRUE(fs::exists(fs::u8path(opt.cache_file)))
        << "the run stopped before it could drop work already paid for";
    EXPECT_EQ(tr::load_cache(opt.cache_file).size(), 1U);
}

// --gate-only answers over a document the gate accepts, then stops: it writes
// no document and no table.
TEST(Translate, gate_only_answers_a_passing_gate_and_writes_nothing) {
    ScopedTempDir temp("snt_ut_gate_only");
    const fs::path& dir = temp.path();

    tr::TranslateOptions opt;
    opt.input_file = (dir / fs::u8path("extracted_text.json")).u8string();
    opt.cache_file = (dir / fs::u8path("translation_cache_gemini.json")).u8string();
    opt.output_file = (dir / fs::u8path("translated_text.json")).u8string();
    opt.tsv_file = (dir / fs::u8path("translations.tsv")).u8string();
    opt.gate_only = true;

    tr::Cache seeded;
    seeded.set(kSpokenLine, "\"Ah.\"");
    tr::save_cache(seeded, opt.cache_file);
    snt::write_file(opt.input_file, snt::json_dump(bj::value(gate_passing_document())));

    EXPECT_EQ(tr::translate_all(opt), 0) << "the gate accepted the document";
    EXPECT_FALSE(fs::exists(fs::u8path(opt.output_file)))
        << "--gate-only wrote the document it was asked only to check";
    EXPECT_FALSE(fs::exists(fs::u8path(opt.tsv_file)));
    EXPECT_FALSE(fs::exists(fs::u8path(snt::coverage_report_path(opt.output_file))));
    ASSERT_TRUE(fs::exists(fs::u8path(opt.cache_file)));
    EXPECT_EQ(tr::load_cache(opt.cache_file).size(), 1U);
}

// Three pipeline entries in order: the speaker gate and the table build are
// folded into 02_translate so they cannot be skipped.
TEST(Translate, the_pipeline_runs_three_entries_in_order) {
    const auto& entries = snt::apps::pipeline_steps();
    ASSERT_EQ(entries.size(), kExpectedPipelineEntryCount);
    EXPECT_STREQ(entries[0].name, "01_extract");
    EXPECT_STREQ(entries[1].name, "02_translate");
    EXPECT_STREQ(entries[2].name, "deploy");
    for (const auto& entry : entries) {
        EXPECT_EQ(std::string(entry.name).find("speaker_gate"), std::string::npos);
        EXPECT_EQ(std::string(entry.name).find("build_translations"), std::string::npos);
        EXPECT_FALSE(std::string(entry.what).empty());
    }

    EXPECT_TRUE(tr::validate_options(0, 0, 0).has_value()) << "--batch 0";
    EXPECT_TRUE(tr::validate_options(kStandardBatchSize, 0, -1).has_value()) << "--max-batches -1";
    EXPECT_TRUE(tr::validate_options(kStandardBatchSize, -1, 0).has_value()) << "--test -1";
    EXPECT_FALSE(tr::validate_options(kStandardBatchSize, 0, 0).has_value());
}
