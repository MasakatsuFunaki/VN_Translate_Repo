// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// TSV builder tests: escape correctness, glossary name rows, coverage counting
// + the deployed-TSV shape gate (skips when the TSV hasn't been built yet).
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "build_tsv/box_budget.h"
#include "build_tsv/build_tsv.h"
#include "common/coverage.h"
#include "common/util.h"
#include "cache/glossary.h"
#include "cache/cache_core.h"

namespace fs = std::filesystem;
namespace bt = snu::build_tsv;
namespace tr = snu::translate;
namespace bj = boost::json;

namespace {

// jp -> list of en, in file order, from a built TSV.
std::map<std::string, std::vector<std::string>> read_tsv(const std::string& path) {
    std::map<std::string, std::vector<std::string>> rows;
    std::ifstream f(fs::u8path(path), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        rows[line.substr(0, tab)].push_back(line.substr(tab + 1));
    }
    return rows;
}

// Unique names across both glossary tables; each ships bare and in backlog brackets.
std::size_t glossary_names() {
    std::set<std::string> names;
    for (const auto* table : {&snu::translate::name_translations_ordered(),
                              &snu::translate::extra_name_translations()})
        for (const auto& [jp, en] : *table) names.insert(jp);
    return names.size();
}

// The backlog's form of a name key: the engine's lenticular brackets round it.
std::string bracketed(const std::string& s) {
    return "\xE3\x80\x90" + s + "\xE3\x80\x91";
}

}  // namespace

// The TSV escape MUST not let raw \t through -- that's the column
// separator; a leak corrupts the line-based parse on the DLL side.
TEST(BuildTsv, escape_function_strips_tab) {
    std::string out = bt::esc("a\tb");
    EXPECT_EQ(out.find('\t'), std::string::npos);
    EXPECT_EQ(out, "a\\tb");
}

// A source break must survive the full chain: escape in the answer, real byte
// in the cache, escape again in the table row the DLL parses back into a break.
// The break travels from the answer into the cache and the key unchanged; the
// shipped value is run_build's business (see run_build_flows_every_break...).
TEST(BuildTsv, a_line_break_survives_from_the_answer_to_the_cache_and_the_key) {
    const std::string jp = "一行目\n二行目";

    auto answered = tr::parse_numbered_response("1. First line\\nsecond line\n", 1);
    ASSERT_EQ(answered.size(), 1u);

    tr::Cache cache;
    cache.set(jp, answered.at(1));
    ASSERT_TRUE(cache.get(jp));
    EXPECT_EQ(*cache.get(jp), "First line\nsecond line")
        << "the cache holds the answer as given; the table decides the layout";

    const std::string en = bt::normalize_leading_space(jp, *cache.get(jp));
    const std::string row = bt::esc(jp) + "\t" + bt::esc(en);
    EXPECT_EQ(row, "一行目\\n二行目\tFirst line\\nsecond line");
    EXPECT_EQ(row.find('\n'), std::string::npos)
        << "a break written raw would split the row into two TSV lines";
    EXPECT_EQ(std::count(row.begin(), row.end(), '\t'), 1)
        << "the escape has to leave exactly one column separator";

    // Three lines, and an indent on the key, travel the same way.
    const std::string indented = "\xE3\x80\x80" "一\n二\n三";
    auto three = tr::parse_numbered_response("1. One\\ntwo\\nthree\n", 1);
    ASSERT_EQ(three.size(), 1u);
    const std::string value = bt::normalize_leading_space(indented, three.at(1));
    EXPECT_EQ(value, "\xE3\x80\x80" "One\ntwo\nthree");
    EXPECT_EQ(bt::esc(indented) + "\t" + bt::esc(value),
              "\xE3\x80\x80" "一\\n二\\n三\t" "\xE3\x80\x80" "One\\ntwo\\nthree");
}

TEST(BuildTsv, escape_function_full_set) {
    EXPECT_EQ(bt::esc("a\\b"), "a\\\\b");
    EXPECT_EQ(bt::esc("a\rb\nc"), "a\\rb\\nc");
    EXPECT_EQ(bt::esc("plain"), "plain");
}

TEST(BuildTsv, run_build_produces_one_tab_per_line) {
    // Minimal translated_text.json: one script, one translated string,
    // one untranslated (JP == EN, skipped), one with embedded newline.
    bj::object entry1;
    entry1["text"] = "こんにちは";
    entry1["translated"] = "Hello";
    bj::object entry2;
    entry2["text"] = "未翻訳";
    entry2["translated"] = "未翻訳";
    bj::object entry3;
    entry3["text"] = "改行\nあり";
    entry3["translated"] = "with\nnewline";
    bj::object script;
    script["strings"] = bj::array{entry1, entry2, entry3};
    bj::object root;
    root["sc01"] = script;

    std::string in = (fs::temp_directory_path() / "snu_ut_translated.json").u8string();
    std::string out = (fs::temp_directory_path() / "snu_ut_translations.tsv").u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));

    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    {
        std::ifstream f(fs::u8path(out), std::ios::binary);
        std::string line;
        std::size_t lines = 0;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            ++lines;
            EXPECT_EQ(std::count(line.begin(), line.end(), '\t'), 1)
                << "line has != 1 tab: " << line;
        }
        // Two script rows (the untranslated JP==EN entry is skipped), one row
        // per glossary name, and the backlog's bracketed form of each name.
        EXPECT_EQ(lines, 2u + snu::translate::name_translations_ordered().size() +
                             snu::translate::extra_name_translations().size() +
                             glossary_names());
    }
    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
    fs::remove(fs::u8path(snu::coverage_report_path(in)));
}

// The engine eats <...> tags; copied ruby tags cost kana that don't belong
// in English, so the table build strips them.  A lone '<' is drawn and kept.
TEST(BuildTsv, run_build_takes_ruby_markup_out_of_the_english) {
    bj::object copied_tag;
    copied_tag["text"] = "散る<r さくら>桜</r>";
    copied_tag["translated"] = "the falling <r さくら>cherry</r> blossom";
    bj::object stray;
    stray["text"] = "残り";
    stray["translated"] = "what is < left";
    bj::object clean;
    clean["text"] = "こんにちは";
    clean["translated"] = "Hello";
    bj::object script;
    script["strings"] = bj::array{copied_tag, stray, clean};
    bj::object root;
    root["sc01"] = script;

    std::string in = (fs::temp_directory_path() / "snu_ut_markup.json").u8string();
    std::string out = (fs::temp_directory_path() / "snu_ut_markup.tsv").u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    auto rows = read_tsv(out);
    // The key is the wire bytes and keeps the tag the script wrote; only the
    // English loses it.
    ASSERT_EQ(rows.count("散る<r さくら>桜</r>"), 1u);
    EXPECT_EQ(rows.at("散る<r さくら>桜</r>")[0], "the falling cherry blossom");
    // A '<' with no '>' after it is drawn by the engine as itself, so the
    // table keeps it: rewriting it would only lose a comparison.
    EXPECT_EQ(rows.at("残り")[0], "what is < left");
    EXPECT_EQ(rows.at("こんにちは")[0], "Hello");

    // No value carries a pair the engine would consume.
    for (const auto& [jp, values] : rows)
        for (const std::string& en : values) {
            const size_t open = en.find('<');
            if (open == std::string::npos) continue;
            EXPECT_EQ(en.find('>', open + 1), std::string::npos)
                << jp << " -> " << en;
        }

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
    fs::remove(fs::u8path(snu::coverage_report_path(in)));
}

TEST(BuildTsv, deployed_tsv_carries_no_ruby_markup) {
    fs::path tsv = fs::u8path(std::string(SNU_PROJECT_DIR) + "/translations.tsv");
    if (!fs::exists(tsv)) GTEST_SKIP() << "translations.tsv not built yet";
    std::ifstream f(tsv, std::ios::binary);
    std::string line;
    std::size_t bad = 0;
    while (std::getline(f, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string en = line.substr(tab + 1);
        const auto open = en.find('<');
        if (open == std::string::npos) continue;
        if (en.find('>', open + 1) != std::string::npos) {
            ++bad;
            if (bad < 5) GTEST_LOG_(INFO) << "markup in: " << en.substr(0, 80);
        }
    }
    EXPECT_EQ(bad, 0u) << bad << " English values carry a bracket pair the "
                                 "engine would consume as a tag";
}

// Glossary names are appended to the table (one row each, not duplicated
// when the translated document already covers them).
TEST(BuildTsv, run_build_emits_every_glossary_name_once) {
    bj::object translated_name;
    translated_name["text"] = "直哉";
    translated_name["translated"] = "Naoya";
    bj::object identity;
    identity["text"] = "未翻訳";
    identity["translated"] = "未翻訳";
    bj::object line;
    line["text"] = "こんにちは";
    line["translated"] = "Hello";
    bj::object script;
    script["strings"] = bj::array{translated_name, identity, line};
    bj::object root;
    root["sc01"] = script;

    std::string in = (fs::temp_directory_path() / "snu_ut_names.json").u8string();
    std::string out = (fs::temp_directory_path() / "snu_ut_names.tsv").u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    auto rows = read_tsv(out);
    const auto& speakers = snu::translate::name_translations_ordered();
    const auto& extras = snu::translate::extra_name_translations();
    for (const auto* table : {&speakers, &extras})
        for (const auto& [jp, en] : *table) {
            ASSERT_EQ(rows.count(jp), 1u) << "missing glossary row for " << jp;
            EXPECT_EQ(rows.at(jp).size(), 1u) << "duplicate glossary row for " << jp;
            EXPECT_EQ(rows.at(jp).front(), en) << "wrong English for " << jp;
        }
    // Every character of the documented cast has a row, whatever the scripts
    // happen to contain.
    for (const char* jp : {"直哉", "稟", "健一郎", "圭", "優美", "里奈",
                           "美和", "冬馬", "明石", "篠田", "弓子", "伯奇"})
        EXPECT_EQ(rows.count(jp), 1u) << "no row for cast name " << jp;

    EXPECT_EQ(rows.count("未翻訳"), 0u);  // identity entries still dropped
    EXPECT_EQ(rows.at("こんにちは").front(), "Hello");
    // 直哉 is shared with the glossary, so only こんにちは adds to the tables.
    // Every name then ships a second time in the backlog's brackets.
    EXPECT_EQ(rows.size(), speakers.size() + extras.size() + 1 + glossary_names());
    ASSERT_EQ(rows.count(bracketed("直哉")), 1u);
    EXPECT_EQ(rows.at(bracketed("直哉")).front(),
              bracketed(rows.at("直哉").front()))
        << "the backlog and the name plate have to spell the same person alike";

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
    fs::remove(fs::u8path(snu::coverage_report_path(in)));
}

// When the document and glossary disagree on a name, the document wins.
TEST(BuildTsv, run_build_keeps_document_english_over_glossary) {
    const std::string jp_name = "直哉";
    const std::string document_english = "Naoya Kusanagi";
    const std::string glossary_english =
        snu::translate::name_translations().at(jp_name);
    ASSERT_NE(document_english, glossary_english)
        << "the test needs the two sides to disagree";

    bj::object named;
    named["text"] = jp_name;
    named["translated"] = document_english;
    bj::object script;
    script["strings"] = bj::array{named};
    bj::object root;
    root["sc01"] = script;

    std::string in = (fs::temp_directory_path() / "snu_ut_precedence.json").u8string();
    std::string out = (fs::temp_directory_path() / "snu_ut_precedence.tsv").u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    auto rows = read_tsv(out);
    ASSERT_EQ(rows.count(jp_name), 1u);
    EXPECT_EQ(rows.at(jp_name).size(), 1u) << "the glossary added a second row";
    EXPECT_EQ(rows.at(jp_name).front(), document_english);
    // Every other glossary name is untouched by the document and keeps its
    // glossary English, so the injection itself still ran.
    for (const auto* table : {&snu::translate::name_translations_ordered(),
                              &snu::translate::extra_name_translations()})
        for (const auto& [jp, en] : *table)
            if (jp != jp_name) EXPECT_EQ(rows.at(jp).front(), en) << jp;

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
    fs::remove(fs::u8path(snu::coverage_report_path(in)));
}

// The three glossary tables serve different contracts and their key sets must
// not overlap.  Two names that draw alike are one person to the player.
TEST(BuildTsv, colliding_name_plates_are_reported_and_nothing_else_is) {
    const std::vector<std::pair<std::string, std::string>> rows = {
        {"圭", "Kei"},
        {"直哉", "Naoya"},
        {"看護婦", "Nurse"},
        {"看護師", "Nurse"},
        {"老人", "Old Man"},
    };
    const auto shared = bt::name_plates_sharing_english(rows);
    ASSERT_EQ(shared.size(), 1u);
    EXPECT_EQ(shared.front(), "看護婦 / 看護師 -> Nurse");

    // Three keys on one value name all three.
    EXPECT_EQ(bt::name_plates_sharing_english(
                  {{"甲", "X"}, {"乙", "X"}, {"丙", "X"}}),
              std::vector<std::string>{"甲 / 乙 / 丙 -> X"});

    // One key per value is what a table is supposed to look like.
    const std::vector<std::pair<std::string, std::string>> clean = {
        {"圭", "Kei"}, {"直哉", "Naoya"}, {"老人", "Old Man"}};
    EXPECT_TRUE(bt::name_plates_sharing_english(clean).empty());
    EXPECT_TRUE(bt::name_plates_sharing_english({}).empty());
}

// Same collision check over the glossary tables, where a clash is permanent.
TEST(BuildTsv, no_two_glossary_names_are_spelled_alike) {
    std::vector<std::pair<std::string, std::string>> rows;
    for (const auto* table : {&tr::name_translations_ordered(),
                              &tr::extra_name_translations()})
        for (const auto& [jp, en] : *table) rows.emplace_back(jp, en);

    const auto shared = bt::name_plates_sharing_english(rows);
    std::string listed;
    for (const std::string& s : shared) listed += "\n  " + s;
    EXPECT_TRUE(shared.empty())
        << shared.size() << " English name(s) stand for more than one "
        << "character:" << listed;
}

// The backlog's rows are the plates' rows inside the engine's brackets, so the
// shipped table carries the check's answer in a form that needs no glossary.
TEST(BuildTsv, deployed_tsv_draws_no_two_names_alike) {
    fs::path tsv = fs::u8path(std::string(SNU_PROJECT_DIR) + "/translations.tsv");
    if (!fs::exists(tsv)) GTEST_SKIP() << "translations.tsv not built yet";
    std::vector<std::pair<std::string, std::string>> name_rows;
    std::ifstream f(tsv, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string jp = line.substr(0, tab);
        if (jp.size() < 7 || jp.compare(0, 3, "\xE3\x80\x90") != 0) continue;
        name_rows.emplace_back(jp, line.substr(tab + 1));
    }
    ASSERT_FALSE(name_rows.empty()) << "the table should carry backlog names";
    const auto shared = bt::name_plates_sharing_english(name_rows);
    std::string listed;
    for (const std::string& s : shared) listed += "\n  " + s;
    EXPECT_TRUE(shared.empty())
        << shared.size() << " name(s) the backlog draws alike:" << listed;
}

TEST(BuildTsv, glossary_tables_are_key_disjoint) {
    const auto& speakers = snu::translate::name_translations_ordered();
    const auto& extras = snu::translate::extra_name_translations();
    const auto& terms = snu::translate::terminology_translations();

    ASSERT_FALSE(speakers.empty());
    ASSERT_FALSE(extras.empty());
    ASSERT_FALSE(terms.empty());

    std::map<std::string, std::string> seen;  // key -> which table
    auto add = [&](const auto& table, const char* table_name) {
        for (const auto& [jp, en] : table) {
            auto [it, fresh] = seen.emplace(jp, table_name);
            EXPECT_TRUE(fresh) << jp << " is in both " << it->second << " and "
                               << table_name << "; it belongs in exactly one";
        }
    };
    add(speakers, "name_translations_ordered");
    add(extras, "extra_name_translations");
    add(terms, "terminology_translations");

    // Anchors: if a table is emptied or repurposed, the assertions above go
    // quiet but these do not.
    EXPECT_EQ(seen.at("直哉"), "name_translations_ordered");
    EXPECT_EQ(seen.at("篠田"), "extra_name_translations");
    EXPECT_EQ(seen.at("横たわる櫻"), "terminology_translations");
    EXPECT_GE(seen.size(), speakers.size() + extras.size() + terms.size());
}

// Terminology is prompt-only and must never appear as a runtime table row.
TEST(BuildTsv, terminology_never_becomes_a_runtime_row) {
    bj::object line;
    line["text"] = "こんにちは";
    line["translated"] = "Hello";
    bj::object script;
    script["strings"] = bj::array{line};
    bj::object root;
    root["sc01"] = script;

    std::string in = (fs::temp_directory_path() / "snu_ut_terms.json").u8string();
    std::string out = (fs::temp_directory_path() / "snu_ut_terms.tsv").u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    auto rows = read_tsv(out);
    for (const auto& [jp, en] : snu::translate::terminology_translations())
        EXPECT_EQ(rows.count(jp), 0u) << "terminology leaked into the table: " << jp;

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
    fs::remove(fs::u8path(snu::coverage_report_path(in)));
}

// Every superseded rendering must name a live pin and an English it no longer uses.
TEST(BuildTsv, superseded_renderings_name_live_pins) {
    const auto& superseded = snu::translate::superseded_renderings();
    ASSERT_FALSE(superseded.empty());
    std::map<std::string, std::string> pins;
    for (const auto& [jp, en] : snu::translate::terminology_translations())
        pins.emplace(jp, en);
    for (const auto& [jp, old_en] : superseded) {
        ASSERT_EQ(pins.count(jp), 1u) << jp << " is no longer a pinned term";
        EXPECT_NE(pins.at(jp), old_en)
            << jp << " lists its own current rendering as superseded";
        EXPECT_FALSE(old_en.empty());
    }
    // The two corrections the table exists for.
    EXPECT_EQ(pins.at("鳥谷"), "Toritani");
    EXPECT_EQ(pins.at("横たわる櫻"), "The Reclining Sakura");
}

// Same contract for the name table: every superseded name is still a live
// name, and names an English that name no longer uses.
TEST(BuildTsv, superseded_name_renderings_name_live_names) {
    const auto& superseded = snu::translate::superseded_name_renderings();
    ASSERT_FALSE(superseded.empty());
    std::map<std::string, std::string> names;
    for (const auto* table : {&snu::translate::name_translations_ordered(),
                              &snu::translate::extra_name_translations()})
        for (const auto& [jp, en] : *table) names.emplace(jp, en);

    for (const auto& [jp, old_en] : superseded) {
        ASSERT_EQ(names.count(jp), 1u) << jp << " is not a name in either table";
        EXPECT_NE(names.at(jp), old_en)
            << jp << " lists its own current spelling as superseded";
        EXPECT_FALSE(old_en.empty());
    }
    // The corrections the table exists for -- each one a reading the corpus
    // states in furigana and the table used to contradict.
    EXPECT_EQ(names.at("吹"), "Sui");
    EXPECT_EQ(names.at("寧"), "Nei");
    EXPECT_EQ(names.at("宇綱"), "Ietsuna");
    EXPECT_EQ(names.at("直哉＆吹"), "Naoya & Sui");
}

// Pinned names are repaired in both the name-window row and the prose,
// keyed on the Japanese in each row.
TEST(BuildTsv, pinned_names_repair_the_row_and_the_prose_around_it) {
    // The bare-name rows, which is what the name window reads.
    EXPECT_EQ(bt::apply_pinned_terms("吹", "Fuki"), "Sui");
    EXPECT_EQ(bt::apply_pinned_terms("寧", "Yasushi"), "Nei");
    EXPECT_EQ(bt::apply_pinned_terms("宇綱", "Utsuna"), "Ietsuna");
    EXPECT_EQ(bt::apply_pinned_terms("直哉＆吹", "Naoya & Fuki"), "Naoya & Sui");

    // Inside a sentence, with punctuation and possessives on either side.
    EXPECT_EQ(bt::apply_pinned_terms("「思い出しました。吹です」",
                                     "\"I remember now. I am Fuki.\""),
              "\"I remember now. I am Sui.\"");
    EXPECT_EQ(bt::apply_pinned_terms("寧の飴。", "Yasushi's candy."), "Nei's candy.");

    // Already correct -> untouched, which is every row of a fresh run.
    EXPECT_EQ(bt::apply_pinned_terms("吹は笑った。", "Sui laughed."), "Sui laughed.");
    // The old spelling without the Japanese to vouch for it is left alone.
    EXPECT_EQ(bt::apply_pinned_terms("朝が来た。", "Fuki laughed."), "Fuki laughed.");
    // Whole terms only.
    EXPECT_EQ(bt::apply_pinned_terms("吹は笑った。", "Fukiko laughed."),
              "Fukiko laughed.");
}

// End to end: the shipped table carries corrected spellings; the cache is untouched.
TEST(BuildTsv, run_build_ships_the_corrected_name) {
    auto entry = [](const std::string& jp, const std::string& en) {
        bj::object o;
        o["text"] = jp;
        o["translated"] = en;
        return o;
    };
    const std::string prose = "「思い出しました。吹です」";
    bj::object script;
    script["strings"] = bj::array{
        entry("吹", "Fuki"),
        entry("寧", "Yasushi"),
        entry("宇綱", "Utsuna"),
        entry("直哉＆吹", "Naoya & Fuki"),
        entry(prose, "\"I remember now. I am Fuki.\""),
    };
    bj::object root;
    root["sc01"] = script;

    std::string in = (fs::temp_directory_path() / "snu_ut_names_repair.json").u8string();
    std::string out = (fs::temp_directory_path() / "snu_ut_names_repair.tsv").u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    auto rows = read_tsv(out);
    EXPECT_EQ(rows.at("吹").front(), "Sui");
    EXPECT_EQ(rows.at("寧").front(), "Nei");
    EXPECT_EQ(rows.at("宇綱").front(), "Ietsuna");
    EXPECT_EQ(rows.at("直哉＆吹").front(), "Naoya & Sui");
    EXPECT_EQ(rows.at(prose).front(), "\"I remember now. I am Sui.\"");
    // Exactly one row per name -- the document row is repaired in place rather
    // than joined by a second row from the glossary.
    EXPECT_EQ(rows.at("吹").size(), 1u);

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
    fs::remove(fs::u8path(snu::coverage_report_path(in)));
}

// A repair that contains the old spelling must be idempotent across rebuilds.
TEST(BuildTsv, a_repair_that_contains_the_spelling_it_replaces_runs_once) {
    const std::string current = tr::name_translations().at("看護師");
    ASSERT_NE(current.find("Nurse"), std::string::npos)
        << "this test is about a replacement that carries the old spelling";

    const std::string once = bt::apply_pinned_terms("看護師", "Nurse");
    EXPECT_EQ(once, current);
    EXPECT_EQ(bt::apply_pinned_terms("看護師", once), once);
    EXPECT_EQ(bt::apply_pinned_terms("看護師", bt::apply_pinned_terms("看護師", once)),
              once);

    // And a sentence that mentions her keeps the rest of itself.
    EXPECT_EQ(bt::apply_pinned_terms("看護師が来た", "The Nurse arrived."),
              "The " + current + " arrived.");
}

TEST(BuildTsv, pinned_terms_repair_only_what_the_japanese_vouches_for) {
    const std::string painting = "『横たわる櫻』";
    // The rendering the pin replaced, in a row whose source carries the term.
    EXPECT_EQ(bt::apply_pinned_terms(painting + "を描いた男。",
                                     "The man who painted 'The Prostrate Cherry'."),
              "The man who painted 'The Reclining Sakura'.");
    // Mid-sentence, with punctuation on both sides.
    EXPECT_EQ(bt::apply_pinned_terms(painting + "以外に。",
                                     "Beyond 'The Prostrate Cherry,' nothing."),
              "Beyond 'The Reclining Sakura,' nothing.");
    // Already correct -> untouched.  This is the common row.
    EXPECT_EQ(bt::apply_pinned_terms(painting + "を描いた男。",
                                     "The man who painted 'The Reclining Sakura'."),
              "The man who painted 'The Reclining Sakura'.");
    // Nothing pinned in the row at all -> byte-for-byte the same string.
    EXPECT_EQ(bt::apply_pinned_terms("朝が来た。", "Morning came."), "Morning came.");
    EXPECT_EQ(bt::apply_pinned_terms("", ""), "");
    // The old English without the Japanese to vouch for it is left alone: the
    // rewrite may not invent a correction the source does not support.
    EXPECT_EQ(bt::apply_pinned_terms("朝が来た。", "The Prostrate Cherry stood there."),
              "The Prostrate Cherry stood there.");

    // A changed surname reaches the table the same way.
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷は黙っていた。", "Toriya said nothing."),
              "Toritani said nothing.");
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷の絵。", "Toriya's painting."),
              "Toritani's painting.");
    // Whole terms only -- a longer word that merely starts the same survives.
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷は黙っていた。", "Toriyama said nothing."),
              "Toriyama said nothing.");
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷は黙っていた。", "Ask Toriya, ask Toriya2."),
              "Ask Toritani, ask Toriya2.");
    // Surrounding text is preserved exactly, including repeats.
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷と鳥谷。", "Toriya and Toriya."),
              "Toritani and Toritani.");
}

// End-to-end: the rows the table is built from are repaired as it is written,
// and the cache the values came from is never touched.
TEST(BuildTsv, run_build_repairs_superseded_renderings_in_place) {
    auto entry = [](const std::string& jp, const std::string& en) {
        bj::object o;
        o["text"] = jp;
        o["translated"] = en;
        return o;
    };
    const std::string stale_jp = "『横たわる櫻』以外にあの男は桜の絵を発表しなかった。";
    const std::string good_jp = "『横たわる櫻』を描いた男。";
    bj::object script;
    script["strings"] = bj::array{
        entry(stale_jp, "Beyond 'The Prostrate Cherry,' that man never exhibited another."),
        entry(good_jp, "The man who painted 'The Reclining Sakura'."),
        entry("朝が来た。", "Morning came."),
    };
    bj::object root;
    root["sc01"] = script;

    std::string in = (fs::temp_directory_path() / "snu_ut_repin.json").u8string();
    std::string out = (fs::temp_directory_path() / "snu_ut_repin.tsv").u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    auto rows = read_tsv(out);
    EXPECT_EQ(rows.at(stale_jp).front(),
              "Beyond 'The Reclining Sakura,' that man never exhibited another.");
    EXPECT_EQ(rows.at(good_jp).front(), "The man who painted 'The Reclining Sakura'.");
    EXPECT_EQ(rows.at("朝が来た。").front(), "Morning came.");
    EXPECT_EQ(rows.at("直哉").front(), "Naoya");  // name rows pass through

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
    fs::remove(fs::u8path(snu::coverage_report_path(in)));
}

namespace {
const std::string IDEO = "\xE3\x80\x80";  // U+3000, the engine's paragraph indent
}

// The English indent must match the Japanese key's; the build enforces this.
TEST(BuildTsv, leading_space_is_copied_from_the_japanese_key) {
    // Indented key -> indented English.
    EXPECT_EQ(bt::normalize_leading_space(IDEO + "桜が舞う。", "The blossoms scatter."),
              IDEO + "The blossoms scatter.");
    // Already correct -> unchanged.
    EXPECT_EQ(bt::normalize_leading_space(IDEO + "桜が舞う。", IDEO + "The blossoms scatter."),
              IDEO + "The blossoms scatter.");
    // Plain key -> plain English, whatever the model prefixed.
    EXPECT_EQ(bt::normalize_leading_space("桜が舞う。", IDEO + "The blossoms scatter."),
              "The blossoms scatter.");
    EXPECT_EQ(bt::normalize_leading_space("桜が舞う。", "The blossoms scatter."),
              "The blossoms scatter.");
    // Bracketed dialogue is unindented in the source and must stay unindented.
    EXPECT_EQ(bt::normalize_leading_space("「この美しさは本物である」",
                                          IDEO + "\"This beauty is real.\""),
              "\"This beauty is real.\"");
    // A doubled indent is a real form and is reproduced exactly.
    EXPECT_EQ(bt::normalize_leading_space(IDEO + IDEO + "深い場所。", "A deep place."),
              IDEO + IDEO + "A deep place.");
    // Stray ASCII blanks are not indentation either.
    EXPECT_EQ(bt::normalize_leading_space("桜。", "  Cherry."), "Cherry.");
    EXPECT_EQ(bt::normalize_leading_space(IDEO + "桜。", " \tCherry."), IDEO + "Cherry.");
    // Interior spacing is untouched.
    EXPECT_EQ(bt::normalize_leading_space("桜。", "a  b"), "a  b");
}

TEST(BuildTsv, run_build_writes_rows_with_the_key_s_indentation) {
    auto entry = [](const std::string& jp, const std::string& en) {
        bj::object o;
        o["text"] = jp;
        o["translated"] = en;
        return o;
    };
    bj::object script;
    script["strings"] = bj::array{
        entry(IDEO + "桜が舞う。", "The blossoms scatter."),           // must gain it
        entry("「本物である」", IDEO + "\"It is real.\""),             // must lose it
        entry(IDEO + "春の空。", IDEO + "The spring sky."),            // must keep it
        entry("朝が来た。", "Morning came."),                          // untouched
    };
    bj::object root;
    root["sc01"] = script;

    std::string in = (fs::temp_directory_path() / "snu_ut_indent.json").u8string();
    std::string out = (fs::temp_directory_path() / "snu_ut_indent.tsv").u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    auto rows = read_tsv(out);
    EXPECT_EQ(rows.at(IDEO + "桜が舞う。").front(), IDEO + "The blossoms scatter.");
    EXPECT_EQ(rows.at("「本物である」").front(), "\"It is real.\"");
    EXPECT_EQ(rows.at(IDEO + "春の空。").front(), IDEO + "The spring sky.");
    EXPECT_EQ(rows.at("朝が来た。").front(), "Morning came.");
    // Glossary names carry no indent and must not acquire one.
    EXPECT_EQ(rows.at("直哉").front(), "Naoya");

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
    fs::remove(fs::u8path(snu::coverage_report_path(in)));
}

// Coverage counts split real English from lines still holding the Japanese,
// per script and in total, and the report names the document it counted.
TEST(BuildTsv, coverage_counts_real_english_against_identity) {
    auto entry = [](const char* jp, const char* en) {
        bj::object o;
        o["text"] = jp;
        o["translated"] = en;
        return o;
    };
    bj::object bare;  // no 'translated' field at all -- nothing replaced it yet
    bare["text"] = "未処理";

    bj::object sc01;
    sc01["strings"] = bj::array{entry("こんにちは", "Hello"), entry("未翻訳", "未翻訳"),
                                bare};
    bj::object sc02;
    sc02["strings"] = bj::array{entry("朝が来た。", "Morning came.")};
    bj::object empty;
    empty["strings"] = bj::array{};
    bj::object root;
    root["sc01"] = sc01;
    root["sc02"] = sc02;
    root["sc03"] = empty;

    auto report = snu::count_coverage(root, "some_document.json");
    EXPECT_EQ(report.source_file, "some_document.json");
    EXPECT_EQ(report.totals.translated, 2);
    EXPECT_EQ(report.totals.identity, 2);
    EXPECT_EQ(report.totals.exempt, 0);
    EXPECT_EQ(report.totals.total, 4);
    ASSERT_EQ(report.per_script.size(), 3u);
    EXPECT_EQ(report.per_script.at("sc01").translated, 1);
    EXPECT_EQ(report.per_script.at("sc01").identity, 2);
    EXPECT_EQ(report.per_script.at("sc01").total, 3);
    EXPECT_EQ(report.per_script.at("sc02").translated, 1);
    EXPECT_EQ(report.per_script.at("sc03").total, 0);

    std::string out = (fs::temp_directory_path() / "snu_ut_coverage.json").u8string();
    snu::write_coverage_report(report, out);
    auto back = snu::json_parse_file(out).get_object();
    EXPECT_EQ(back.at("source_file").get_string(), "some_document.json");
    EXPECT_EQ(back.at("totals").get_object().at("translated").get_int64(), 2);
    EXPECT_EQ(back.at("totals").get_object().at("total").get_int64(), 4);
    EXPECT_EQ(back.at("scripts").get_object().at("sc02").get_object()
                  .at("translated").get_int64(), 1);
    fs::remove(fs::u8path(out));
}

// Punctuation-only lines are exempt from the coverage denominator since their
// English is legitimately identical to the Japanese.
TEST(BuildTsv, coverage_separates_exempt_lines_from_real_misses) {
    auto entry = [](const char* jp, const char* en) {
        bj::object o;
        o["text"] = jp;
        o["translated"] = en;
        return o;
    };
    bj::object sc01;
    sc01["strings"] = bj::array{
        entry("こんにちは", "Hello"),        // translated
        entry("未翻訳", "未翻訳"),           // a real miss
        entry("　・・・", "　・・・"),        // exempt: symbols only
        entry("？？？", "？？？"),           // exempt
        entry("「……」", "\"......\""),      // symbols, but genuinely translated
    };
    bj::object root;
    root["sc01"] = sc01;

    auto report = snu::count_coverage(root, "doc.json");
    EXPECT_EQ(report.totals.translated, 2);
    EXPECT_EQ(report.totals.identity, 1);
    EXPECT_EQ(report.totals.exempt, 2);
    EXPECT_EQ(report.totals.total, 5);
    EXPECT_EQ(report.totals.translatable(), 3);

    std::string out = (fs::temp_directory_path() / "snu_ut_exempt.json").u8string();
    snu::write_coverage_report(report, out);
    auto totals = snu::json_parse_file(out).get_object().at("totals").get_object();
    EXPECT_EQ(totals.at("exempt").get_int64(), 2);
    EXPECT_EQ(totals.at("translatable").get_int64(), 3);
    EXPECT_EQ(totals.at("identity").get_int64(), 1);
    fs::remove(fs::u8path(out));
}

// Deployed-TSV gate: every non-empty line of the real translations.tsv is
// <jp>\t<en>.  Skips when the TSV hasn't been built yet.
TEST(BuildTsv, deployed_tsv_has_one_tab_per_line) {
    fs::path tsv = fs::u8path(std::string(SNU_PROJECT_DIR) + "\\translations.tsv");
    if (!fs::exists(tsv)) {
        GTEST_SKIP() << "translations.tsv not built yet";
    }
    std::ifstream f(tsv, std::ios::binary);
    std::string line;
    std::size_t ln = 0, bad = 0;
    while (std::getline(f, line) && bad < 5) {
        ++ln;
        if (line.empty()) continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::count(line.begin(), line.end(), '\t') != 1) {
            ++bad;
            GTEST_LOG_(INFO) << "bad line " << ln << ": " << line.substr(0, 60);
        }
    }
    EXPECT_EQ(bad, 0u);
}

// --- the box a chain of strings shares --------------------------------
//
// Strings chained by 0x0A share one box's lines, so overflow must be
// checked in draw order, not row by row.

namespace {

bt::DrawnSegment jp_segment(const std::string& jp) {
    bt::DrawnSegment s;
    s.jp = jp;
    return s;
}

// The runtime table as the DLL holds it: CP932 keys, folded CP932 values.
translator_logic::TranslationMap runtime_table(
    const std::vector<std::pair<std::string, std::string>>& rows) {
    std::string tsv;
    for (const auto& [jp, en] : rows)
        tsv += bt::esc(jp) + "\t" + bt::esc(en) + "\n";
    translator_logic::TranslationMap table;
    translator_logic::ParseTsvBuffer(tsv.data(), tsv.size(), table);
    return table;
}

}  // namespace

TEST(BoxBudget, one_string_per_box_uses_the_whole_box) {
    auto table = runtime_table({{u8"あ", std::string(200, 'x')}});
    bt::BoxBudgetReport r;
    bt::check_box_budgets("s", {jp_segment(u8"あ")}, table, r);
    EXPECT_EQ(r.boxes, 1u);
    EXPECT_EQ(r.chained_boxes, 0u);
    EXPECT_EQ(r.over_budget_boxes, 0u);
}

TEST(BoxBudget, a_chain_of_strings_shares_the_three_lines) {
    // Three strings, one box: the first two end in 0x0A, the third does not.
    auto table = runtime_table({{u8"あ\n", "one"},
                                {u8"い\n", "two"},
                                {u8"う", "three"}});
    bt::BoxBudgetReport r;
    bt::check_box_budgets(
        "s", {jp_segment(u8"あ\n"), jp_segment(u8"い\n"), jp_segment(u8"う")},
        table, r);
    EXPECT_EQ(r.boxes, 1u);
    EXPECT_EQ(r.chained_boxes, 1u);
    EXPECT_EQ(r.segments, 3u);
    EXPECT_EQ(r.over_budget_boxes, 0u);
}

TEST(BoxBudget, a_chain_that_runs_out_of_lines_is_reported) {
    // Two short values flow onto one line, and the third is longer than the
    // whole box -- so it would fit on its own measure only if the box were
    // empty, and the chain is what makes it overflow.  The length is written
    // against the budget rather than pinned, so the case stays an overflow
    // whatever the box is made to hold.
    const std::string long_value(
        translator_logic::SINGLE_LINE_MAX * (translator_logic::MAX_LINES + 1),
        'x');
    auto table = runtime_table({{u8"あ\n", "one"},
                                {u8"い\n", "two"},
                                {u8"う", long_value}});
    bt::BoxBudgetReport r;
    bt::check_box_budgets(
        "s", {jp_segment(u8"あ\n"), jp_segment(u8"い\n"), jp_segment(u8"う")},
        table, r);
    EXPECT_EQ(r.over_budget_boxes, 1u);
    EXPECT_EQ(r.over_budget_chained, 1u);
    EXPECT_EQ(r.truncated_segments, 1u);
    ASSERT_EQ(r.examples.size(), 1u);
    EXPECT_EQ(r.examples[0].lines_before, 1u)
        << "the first two values flowed onto one line";
    EXPECT_EQ(r.examples[0].segments, 3u);
}

TEST(BoxBudget, a_string_with_no_line_left_is_counted_as_dropped) {
    // Exactly the whole box, derived so a change of width cannot leave the
    // second string a line to draw in.
    const std::string fills_the_box(
        translator_logic::SINGLE_LINE_MAX +
            (translator_logic::MAX_LINES - 1) * translator_logic::WRAP_AT, 'x');
    auto table = runtime_table({{u8"あ\n", fills_the_box},
                                {u8"い", "cannot be drawn"}});
    bt::BoxBudgetReport r;
    bt::check_box_budgets("s", {jp_segment(u8"あ\n"), jp_segment(u8"い")},
                          table, r);
    EXPECT_EQ(r.dropped_segments, 1u);
    EXPECT_EQ(r.over_budget_boxes, 1u);
}

TEST(BoxBudget, the_name_plate_does_not_spend_the_dialogue_box) {
    bt::DrawnSegment name;
    name.jp = u8"直哉";
    name.is_name = true;
    auto table = runtime_table({{u8"あ\n", "one"},
                                {u8"直哉", "Naoya"},
                                {u8"い", std::string(130, 'x')}});
    bt::BoxBudgetReport r;
    bt::check_box_budgets("s", {jp_segment(u8"あ\n"), name, jp_segment(u8"い")},
                          table, r);
    EXPECT_EQ(r.over_budget_boxes, 0u)
        << "the name is a box of its own and must not use a dialogue line";
    EXPECT_EQ(r.boxes, 1u);
}

TEST(BoxBudget, an_untranslated_string_still_spends_its_lines) {
    // The engine draws the Japanese for a row the table does not have.
    // あ spends the first line, so a value needing all four overruns.
    const std::string needs_every_line(
        translator_logic::SINGLE_LINE_MAX +
            (translator_logic::MAX_LINES - 1) * translator_logic::WRAP_AT, 'x');
    auto table = runtime_table({{u8"い", needs_every_line}});
    bt::BoxBudgetReport r;
    bt::check_box_budgets("s", {jp_segment(u8"あ\n"), jp_segment(u8"い")},
                          table, r);
    EXPECT_EQ(r.over_budget_boxes, 1u);
    EXPECT_EQ(r.truncated_segments, 1u);
}

// The shipped table against the shipped extraction, walked in draw order.
// Skips when either is absent.
TEST(BoxBudget, the_deployed_table_fits_the_boxes_it_is_drawn_in) {
    const std::string root = SNU_PROJECT_DIR;
    fs::path tsv = fs::u8path(root + "/translations.tsv");
    fs::path doc = fs::u8path(root + "/script_output/translated_text.json");
    if (!fs::exists(tsv) || !fs::exists(doc))
        GTEST_SKIP() << "the table or the translated document is not built yet";

    auto table = bt::load_runtime_table(tsv.u8string());
    ASSERT_FALSE(table.empty());
    bt::BoxBudgetReport r = bt::check_box_budgets_of_document(doc.u8string(), table);

    std::cout << "[ NOTE     ] " << r.boxes << " message boxes, "
              << r.chained_boxes << " filled by more than one string; "
              << r.over_budget_boxes << " over budget ("
              << r.over_budget_chained << " of them chained)\n";
    for (const bt::BoxOverflow& o : r.examples)
        std::cout << "[ NOTE     ]   " << o.script << " segment "
                  << o.first_segment << " of " << o.segments
                  << ": box held " << o.lines_before << ", this drew "
                  << o.lines_drawn << "\n";

    // Nothing may be lost whole: a string with no line left draws nothing at
    // all, and the box gives the player no sign that it existed.
    EXPECT_EQ(r.dropped_segments, 0u);
    // A box filled by one string alone has the whole box, so a value that
    // does not fit one is too long, full stop -- no chain, no shared budget,
    // nothing to blame but the English.
    //
    // This is an alarm, not a property of the code.  A red line here means the
    // measurement found what it was built to find; it does not mean the wrap
    // regressed.  What it asks for is a surface with room for the English, or
    // -- where the value spends a line on a break of its own -- the break; the
    // table build already drops such a break when doing so is what makes the
    // box fit.  Cut-and-mark is the backstop behind both and not the answer.
    EXPECT_EQ(r.over_budget_boxes - r.over_budget_chained, 0u)
        << "single-string boxes whose English does not fit the box";
}

// --- The rows only the backlog reaches ---------------------------------
//
// The backlog redraws a whole box in one call, so the lookup key is the
// concatenation of all its strings.

// A string that ends in 0x0A hands its box on; a name plate is a box of its
// own and neither joins a chain nor closes one.
TEST(BoxBudget, chained_boxes_group_the_strings_the_backlog_runs_together) {
    std::vector<bt::DrawnSegment> script;
    auto add = [&](const char* jp, bool is_name = false) {
        bt::DrawnSegment s;
        s.jp = jp;
        s.is_name = is_name;
        script.push_back(std::move(s));
    };
    add("一つ目\n");
    add("直哉", /*is_name=*/true);   // drawn between the two, and transparent
    add("二つ目");
    add("単独の箱");
    add("三つの一\n");
    add("三つの二\n");
    add("三つの三");

    std::vector<bt::ChainedBox> boxes;
    bt::collect_chained_boxes(script, boxes);

    ASSERT_EQ(boxes.size(), 2u) << "a box of one string is already its own row";
    EXPECT_EQ(boxes[0], (bt::ChainedBox{"一つ目\n", "二つ目"}));
    EXPECT_EQ(boxes[1],
              (bt::ChainedBox{"三つの一\n", "三つの二\n", "三つの三"}));

    EXPECT_TRUE(bt::hands_the_box_on("続く\n"));
    EXPECT_FALSE(bt::hands_the_box_on("終わり"));
    EXPECT_FALSE(bt::hands_the_box_on(""));
}

// The key is the byte-for-byte concatenation the engine forms; the value is
// the English of the strings run together, the way the panel flows them.
TEST(BuildTsv, run_build_emits_the_whole_box_the_backlog_redraws) {
    bj::object first;
    first["text"] = "\xE3\x80\x80" "美しい桜。\n";
    first["translated"] = "Beautiful cherry blossoms.";
    bj::object second;
    second["text"] = "醜い死骸。";
    second["translated"] = "An ugly corpse.";
    bj::object script;
    script["strings"] = bj::array{first, second};
    bj::object root;
    root["sc01"] = script;

    std::string in = (fs::temp_directory_path() / "snu_ut_backlog_box.json").u8string();
    std::string out = (fs::temp_directory_path() / "snu_ut_backlog_box.tsv").u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    auto rows = read_tsv(out);
    // Read straight off the file, so the break inside the key is still the
    // escape that keeps the row one physical TSV line.
    const std::string key = "\xE3\x80\x80" "美しい桜。\\n醜い死骸。";
    ASSERT_EQ(rows.count(key), 1u) << "no row for the box the backlog redraws";
    EXPECT_EQ(rows.at(key).front(),
              "\xE3\x80\x80" "Beautiful cherry blossoms. An ugly corpse.");
    // The strings keep their own rows: the panel still draws them one call
    // each and looks each one up on its own.  The first keeps the break the
    // extraction gave it, which is the form the panel's lookup tries first.
    EXPECT_EQ(rows.at("\xE3\x80\x80" "美しい桜。\\n").front(),
              "\xE3\x80\x80" "Beautiful cherry blossoms.");
    EXPECT_EQ(rows.at("醜い死骸。").front(), "An ugly corpse.");

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
    fs::remove(fs::u8path(snu::coverage_report_path(in)));
}

// End to end: the DLL's table must match the CP932 buffer the engine hands
// over, through the parser and the substitution.
TEST(BuildTsv, the_backlog_key_matches_the_buffer_the_engine_hands_over) {
    bj::object first;
    first["text"] = "\xE3\x80\x80" "桜の森の下。\n";
    first["translated"] = "Beneath the cherry blossom forest.";
    bj::object second;
    second["text"] = "俺は今、あの絵を思い出した。\n";
    second["translated"] = "I recalled that painting just now.";
    bj::object third;
    third["text"] = "そして少しだけ考えた。";
    third["translated"] = "And I thought about it, briefly.";
    bj::object script;
    script["strings"] = bj::array{first, second, third};
    bj::object root;
    root["sc01"] = script;

    std::string in = (fs::temp_directory_path() / "snu_ut_backlog_wire.json").u8string();
    std::string out = (fs::temp_directory_path() / "snu_ut_backlog_wire.tsv").u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    namespace tl = translator_logic;
    const tl::TranslationMap table = bt::load_runtime_table(out);
    // The engine's own buffer: the three strings run together, in CP932.
    const std::string wire =
        tl::Utf8ToCp932("\xE3\x80\x80" "桜の森の下。\n") +
        tl::Utf8ToCp932("俺は今、あの絵を思い出した。\n") +
        tl::Utf8ToCp932("そして少しだけ考えた。");
    EXPECT_EQ(wire.size(), 66u) << "the byte count the engine measured";

    tl::BoxLineBudget budget;
    tl::Substitution sub;
    ASSERT_EQ(tl::BuildSubstitution(wire.c_str(), table, budget, /*box=*/1,
                                    tl::HISTORY_WINDOW, sub),
              tl::SubstitutionStatus::Substituted)
        << "the backlog would draw Japanese for a box the panel drew in English";
    EXPECT_FALSE(sub.truncated);
    EXPECT_LE(sub.lines, tl::HISTORY_MAX_LINES);
    EXPECT_NE(sub.bytes.find("Beneath the cherry blossom forest."),
              std::string::npos);
    EXPECT_NE(sub.bytes.find("And I thought about it, briefly."),
              std::string::npos);
    // The rectangle array is sized from the Japanese the caller measured, so
    // the English has to stay inside four rectangles per wire byte even
    // without the engine patch.
    EXPECT_LE(sub.bytes.size(),
              tl::DrawBudget(wire.size(), tl::RECTS_PER_WIRE_BYTE));

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
    fs::remove(fs::u8path(snu::coverage_report_path(in)));
}

// The strings run together as one paragraph, so a box that a line-per-string
// layout would overflow still reaches the backlog whole.
TEST(BuildTsv, run_build_runs_a_box_together_when_a_line_each_does_not_fit) {
    const std::string long_en =
        "A sentence deliberately written long enough that it fills most of one "
        "line of the box on its own";
    bj::object first;
    first["text"] = "一行目\n";
    first["translated"] = long_en + " first.";
    bj::object second;
    second["text"] = "二行目\n";
    second["translated"] = long_en + " second.";
    bj::object third;
    third["text"] = "三行目";
    third["translated"] = long_en + " third.";
    bj::object script;
    script["strings"] = bj::array{first, second, third};
    bj::object root;
    root["sc01"] = script;

    std::string in = (fs::temp_directory_path() / "snu_ut_backlog_flow.json").u8string();
    std::string out = (fs::temp_directory_path() / "snu_ut_backlog_flow.tsv").u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    auto rows = read_tsv(out);
    const std::string key = "一行目\\n二行目\\n三行目";
    ASSERT_EQ(rows.count(key), 1u);
    const std::string value = rows.at(key).front();
    EXPECT_EQ(value.find("\\n"), std::string::npos)
        << "a line each would have cost the third string its place";
    EXPECT_NE(value.find(" third."), std::string::npos)
        << "every string of the box still reaches the backlog";

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
    fs::remove(fs::u8path(snu::coverage_report_path(in)));
}

// A break inside the English is the writer's break for a Japanese line.
// English is wider, so the segment before it wrapped and left a stub line.
// The table flows every such break, whether or not the box could afford it;
// the key keeps its break, and so does a trailing one, which chains the box.
TEST(BuildTsv, run_build_flows_every_break_inside_the_english) {
    // Two sentences that fit the box's four lines together and need five with
    // a break between them.  The break ends the first line where the first
    // sentence ends, and the width the rest of that line would have held is
    // exactly what the second sentence runs out of.
    // One word past what MAX_LINES - 1 continuation lines hold, derived so the
    // fixture follows the wrap width instead of pinning one.
    const std::string first = "Short line.";
    const size_t kWordsPerLine = (translator_logic::WRAP_AT + 1) / 6;
    const size_t kWords = kWordsPerLine * (translator_logic::MAX_LINES - 1) + 1;
    std::string second;
    for (size_t i = 0; i < kWords; ++i) second += "wordy ";
    second.pop_back();
    bj::object overflows;
    overflows["text"] = "溢れる";
    overflows["translated"] = first + "\n" + second;
    bj::object fits;
    fits["text"] = "収まる";
    fits["translated"] = "One line.\nAnd a second.";
    bj::object keyed;
    keyed["text"] = "一行目\n二行目";
    keyed["translated"] = "First line\nsecond line";
    bj::object chained;
    chained["text"] = "続く\n";
    chained["translated"] = "Goes on.\n";
    bj::object script;
    script["strings"] = bj::array{overflows, fits, keyed, chained};
    bj::object root;
    root["sc01"] = script;

    std::string in = (fs::temp_directory_path() / "snu_ut_flow.json").u8string();
    std::string out = (fs::temp_directory_path() / "snu_ut_flow.tsv").u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    namespace tl = translator_logic;
    auto rows = read_tsv(out);
    ASSERT_EQ(rows.count("溢れる"), 1u);
    const std::string flowed = rows.at("溢れる").front();
    EXPECT_EQ(flowed.find("\\n"), std::string::npos)
        << "the break stayed and the box has to cut the sentence";
    EXPECT_NE(flowed.find(first), std::string::npos) << "no word may be lost";
    EXPECT_NE(flowed.find(second), std::string::npos) << "no word may be lost";
    EXPECT_EQ(flowed, first + " " + second);

    // And it now fits, drawn by the wrapper the game runs.
    tl::WrappedText w = tl::WrapForMessageBox(tl::EnglishToCp932(flowed),
                                              tl::MAX_LINES);
    EXPECT_FALSE(w.truncated);
    // A value the box could hold with its break flows all the same.
    EXPECT_EQ(rows.at("収まる").front(), "One line. And a second.");
    // The key keeps the break the engine passes; only the value flows.
    ASSERT_EQ(rows.count("一行目\\n二行目"), 1u);
    EXPECT_EQ(rows.at("一行目\\n二行目").front(), "First line second line");
    // A trailing break chains the box and is not layout.
    ASSERT_EQ(rows.count("続く\\n"), 1u);
    EXPECT_EQ(rows.at("続く\\n").front(), "Goes on.\\n");

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
    fs::remove(fs::u8path(snu::coverage_report_path(in)));
}
