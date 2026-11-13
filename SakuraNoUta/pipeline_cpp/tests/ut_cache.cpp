// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Translation-core unit tests: glossary, speaker extraction, prompt assembly,
// cache discard guards, and numbered response parsing on synthetic inputs.
#include "apps/pipeline_steps.h"
#include "cache/cache_core.h"
#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/coverage.h"
#include "common/util.h"

#include <boost/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace tr = snu::translate;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

const char* const U3000 = "　";
const char* const FULLWIDTH_ONE = "１";
const char* const FULLWIDTH_STOP = "．";

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

bj::object dialogue_document(std::size_t n, const std::string& speaker) {
    bj::array strings;
    for (std::size_t i = 0; i < n; ++i) {
        bj::object e;
        e["type"] = "dialogue";
        e["text"] = "「おはよう」";
        if (!speaker.empty()) {
            e["speaker"] = speaker;
        }
        strings.push_back(std::move(e));
    }
    bj::object script;
    script["strings"] = std::move(strings);
    bj::object doc;
    doc["00_test_script"] = std::move(script);
    return doc;
}

}  // namespace

TEST(Translate, needs_translation_filters_correctly) {
    EXPECT_TRUE(tr::needs_translation("こんにちは"));
    EXPECT_TRUE(tr::needs_translation("カタカナ"));
    EXPECT_TRUE(tr::needs_translation("漢字"));
    EXPECT_FALSE(tr::needs_translation("Hello"));
    EXPECT_FALSE(tr::needs_translation(""));
    EXPECT_FALSE(tr::needs_translation("   "));
    EXPECT_FALSE(tr::needs_translation("……"));
    EXPECT_FALSE(tr::needs_translation("ABC123"));
}

TEST(Translate, translation_cache_round_trips) {
    const ScopedTempDir scratch("snu_ut_cache_rt");
    const std::string path =
        (scratch.path() / fs::u8path("cache.json")).u8string();
    tr::Cache cache;
    cache.set("こんにちは", "Hello");
    cache.set("太一", "Taichi");
    tr::save_cache(cache, path);
    const tr::Cache loaded = tr::load_cache(path);
    ASSERT_EQ(loaded.size(), 2u);
    ASSERT_TRUE(loaded.get("こんにちは"));
    EXPECT_EQ(*loaded.get("こんにちは"), "Hello");
    ASSERT_TRUE(loaded.get("太一"));
    EXPECT_EQ(*loaded.get("太一"), "Taichi");
    EXPECT_FALSE(fs::exists(fs::u8path(path + ".tmp")));
}

TEST(Translate, extract_speaker_mapping) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"直哉", "Naoya"}, {"稟", "Rin"}, {"真琴", "Makoto"},
        {"里奈", "Rina"}, {"優美", "Yumi"}};
    for (const auto& [jp, en] : cases) {
        bj::object entry;
        entry["speaker"] = jp;
        entry["text"] = "「dummy」";
        entry["type"] = "dialogue";
        EXPECT_EQ(tr::extract_speaker(entry), en);
    }

    bj::object name_entry;
    name_entry["type"] = "name";
    name_entry["text"] = "直哉";
    EXPECT_EQ(tr::extract_speaker(name_entry), "NAME");

    bj::object narration;
    narration["type"] = "narrative";
    narration["speaker"] = nullptr;
    EXPECT_EQ(tr::extract_speaker(narration), "NARRATION");

    bj::object unknown;
    unknown["type"] = "dialogue";
    unknown["speaker"] = "誰か";
    EXPECT_EQ(tr::extract_speaker(unknown), "誰か");
}

TEST(Translate, strip_speaker_tags_removes_leaked_tags) {
    EXPECT_EQ(tr::strip_speaker_tags("[Naoya] Hello there"), "Hello there");
    EXPECT_EQ(tr::strip_speaker_tags("[NARRATION] The night fell."),
              "The night fell.");
    EXPECT_EQ(tr::strip_speaker_tags("No tags here"), "No tags here");
}

TEST(Translate, strip_speaker_tags_leaves_brackets_inside_the_line) {
    EXPECT_EQ(tr::strip_speaker_tags("He read the sign [EXIT] and stopped."),
              "He read the sign [EXIT] and stopped.");
    EXPECT_EQ(tr::strip_speaker_tags("\"Wait,\" he said [NARRATION] later."),
              "\"Wait,\" he said [NARRATION] later.");
    EXPECT_EQ(tr::strip_speaker_tags("[Rin] the sign said [EXIT]"),
              "the sign said [EXIT]");
}

TEST(Translate, option_validation_refuses_values_that_would_overspend) {
    EXPECT_FALSE(tr::validate_options(150, 0, 0));
    EXPECT_FALSE(tr::validate_options(150, 0, 1));
    EXPECT_FALSE(tr::validate_options(1, 3, 0));

    EXPECT_TRUE(tr::validate_options(150, 0, -1));
    EXPECT_TRUE(tr::validate_options(150, 0, -999));
    EXPECT_TRUE(tr::validate_options(150, -1, 0));
    EXPECT_TRUE(tr::validate_options(0, 0, 0));
    EXPECT_TRUE(tr::validate_options(-5, 0, 0));

    EXPECT_NE(tr::validate_options(150, 0, -1)->find("--max-batches"),
              std::string::npos);
    EXPECT_NE(tr::validate_options(0, 0, 0)->find("--batch"), std::string::npos);
}

TEST(Translate, exempt_predicate_fires_on_symbols_only) {
    EXPECT_TRUE(snu::is_exempt_line("　・・・"));
    EXPECT_TRUE(snu::is_exempt_line("　・・・・・・。"));
    EXPECT_TRUE(snu::is_exempt_line("　「・・・」"));
    EXPECT_TRUE(snu::is_exempt_line("？？？"));
    EXPECT_TRUE(snu::is_exempt_line("……"));
    EXPECT_TRUE(snu::is_exempt_line("「……」"));
    EXPECT_TRUE(snu::is_exempt_line("――"));

    EXPECT_FALSE(snu::is_exempt_line(""));
    EXPECT_FALSE(snu::is_exempt_line("こんにちは"));
    EXPECT_FALSE(snu::is_exempt_line("桜"));
    EXPECT_FALSE(snu::is_exempt_line("「あ……」"));
    EXPECT_FALSE(snu::is_exempt_line("OK"));
    EXPECT_FALSE(snu::is_exempt_line("Ａ"));
    EXPECT_FALSE(snu::is_exempt_line("１２３"));
    EXPECT_FALSE(snu::is_exempt_line("ア"));

    EXPECT_TRUE(tr::is_failed_entry("……", "……"));
}

TEST(Translate, parse_numbered_response_variants) {
    std::string resp =
        "1. First line\n"
        "2) Second line\n"
        "3: Third line\n"
        "4. Wrapped line\n"
        "continues here\n";
    auto t = tr::parse_numbered_response(resp);
    ASSERT_EQ(t.size(), 4u);
    EXPECT_EQ(t.at(1), "First line");
    EXPECT_EQ(t.at(2), "Second line");
    EXPECT_EQ(t.at(3), "Third line");
    EXPECT_EQ(t.at(4), "Wrapped line continues here");
}

TEST(Translate, parse_numbered_response_strips_echoed_tags) {
    auto t = tr::parse_numbered_response("1. [Naoya] \"Morning.\"\n");
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t.at(1), "\"Morning.\"");
}

TEST(Translate, parse_numbered_response_keeps_a_backward_number_as_prose) {
    std::string resp =
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

    EXPECT_EQ(tr::parse_numbered_response(resp).at(2),
              "\"It has been a while,\" she said.");
}

TEST(Translate, parse_numbered_response_keeps_a_forward_number_as_prose) {
    std::string resp =
        "1. He never spoke of the painting again.\n"
        "1985 came and went, and still he said nothing.\n"
        "2. The gallery closed that spring.\n";
    auto t = tr::parse_numbered_response(resp, 2);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t.at(1),
              "He never spoke of the painting again. 1985 came and went, and still "
              "he said nothing.");
    EXPECT_EQ(t.at(2), "The gallery closed that spring.");
    EXPECT_EQ(t.count(1985), 0u) << "a line number nobody asked for was invented";

    auto huge = tr::parse_numbered_response("1. Priced at\n999999999999 yen.\n", 1);
    ASSERT_EQ(huge.size(), 1u);
    EXPECT_EQ(huge.at(1), "Priced at 999999999999 yen.");
}

TEST(Translate, parse_numbered_response_still_joins_and_still_allows_gaps) {
    auto t = tr::parse_numbered_response(
        "1. The first line\n"
        "runs on for a second physical line\n"
        "and a third.\n"
        "2. Second line\n"
        "4. Fourth line -- the third never came back\n",
        4);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t.at(1), "The first line runs on for a second physical line and a third.");
    EXPECT_EQ(t.at(2), "Second line");
    EXPECT_EQ(t.count(3), 0u);
    EXPECT_EQ(t.at(4), "Fourth line -- the third never came back");
}

TEST(Translate, parse_numbered_response_rejects_a_prose_opener) {
    auto with_third_line = [](const char* third) {
        return std::string("1. He came back at dawn.\n") +
               "2. The house was empty.\n" + third + "\n" +
               "3. She never wrote back.\n" +
               "4. Nothing was left of the garden.\n";
    };

    for (const char* prose : {"3 days later, the rain stopped.", "3:00 sharp, he left.",
                              "3.5 cm across."}) {
        auto t = tr::parse_numbered_response(with_third_line(prose), 4);
        ASSERT_EQ(t.size(), 4u) << prose;
        EXPECT_EQ(t.at(1), "He came back at dawn.") << prose;
        EXPECT_EQ(t.at(2), std::string("The house was empty. ") + prose) << prose;
        EXPECT_EQ(t.at(3), "She never wrote back.")
            << "the real entry 3 was swallowed by: " << prose;
        EXPECT_EQ(t.at(4), "Nothing was left of the garden.") << prose;
    }

    auto t = tr::parse_numbered_response(
        "1. He never spoke of it again.\n"
        "1985 came and went.\n"
        "2. The gallery closed that spring.\n",
        2);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t.at(1), "He never spoke of it again. 1985 came and went.");
    EXPECT_EQ(t.at(2), "The gallery closed that spring.");
}

TEST(Translate, parse_numbered_response_accepts_every_requested_separator) {
    auto t = tr::parse_numbered_response(
        "1. First line\n"
        "2) Second line\n"
        "3: Third line\n",
        3);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t.at(1), "First line");
    EXPECT_EQ(t.at(2), "Second line");
    EXPECT_EQ(t.at(3), "Third line") << "the number equal to the count was refused";
}

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

    EXPECT_TRUE(tr::parse_numbered_response("1. A\n3. C\n2. B\n").empty());
}

TEST(Translate, parse_numbered_response_reports_the_rejected_line_count) {
    int rejected = -1;
    auto good = tr::parse_numbered_response("1. A\n2. B\n", 2, &rejected);
    EXPECT_EQ(good.size(), 2u);
    EXPECT_EQ(rejected, 0);

    rejected = -1;
    auto bad = tr::parse_numbered_response("1. A\n2. B\n1. C\n1. D\n", 4, &rejected);
    EXPECT_TRUE(bad.empty());
    EXPECT_EQ(rejected, 2);

    EXPECT_EQ(tr::parse_numbered_response("1. A\n", 1).size(), 1u);
}

TEST(Translate, parse_numbered_response_treats_an_overlong_digit_run_as_prose) {
    int rejected = -1;
    auto t = tr::parse_numbered_response("1. Priced at\n999999999999. yen, he said.\n", 1,
                                         &rejected);
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t.at(1), "Priced at 999999999999. yen, he said.");
    EXPECT_EQ(rejected, 0);
}

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

TEST(Translate, parse_numbered_response_does_not_glue_a_spaceless_opener_to_the_line_before) {
    const std::vector<std::string> openers = {"2.Bravo", std::string("2.") + U3000 + "Bravo"};
    for (const auto& opener : openers) {
        auto t = tr::parse_numbered_response("1. Alpha\n" + opener + "\n3. Charlie\n", 3);
        ASSERT_EQ(t.size(), 3u) << opener;
        EXPECT_EQ(t.at(1), "Alpha") << opener;
        EXPECT_EQ(t.at(2), "Bravo") << opener;
        EXPECT_EQ(t.at(3), "Charlie") << opener;
    }
}

TEST(Translate, parse_numbered_response_keeps_a_sub_point_number_as_prose) {
    auto t = tr::parse_numbered_response(
        "1. The list was short.\n"
        "1.1 sub point, he wrote in the margin.\n"
        "2. Nothing else was on the page.\n",
        2);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t.at(1), "The list was short. 1.1 sub point, he wrote in the margin.");
    EXPECT_EQ(t.at(2), "Nothing else was on the page.");
}

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

TEST(Translate, parse_numbered_response_keeps_a_translation_that_reads_like_markup) {
    auto t = tr::parse_numbered_response(
        "1. He came back at dawn.\n"
        "2. The house was empty.\n"
        "3. Tag was the worst.\n",
        3);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t.at(3), "Tag was the worst.");
}

TEST(Translate, parse_numbered_response_reads_crlf_line_endings) {
    auto t = tr::parse_numbered_response("1. Alpha\r\n2. Bravo\r\n", 2);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t.at(1), "Alpha");
    EXPECT_EQ(t.at(2), "Bravo");
}

TEST(Translate, parse_numbered_response_drops_anything_before_the_first_entry) {
    auto prose = tr::parse_numbered_response(
        "Here are the translations:\n\n1. Alpha\n2. Bravo\n", 2);
    ASSERT_EQ(prose.size(), 2u);
    EXPECT_EQ(prose.at(1), "Alpha");
    EXPECT_EQ(prose.at(2), "Bravo");

    auto numbered = tr::parse_numbered_response(
        "2 lines follow, translated below\n1. Alpha\n2. Bravo\n", 2);
    ASSERT_EQ(numbered.size(), 2u);
    EXPECT_EQ(numbered.at(1), "Alpha");
    EXPECT_EQ(numbered.at(2), "Bravo");

    int rejected = -1;
    EXPECT_TRUE(tr::parse_numbered_response("", 2, &rejected).empty());
    EXPECT_EQ(rejected, 0);
    EXPECT_TRUE(tr::parse_numbered_response("  \n\t\n \r\n", 2, &rejected).empty());
    EXPECT_EQ(rejected, 0);
}

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

    auto wrapped = tr::parse_numbered_response(
        "1. Alpha\nand the rest of it\n2. Bravo\n", 2, nullptr, &tail);
    ASSERT_EQ(wrapped.size(), 2u);
    EXPECT_EQ(wrapped.at(1), "Alpha and the rest of it");
    EXPECT_TRUE(tail.empty());
}

TEST(Translate, parse_numbered_response_holds_over_a_production_batch) {
    const int kLines = 300;
    std::string body;
    for (int i = 1; i <= kLines; ++i) {
        body += std::to_string(i) + ". Line " + std::to_string(i) + "\n";
    }

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

TEST(Translate, parse_numbered_response_without_a_count_still_holds_the_order_rule) {
    EXPECT_TRUE(tr::parse_numbered_response("1. A\n3. C\n2. B\n").empty());

    auto forward = tr::parse_numbered_response("1. A\n9. I\n");
    ASSERT_EQ(forward.size(), 2u) << "with no count, nothing says how far forward is too far";
    EXPECT_EQ(forward.at(9), "I");
}

TEST(Translate, a_line_break_travels_as_an_escape_in_both_directions) {
    EXPECT_EQ(tr::escape_line_breaks("first\nsecond"), "first\\nsecond");
    EXPECT_EQ(tr::escape_line_breaks("first\r\nsecond"), "first\\nsecond");
    EXPECT_EQ(tr::escape_line_breaks("first\rsecond"), "first\\nsecond");
    EXPECT_EQ(tr::escape_line_breaks("one\ntwo\nthree"), "one\\ntwo\\nthree");
    EXPECT_EQ(tr::escape_line_breaks("no break here"), "no break here");

    EXPECT_EQ(tr::unescape_line_breaks("first\\nsecond"), "first\nsecond");
    EXPECT_EQ(tr::unescape_line_breaks(tr::escape_line_breaks("one\ntwo\nthree")),
              "one\ntwo\nthree");
    EXPECT_EQ(tr::unescape_line_breaks("no break here"), "no break here");

    EXPECT_EQ(tr::unescape_line_breaks("first \\n second"), "first\nsecond");
    EXPECT_EQ(tr::unescape_line_breaks(std::string("first\\n") + U3000 + "second"),
              std::string("first\n") + U3000 + "second");
    EXPECT_EQ(tr::unescape_line_breaks("a\\tb"), "a\\tb");
    EXPECT_EQ(tr::unescape_line_breaks("trailing backslash\\"), "trailing backslash\\");
}

TEST(Translate, user_prompt_keeps_every_entry_on_one_physical_line) {
    std::vector<std::pair<std::string, std::string>> batch = {
        {"Naoya", "一行目\n二行目"}, {"NARRATION", "朝が来た。"}};
    std::vector<std::pair<std::string, std::string>> context = {
        {"Rin", "Good night.\nSleep well."}};
    std::string prompt = tr::build_user_prompt(batch, context);

    EXPECT_NE(prompt.find("[Rin] Good night.\\nSleep well.\n"), std::string::npos);
    EXPECT_NE(prompt.find("1. [Naoya] 一行目\\n二行目\n"), std::string::npos);
    EXPECT_NE(prompt.find("2. [NARRATION] 朝が来た。\n"), std::string::npos);

    std::size_t open = prompt.find("<lines_to_translate>\n");
    ASSERT_NE(open, std::string::npos);
    open += std::string("<lines_to_translate>\n").size();
    std::size_t close = prompt.find("</lines_to_translate>", open);
    ASSERT_NE(close, std::string::npos);
    EXPECT_EQ(std::count(prompt.begin() + static_cast<std::ptrdiff_t>(open),
                         prompt.begin() + static_cast<std::ptrdiff_t>(close), '\n'),
              2);
}

TEST(Translate, parse_numbered_response_restores_a_line_break_inside_an_entry) {
    auto t = tr::parse_numbered_response(
        "1. First half\\nsecond half\n"
        "2. One\\ntwo\\nthree\n"
        "3. No break at all\n",
        3);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t.at(1), "First half\nsecond half");
    EXPECT_EQ(t.at(2), "One\ntwo\nthree");
    EXPECT_EQ(t.at(3), "No break at all");

    EXPECT_EQ(tr::parse_numbered_response("1. Alpha\\n\n", 1).at(1), "Alpha");

    auto tagged = tr::parse_numbered_response(
        "1. [Naoya] \"Morning.\"\\n\"And to you.\"\n", 1);
    ASSERT_EQ(tagged.size(), 1u);
    EXPECT_EQ(tagged.at(1), "\"Morning.\"\n\"And to you.\"");
    auto indented = tr::parse_numbered_response(
        std::string("1. ") + U3000 + "First half\\nsecond half\n", 1);
    ASSERT_EQ(indented.size(), 1u);
    EXPECT_EQ(indented.at(1), std::string(U3000) + "First half\nsecond half");
}

TEST(Translate, cache_file_round_trips_a_line_break) {
    const ScopedTempDir scratch("snu_ut_cache_breaks");
    const std::string path =
        (scratch.path() / fs::u8path("cache_breaks.json")).u8string();
    tr::Cache cache;
    cache.set("一行目\n二行目", "First half\nsecond half");
    tr::save_cache(cache, path);
    const tr::Cache back = tr::load_cache(path);
    ASSERT_TRUE(back.get("一行目\n二行目"));
    EXPECT_EQ(*back.get("一行目\n二行目"), "First half\nsecond half");
}

TEST(Translate, a_numbered_list_inside_a_source_line_parses_clean) {
    const std::string jp =
        std::string(U3000) + "１．巨乳系エッチ雑誌\n２．お尻系エッチ雑誌\n３．ロリコン系雑誌";
    std::vector<std::pair<std::string, std::string>> batch = {{"NARRATION", jp}};
    std::string prompt = tr::build_user_prompt(batch, {});
    EXPECT_NE(prompt.find("1. [NARRATION] " + std::string(U3000) +
                          "１．巨乳系エッチ雑誌\\n２．お尻系エッチ雑誌\\n３．ロリコン系雑誌\n"),
              std::string::npos);

    int rejected = -1;
    std::string tail = "unset";
    auto t = tr::parse_numbered_response(
        std::string("1. ") + U3000 +
            "1. Big-bust adult magazines\\n2. Rear-end adult magazines\\n"
            "3. Lolicon magazines\n",
        1, &rejected, &tail);
    ASSERT_EQ(t.size(), 1u) << "the list's own numbering was read as line numbers";
    EXPECT_EQ(rejected, 0);
    EXPECT_TRUE(tail.empty());
    EXPECT_EQ(t.at(1), std::string(U3000) +
                           "1. Big-bust adult magazines\n2. Rear-end adult magazines\n"
                           "3. Lolicon magazines");

    const std::string circled = std::string(U3000) + "①エッチな漫画雑誌" + U3000 +
                                "②エッチな漫画単行本\n③エッチな雑誌（写真）" + U3000 +
                                "④エッチな本（写真）";
    auto circled_t = tr::parse_numbered_response(
        "1. " + tr::escape_line_breaks(circled) + "\n", 1, &rejected);
    ASSERT_EQ(circled_t.size(), 1u);
    EXPECT_EQ(rejected, 0);
    EXPECT_EQ(circled_t.at(1), circled);
}

TEST(Translate, parse_numbered_response_reads_a_japanese_numbering_mark) {
    const char* const IDEOGRAPHIC_STOP = "。";
    const char* const IDEOGRAPHIC_COMMA = "、";
    for (const char* mark : {IDEOGRAPHIC_STOP, IDEOGRAPHIC_COMMA}) {
        int rejected = -1;
        auto t = tr::parse_numbered_response(
            "1" + std::string(mark) + " Alpha\n2" + mark + " Bravo\n", 2, &rejected);
        ASSERT_EQ(t.size(), 2u) << mark;
        EXPECT_EQ(t.at(1), "Alpha") << mark;
        EXPECT_EQ(t.at(2), "Bravo") << mark;
        EXPECT_EQ(rejected, 0) << mark;
    }
    auto mixed = tr::parse_numbered_response(
        "1. Alpha\n2" + std::string(IDEOGRAPHIC_STOP) + " Bravo\n3. Charlie\n", 3);
    ASSERT_EQ(mixed.size(), 3u);
    EXPECT_EQ(mixed.at(2), "Bravo") << "the line was glued onto entry 1";
}

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

    int rejected = -1;
    auto prose = tr::parse_numbered_response(
        "1. He waited.\n1985-1990 was the whole of it.\n2. Then he left.\n", 2, &rejected);
    ASSERT_EQ(prose.size(), 2u);
    EXPECT_EQ(rejected, 0);
    EXPECT_EQ(prose.at(1), "He waited. 1985-1990 was the whole of it.");
}

TEST(Translate, parse_numbered_response_keeps_emphasis_out_of_the_numbering) {
    struct Case {
        const char* body;
        const char* want;
    };
    const Case cases[] = {
        {"**1. Alpha**", "Alpha"},
        {"**1.** Bravo", "Bravo"},
        {"**1.** *Charlie*", "*Charlie*"},
        {"1.*sigh*", "*sigh*"},
        {"1.. Delta", "Delta"},
        {"1.Echo", "Echo"},
    };
    for (const auto& c : cases) {
        int rejected = -1;
        auto t = tr::parse_numbered_response(std::string(c.body) + "\n", 1, &rejected);
        ASSERT_EQ(t.size(), 1u) << c.body;
        EXPECT_EQ(t.at(1), c.want) << c.body;
        EXPECT_EQ(rejected, 0) << c.body;
    }
}

TEST(Translate, a_short_answer_that_stopped_on_its_own_cannot_be_applied) {
    EXPECT_TRUE(tr::answer_count_is_usable(150, 150, "end_turn"));
    EXPECT_FALSE(tr::answer_count_is_usable(150, 149, "end_turn"));
    EXPECT_FALSE(tr::answer_count_is_usable(150, 149, ""));
    EXPECT_FALSE(tr::answer_count_is_usable(150, 0, "end_turn"));
    EXPECT_FALSE(tr::answer_count_is_usable(150, 151, "end_turn"));

    EXPECT_TRUE(tr::answer_count_is_usable(150, 90, "max_tokens"));
    EXPECT_TRUE(tr::answer_count_is_usable(150, 90, "model_context_window_exceeded"));

    int rejected = -1;
    auto merged = tr::parse_numbered_response("1. A\n2. B and C together\n3. D\n", 4,
                                              &rejected);
    ASSERT_EQ(merged.size(), 3u);
    EXPECT_EQ(rejected, 0) << "the numbering is in order and in range";
    EXPECT_FALSE(tr::answer_count_is_usable(4, merged.size(), "end_turn"));
}

TEST(Translate, truncated_answer_loses_only_its_last_entry) {
    std::map<int, std::string> full = {
        {1, "He turned and left."}, {2, "The door stayed open."},
        {3, "He turned and said, \"I have always"}};

    auto t = full;
    EXPECT_TRUE(tr::drop_truncated_entry(t, "max_tokens"));
    EXPECT_EQ(t.size(), 2u);
    EXPECT_EQ(t.count(3), 0u) << "the half-written entry was kept";
    EXPECT_EQ(t.at(1), "He turned and left.");
    EXPECT_EQ(t.at(2), "The door stayed open.");

    auto ctx = full;
    EXPECT_TRUE(tr::drop_truncated_entry(ctx, "model_context_window_exceeded"));
    EXPECT_EQ(ctx.size(), 2u);
    EXPECT_EQ(ctx.count(3), 0u);

    for (const char* reason : {"end_turn", "stop_sequence", ""}) {
        auto keep = full;
        EXPECT_FALSE(tr::drop_truncated_entry(keep, reason)) << reason;
        EXPECT_EQ(keep.size(), 3u) << reason;
    }

    std::map<int, std::string> sparse = {{1, "One."}, {2, "Two."}, {7, "Seven and a ha"}};
    EXPECT_TRUE(tr::drop_truncated_entry(sparse, "max_tokens"));
    EXPECT_EQ(sparse.count(7), 0u);
    EXPECT_EQ(sparse.size(), 2u);

    std::map<int, std::string> none;
    EXPECT_FALSE(tr::drop_truncated_entry(none, "max_tokens"));
}

TEST(Translate, a_refused_answer_is_discarded_whole) {
    const std::map<int, std::string> full = {
        {1, "He turned and left."}, {2, "The door stayed open."},
        {3, "He turned and said, \"I have always"}};

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
        EXPECT_EQ(keep.size(), 3u) << reason;
    }

    std::map<int, std::string> none;
    EXPECT_TRUE(tr::discard_refused_answer(none, "refusal"));
}

TEST(Translate, raw_answer_log_sits_beside_the_run_it_belongs_to) {
    std::string sibling =
        (fs::temp_directory_path() / "script_output" / "translated_text.json").u8string();
    std::string log = tr::raw_answer_log_path(sibling);
    EXPECT_EQ(fs::u8path(log).parent_path(), fs::u8path(sibling).parent_path());
    EXPECT_EQ(fs::u8path(log).filename().u8string(), "raw_answers.log");

    EXPECT_TRUE(tr::raw_answer_log().empty()) << "a side file is written unasked";
    tr::set_raw_answer_log(log);
    EXPECT_EQ(tr::raw_answer_log(), log);
    tr::set_raw_answer_log("");
}

TEST(Translate, user_prompt_contains_speaker_tags_and_context) {
    std::vector<std::pair<std::string, std::string>> batch = {
        {"Naoya", "「おはよう」"}, {"NARRATION", "朝が来た。"}};
    std::vector<std::pair<std::string, std::string>> context = {
        {"Rin", "Good night."}};
    std::string prompt = tr::build_user_prompt(batch, context);
    EXPECT_NE(prompt.find("<previous_context>\n[Rin] Good night.\n"),
              std::string::npos);
    EXPECT_NE(prompt.find("1. [Naoya] 「おはよう」"), std::string::npos);
    EXPECT_NE(prompt.find("2. [NARRATION] 朝が来た。"), std::string::npos);
    EXPECT_NE(prompt.find("Output EXACTLY 2 numbered translations"),
              std::string::npos);
}

TEST(Translate, failed_line_stays_uncached_but_reaches_context) {
    tr::Cache cache;
    std::vector<std::pair<std::string, std::string>> context;

    tr::apply_line_result(cache, context, "Naoya", "「おはよう」", "");
    EXPECT_FALSE(cache.contains("「おはよう」"));
    EXPECT_EQ(cache.size(), 0u);
    ASSERT_EQ(context.size(), 1u);
    EXPECT_EQ(context[0].first, "Naoya");
    EXPECT_EQ(context[0].second, "「おはよう」");

    tr::apply_line_result(cache, context, "Rin", "朝が来た。", "Morning came.");
    ASSERT_TRUE(cache.get("朝が来た。"));
    EXPECT_EQ(*cache.get("朝が来た。"), "Morning came.");
    ASSERT_EQ(context.size(), 2u);
    EXPECT_EQ(context[1].second, "Morning came.");

    tr::apply_line_result(cache, context, "Naoya", "一行目\n二行目", "One line only.");
    ASSERT_TRUE(cache.get("一行目\n二行目"));
    EXPECT_EQ(*cache.get("一行目\n二行目"), "One line only.");
    tr::apply_line_result(cache, context, "Naoya", "一行だけ\n", "One line only.");
    EXPECT_TRUE(cache.contains("一行だけ\n"));
}

TEST(Translate, failed_entry_predicate_only_fires_on_japanese_identity) {
    EXPECT_TRUE(tr::is_failed_entry("こんにちは", "こんにちは"));
    EXPECT_TRUE(tr::is_failed_entry("……", "……"));
    EXPECT_TRUE(tr::is_failed_entry("Eスbg152n", "Eスbg152n"));

    EXPECT_FALSE(tr::is_failed_entry("こんにちは", "Hello"));
    EXPECT_FALSE(tr::is_failed_entry("直哉", "Naoya"));
    EXPECT_FALSE(tr::is_failed_entry("OK", "OK"));
    EXPECT_FALSE(tr::is_failed_entry("bg152n", "bg152n"));
    EXPECT_FALSE(tr::is_failed_entry("", ""));
}

TEST(Translate, purge_removes_failed_entries_and_keeps_the_rest) {
    tr::Cache cache;
    cache.set("直哉", "Naoya");
    cache.set("こんにちは", "こんにちは");
    cache.set("朝が来た。", "Morning came.");
    cache.set("OK", "OK");
    cache.set("桜の詩", "桜の詩");
    cache.set("稟", "Rin");

    EXPECT_EQ(tr::purge_failed_entries(cache), 2u);
    EXPECT_EQ(cache.size(), 4u);
    EXPECT_FALSE(cache.contains("こんにちは"));
    EXPECT_FALSE(cache.contains("桜の詩"));
    ASSERT_TRUE(cache.get("直哉"));
    EXPECT_EQ(*cache.get("直哉"), "Naoya");
    EXPECT_TRUE(cache.contains("朝が来た。"));
    EXPECT_TRUE(cache.contains("OK"));
    EXPECT_TRUE(cache.contains("稟"));

    const auto& items = cache.items();
    ASSERT_EQ(items.size(), 4u);
    EXPECT_EQ(items[0].first, "直哉");
    EXPECT_EQ(items[1].first, "朝が来た。");
    EXPECT_EQ(items[2].first, "OK");
    EXPECT_EQ(items[3].first, "稟");

    EXPECT_EQ(tr::purge_failed_entries(cache), 0u);
    EXPECT_EQ(cache.size(), 4u);
}

TEST(Translate, speaker_gate_refuses_a_document_that_lost_its_speakers) {
    tr::SpeakerCheckReport lost = tr::run_speaker_checks(dialogue_document(50, ""));
    EXPECT_FALSE(lost.passed);
    EXPECT_EQ(lost.failed_check, 1);
    EXPECT_NE(lost.reason.find("NARRATION"), std::string::npos);

    tr::SpeakerCheckReport thin = tr::run_speaker_checks(dialogue_document(4, "直哉"));
    EXPECT_FALSE(thin.passed);
    EXPECT_EQ(thin.failed_check, 1);

    tr::SpeakerCheckReport partial = tr::run_speaker_checks(dialogue_document(50, "直哉"));
    EXPECT_FALSE(partial.passed);
    EXPECT_EQ(partial.failed_check, 5);
}

TEST(Translate, a_failed_gate_stops_the_translation_step_before_it_spends) {
    const ScopedTempDir scratch("snu_ut_gate");
    const fs::path& dir = scratch.path();

    tr::TranslateOptions opt;
    opt.input_file = (dir / fs::u8path("extracted_text.json")).u8string();
    opt.cache_file =
        (dir / fs::u8path("translation_cache_anthropic.json")).u8string();
    opt.output_file = (dir / fs::u8path("translated_text.json")).u8string();
    opt.tsv_file = (dir / fs::u8path("translations.tsv")).u8string();
    tr::Cache seeded;
    seeded.set("朝が来た。", "Morning came.");
    tr::save_cache(seeded, opt.cache_file);
    opt.fresh_run = true;

    snu::write_file(opt.input_file, bj::serialize(bj::value(dialogue_document(50, ""))));

    EXPECT_EQ(tr::translate_all(opt), 2) << "a refused gate has to fail the pass";
    EXPECT_FALSE(fs::exists(fs::u8path(opt.output_file)));
    EXPECT_FALSE(fs::exists(fs::u8path(opt.tsv_file)));
    EXPECT_TRUE(fs::exists(fs::u8path(opt.cache_file)))
        << "the run stopped before it could drop work already paid for";
    EXPECT_EQ(tr::load_cache(opt.cache_file).size(), 1u);
}

TEST(Translate, the_pipeline_runs_three_steps_in_order) {
    const auto& stages = snu::apps::pipeline_steps();
    ASSERT_EQ(stages.size(), 3u);
    EXPECT_STREQ(stages[0].name, "01_extract");
    EXPECT_STREQ(stages[1].name, "02_build_tsv");
    EXPECT_STREQ(stages[2].name, "deploy");
    for (const auto& entry : stages) {
        EXPECT_EQ(std::string(entry.name).find("speaker_gate"), std::string::npos);
        EXPECT_EQ(std::string(entry.name).find("build_translations"), std::string::npos);
        EXPECT_FALSE(std::string(entry.what).empty());
    }

    EXPECT_TRUE(tr::validate_options(0, 0, 0).has_value()) << "--batch 0";
    EXPECT_TRUE(tr::validate_options(150, 0, -1).has_value()) << "--max-batches -1";
    EXPECT_TRUE(tr::validate_options(150, -1, 0).has_value()) << "--test -1";
    EXPECT_FALSE(tr::validate_options(150, 0, 0).has_value());
}

TEST(Translate, glossary_contains_core_names_and_is_consistent) {
    const auto& map = tr::name_translations();
    EXPECT_EQ(map.count("直哉"), 1u);
    EXPECT_EQ(map.at("直哉"), "Naoya");
    EXPECT_EQ(tr::name_translations_ordered().size(), map.size());
}

TEST(Translate, a_flag_that_deletes_a_paid_cache_is_refused_until_it_is_meant) {
    EXPECT_FALSE(tr::refuse_cache_discard(0, "--test", false).has_value());
    EXPECT_FALSE(tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD,
                                          "--test", false).has_value());

    for (const char* flag : {"--test", "--retranslate", "--clean"}) {
        auto why = tr::refuse_cache_discard(tr::CACHE_DISCARD_THRESHOLD + 1,
                                            flag, false);
        ASSERT_TRUE(why.has_value()) << flag;
        EXPECT_NE(why->find(flag), std::string::npos) << *why;
        EXPECT_NE(why->find("--max-batches"), std::string::npos) << *why;
        EXPECT_NE(why->find("--discard-cache"), std::string::npos) << *why;
    }

    auto why = tr::refuse_cache_discard(56103, "--test", false);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("56103"), std::string::npos) << *why;

    EXPECT_FALSE(tr::refuse_cache_discard(56103, "--test", true).has_value());
}

TEST(Translate, the_cache_guard_counts_the_file_on_disk) {
    const ScopedTempDir scratch("snu_ut_cache_guard");
    const std::string missing =
        (scratch.path() / fs::u8path("no_such_cache.json")).u8string();
    EXPECT_EQ(tr::cache_entry_count(missing), 0u);

    const std::string path =
        (scratch.path() / fs::u8path("guard_cache.json")).u8string();
    tr::Cache cache;
    for (int i = 0; i < 7; ++i) {
        cache.set("日本語" + std::to_string(i), "English " + std::to_string(i));
    }
    tr::save_cache(cache, path);
    EXPECT_EQ(tr::cache_entry_count(path), 7u);
}
