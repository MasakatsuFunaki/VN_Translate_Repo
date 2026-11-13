// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// TSV builder unit tests: escape correctness, glossary name rows, coverage
// counting, box budgets, and backlog whole-box emission on synthetic fixtures.
#include "build_tsv/box_budget.h"
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "cache/glossary.h"
#include "common/coverage.h"
#include "common/util.h"
#include "translator_logic.h"

#include <boost/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace bt = snu::build_tsv;
namespace tr = snu::translate;
namespace bj = boost::json;

namespace {

const std::string IDEO = "　";

constexpr std::size_t kWordyTokenWidth = 6;
constexpr std::size_t kQuartileDivisor = 4;
constexpr std::size_t kMidpointDivisor = 2;
constexpr std::size_t kThreeQuarterNumerator = 3;

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

// jp -> list of en, in file order, from a built TSV.
std::map<std::string, std::vector<std::string>> read_tsv(const std::string& path) {
    std::map<std::string, std::vector<std::string>> rows;
    std::ifstream f(fs::u8path(path), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) {
            continue;
        }
        auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        rows[line.substr(0, tab)].push_back(line.substr(tab + 1));
    }
    return rows;
}

// Unique names across both glossary tables; each ships bare and in backlog brackets.
std::size_t glossary_names() {
    std::set<std::string> names;
    for (const auto* table : {&snu::translate::name_translations_ordered(),
                              &snu::translate::extra_name_translations()}) {
        for (const auto& [jp, en] : *table) {
            names.insert(jp);
        }
    }
    return names.size();
}

// The backlog's form of a name key: the engine's lenticular brackets round it.
std::string bracketed(const std::string& s) {
    return "【" + s + "】";
}

bt::DrawnSegment jp_segment(const std::string& jp) {
    bt::DrawnSegment s;
    s.jp = jp;
    return s;
}

// The runtime table as the DLL holds it: CP932 keys, folded CP932 values.
translator_logic::TranslationMap runtime_table(
    const std::vector<std::pair<std::string, std::string>>& rows) {
    std::string tsv;
    for (const auto& [jp, en] : rows) {
        tsv += bt::esc(jp) + "\t" + bt::esc(en) + "\n";
    }
    translator_logic::TranslationMap table;
    translator_logic::ParseTsvBuffer(tsv.data(), tsv.size(), table);
    return table;
}

std::vector<std::size_t> boundary_indices(std::size_t n) {
    if (n == 0) {
        return {};
    }
    const std::size_t q1 = n / kQuartileDivisor;
    const std::size_t mid = n / kMidpointDivisor;
    const std::size_t q3 = (kThreeQuarterNumerator * n) / kQuartileDivisor;
    const std::size_t candidates[] = {
        0,
        1,
        q1 > 0 ? q1 - 1 : 0,
        q1,
        q1 + 1,
        mid > 0 ? mid - 1 : 0,
        mid,
        mid + 1,
        q3 > 0 ? q3 - 1 : 0,
        q3,
        q3 + 1,
        n > 1 ? n - 2 : 0,
        n - 1,
    };
    std::set<std::size_t> unique;
    for (std::size_t idx : candidates) {
        if (idx < n) {
            unique.insert(idx);
        }
    }
    return {unique.begin(), unique.end()};
}

}  // namespace

TEST(BuildTsv, escape_function_strips_tab) {
    std::string out = bt::esc("a\tb");
    EXPECT_EQ(out.find('\t'), std::string::npos);
    EXPECT_EQ(out, "a\\tb");
}

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

    const std::string indented = "　一\n二\n三";
    auto three = tr::parse_numbered_response("1. One\\ntwo\\nthree\n", 1);
    ASSERT_EQ(three.size(), 1u);
    const std::string value = bt::normalize_leading_space(indented, three.at(1));
    EXPECT_EQ(value, "　One\ntwo\nthree");
    EXPECT_EQ(bt::esc(indented) + "\t" + bt::esc(value),
              "　一\\n二\\n三\t　One\\ntwo\\nthree");
}

TEST(BuildTsv, escape_function_full_set) {
    EXPECT_EQ(bt::esc("a\\b"), "a\\\\b");
    EXPECT_EQ(bt::esc("a\rb\nc"), "a\\rb\\nc");
    EXPECT_EQ(bt::esc("plain"), "plain");
}

TEST(BuildTsv, run_build_produces_one_tab_per_line) {
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

    const ScopedTempDir scratch("snu_ut_build_tabs");
    const std::string in =
        (scratch.path() / fs::u8path("translated.json")).u8string();
    const std::string out =
        (scratch.path() / fs::u8path("translations.tsv")).u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));

    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    std::ifstream f(fs::u8path(out), std::ios::binary);
    std::string line;
    std::size_t lines = 0;
    while (std::getline(f, line)) {
        if (line.empty()) {
            continue;
        }
        ++lines;
        EXPECT_EQ(std::count(line.begin(), line.end(), '\t'), 1)
            << "line has != 1 tab: " << line;
    }
    EXPECT_EQ(lines, 2u + snu::translate::name_translations_ordered().size() +
                         snu::translate::extra_name_translations().size() +
                         glossary_names());
}

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

    const ScopedTempDir scratch("snu_ut_markup");
    const std::string in =
        (scratch.path() / fs::u8path("markup.json")).u8string();
    const std::string out =
        (scratch.path() / fs::u8path("markup.tsv")).u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    const auto rows = read_tsv(out);
    ASSERT_EQ(rows.count("散る<r さくら>桜</r>"), 1u);
    EXPECT_EQ(rows.at("散る<r さくら>桜</r>")[0], "the falling cherry blossom");
    EXPECT_EQ(rows.at("残り")[0], "what is < left");
    EXPECT_EQ(rows.at("こんにちは")[0], "Hello");

    for (const auto& [jp, values] : rows) {
        for (const std::string& en : values) {
            const std::size_t open = en.find('<');
            if (open == std::string::npos) {
                continue;
            }
            EXPECT_EQ(en.find('>', open + 1), std::string::npos)
                << jp << " -> " << en;
        }
    }
}

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

    const ScopedTempDir scratch("snu_ut_names");
    const std::string in =
        (scratch.path() / fs::u8path("names.json")).u8string();
    const std::string out =
        (scratch.path() / fs::u8path("names.tsv")).u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    const auto rows = read_tsv(out);
    const auto& speakers = snu::translate::name_translations_ordered();
    const auto& extras = snu::translate::extra_name_translations();
    for (const auto* table : {&speakers, &extras}) {
        for (const auto& [jp, en] : *table) {
            ASSERT_EQ(rows.count(jp), 1u) << "missing glossary row for " << jp;
            EXPECT_EQ(rows.at(jp).size(), 1u) << "duplicate glossary row for " << jp;
            EXPECT_EQ(rows.at(jp).front(), en) << "wrong English for " << jp;
        }
    }
    for (const char* jp : {"直哉", "稟", "健一郎", "圭", "優美", "里奈",
                           "美和", "冬馬", "明石", "篠田", "弓子", "伯奇"}) {
        EXPECT_EQ(rows.count(jp), 1u) << "no row for cast name " << jp;
    }

    EXPECT_EQ(rows.count("未翻訳"), 0u);
    EXPECT_EQ(rows.at("こんにちは").front(), "Hello");
    EXPECT_EQ(rows.size(), speakers.size() + extras.size() + 1 + glossary_names());
    ASSERT_EQ(rows.count(bracketed("直哉")), 1u);
    EXPECT_EQ(rows.at(bracketed("直哉")).front(),
              bracketed(rows.at("直哉").front()))
        << "the backlog and the name plate have to spell the same person alike";
}

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

    const ScopedTempDir scratch("snu_ut_precedence");
    const std::string in =
        (scratch.path() / fs::u8path("precedence.json")).u8string();
    const std::string out =
        (scratch.path() / fs::u8path("precedence.tsv")).u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    const auto rows = read_tsv(out);
    ASSERT_EQ(rows.count(jp_name), 1u);
    EXPECT_EQ(rows.at(jp_name).size(), 1u) << "the glossary added a second row";
    EXPECT_EQ(rows.at(jp_name).front(), document_english);
    for (const auto* table : {&snu::translate::name_translations_ordered(),
                              &snu::translate::extra_name_translations()}) {
        for (const auto& [jp, en] : *table) {
            if (jp != jp_name) {
                EXPECT_EQ(rows.at(jp).front(), en) << jp;
            }
        }
    }
}

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

    EXPECT_EQ(bt::name_plates_sharing_english(
                  {{"甲", "X"}, {"乙", "X"}, {"丙", "X"}}),
              std::vector<std::string>{"甲 / 乙 / 丙 -> X"});

    const std::vector<std::pair<std::string, std::string>> clean = {
        {"圭", "Kei"}, {"直哉", "Naoya"}, {"老人", "Old Man"}};
    EXPECT_TRUE(bt::name_plates_sharing_english(clean).empty());
    EXPECT_TRUE(bt::name_plates_sharing_english({}).empty());
}

TEST(BuildTsv, no_two_glossary_names_are_spelled_alike) {
    std::vector<std::pair<std::string, std::string>> rows;
    for (const auto* table : {&tr::name_translations_ordered(),
                              &tr::extra_name_translations()}) {
        for (const auto& [jp, en] : *table) {
            rows.emplace_back(jp, en);
        }
    }

    const auto shared = bt::name_plates_sharing_english(rows);
    std::string listed;
    for (const std::string& s : shared) {
        listed += "\n  " + s;
    }
    EXPECT_TRUE(shared.empty())
        << shared.size() << " English name(s) stand for more than one "
        << "character:" << listed;
}

TEST(BuildTsv, glossary_tables_are_key_disjoint) {
    const auto& speakers = snu::translate::name_translations_ordered();
    const auto& extras = snu::translate::extra_name_translations();
    const auto& terms = snu::translate::terminology_translations();

    ASSERT_FALSE(speakers.empty());
    ASSERT_FALSE(extras.empty());
    ASSERT_FALSE(terms.empty());

    std::map<std::string, std::string> seen;
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

    EXPECT_EQ(seen.at("直哉"), "name_translations_ordered");
    EXPECT_EQ(seen.at("篠田"), "extra_name_translations");
    EXPECT_EQ(seen.at("横たわる櫻"), "terminology_translations");
    EXPECT_GE(seen.size(), speakers.size() + extras.size() + terms.size());
}

TEST(BuildTsv, terminology_never_becomes_a_runtime_row) {
    bj::object line;
    line["text"] = "こんにちは";
    line["translated"] = "Hello";
    bj::object script;
    script["strings"] = bj::array{line};
    bj::object root;
    root["sc01"] = script;

    const ScopedTempDir scratch("snu_ut_terms");
    const std::string in =
        (scratch.path() / fs::u8path("terms.json")).u8string();
    const std::string out =
        (scratch.path() / fs::u8path("terms.tsv")).u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    const auto rows = read_tsv(out);
    for (const auto& [jp, en] : snu::translate::terminology_translations()) {
        EXPECT_EQ(rows.count(jp), 0u) << "terminology leaked into the table: " << jp;
    }
}

TEST(BuildTsv, superseded_renderings_name_live_pins) {
    const auto& superseded = snu::translate::superseded_renderings();
    ASSERT_FALSE(superseded.empty());
    std::map<std::string, std::string> pins;
    for (const auto& [jp, en] : snu::translate::terminology_translations()) {
        pins.emplace(jp, en);
    }
    for (const auto& [jp, old_en] : superseded) {
        ASSERT_EQ(pins.count(jp), 1u) << jp << " is no longer a pinned term";
        EXPECT_NE(pins.at(jp), old_en)
            << jp << " lists its own current rendering as superseded";
        EXPECT_FALSE(old_en.empty());
    }
    EXPECT_EQ(pins.at("鳥谷"), "Toritani");
    EXPECT_EQ(pins.at("横たわる櫻"), "The Reclining Sakura");
}

TEST(BuildTsv, superseded_name_renderings_name_live_names) {
    const auto& superseded = snu::translate::superseded_name_renderings();
    ASSERT_FALSE(superseded.empty());
    std::map<std::string, std::string> names;
    for (const auto* table : {&snu::translate::name_translations_ordered(),
                              &snu::translate::extra_name_translations()}) {
        for (const auto& [jp, en] : *table) {
            names.emplace(jp, en);
        }
    }

    for (const auto& [jp, old_en] : superseded) {
        ASSERT_EQ(names.count(jp), 1u) << jp << " is not a name in either table";
        EXPECT_NE(names.at(jp), old_en)
            << jp << " lists its own current spelling as superseded";
        EXPECT_FALSE(old_en.empty());
    }
    EXPECT_EQ(names.at("吹"), "Sui");
    EXPECT_EQ(names.at("寧"), "Nei");
    EXPECT_EQ(names.at("宇綱"), "Ietsuna");
    EXPECT_EQ(names.at("直哉＆吹"), "Naoya & Sui");
}

TEST(BuildTsv, pinned_names_repair_the_row_and_the_prose_around_it) {
    EXPECT_EQ(bt::apply_pinned_terms("吹", "Fuki"), "Sui");
    EXPECT_EQ(bt::apply_pinned_terms("寧", "Yasushi"), "Nei");
    EXPECT_EQ(bt::apply_pinned_terms("宇綱", "Utsuna"), "Ietsuna");
    EXPECT_EQ(bt::apply_pinned_terms("直哉＆吹", "Naoya & Fuki"), "Naoya & Sui");

    EXPECT_EQ(bt::apply_pinned_terms("「思い出しました。吹です」",
                                     "\"I remember now. I am Fuki.\""),
              "\"I remember now. I am Sui.\"");
    EXPECT_EQ(bt::apply_pinned_terms("寧の飴。", "Yasushi's candy."), "Nei's candy.");

    EXPECT_EQ(bt::apply_pinned_terms("吹は笑った。", "Sui laughed."), "Sui laughed.");
    EXPECT_EQ(bt::apply_pinned_terms("朝が来た。", "Fuki laughed."), "Fuki laughed.");
    EXPECT_EQ(bt::apply_pinned_terms("吹は笑った。", "Fukiko laughed."),
              "Fukiko laughed.");
}

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

    const ScopedTempDir scratch("snu_ut_names_repair");
    const std::string in =
        (scratch.path() / fs::u8path("names_repair.json")).u8string();
    const std::string out =
        (scratch.path() / fs::u8path("names_repair.tsv")).u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    const auto rows = read_tsv(out);
    EXPECT_EQ(rows.at("吹").front(), "Sui");
    EXPECT_EQ(rows.at("寧").front(), "Nei");
    EXPECT_EQ(rows.at("宇綱").front(), "Ietsuna");
    EXPECT_EQ(rows.at("直哉＆吹").front(), "Naoya & Sui");
    EXPECT_EQ(rows.at(prose).front(), "\"I remember now. I am Sui.\"");
    EXPECT_EQ(rows.at("吹").size(), 1u);
}

TEST(BuildTsv, a_repair_that_contains_the_spelling_it_replaces_runs_once) {
    const std::string current = tr::name_translations().at("看護師");
    ASSERT_NE(current.find("Nurse"), std::string::npos)
        << "this test is about a replacement that carries the old spelling";

    const std::string once = bt::apply_pinned_terms("看護師", "Nurse");
    EXPECT_EQ(once, current);
    EXPECT_EQ(bt::apply_pinned_terms("看護師", once), once);
    EXPECT_EQ(bt::apply_pinned_terms("看護師", bt::apply_pinned_terms("看護師", once)),
              once);

    EXPECT_EQ(bt::apply_pinned_terms("看護師が来た", "The Nurse arrived."),
              "The " + current + " arrived.");
}

TEST(BuildTsv, pinned_terms_repair_only_what_the_japanese_vouches_for) {
    const std::string painting = "『横たわる櫻』";
    EXPECT_EQ(bt::apply_pinned_terms(painting + "を描いた男。",
                                     "The man who painted 'The Prostrate Cherry'."),
              "The man who painted 'The Reclining Sakura'.");
    EXPECT_EQ(bt::apply_pinned_terms(painting + "以外に。",
                                     "Beyond 'The Prostrate Cherry,' nothing."),
              "Beyond 'The Reclining Sakura,' nothing.");
    EXPECT_EQ(bt::apply_pinned_terms(painting + "を描いた男。",
                                     "The man who painted 'The Reclining Sakura'."),
              "The man who painted 'The Reclining Sakura'.");
    EXPECT_EQ(bt::apply_pinned_terms("朝が来た。", "Morning came."), "Morning came.");
    EXPECT_EQ(bt::apply_pinned_terms("", ""), "");
    EXPECT_EQ(bt::apply_pinned_terms("朝が来た。", "The Prostrate Cherry stood there."),
              "The Prostrate Cherry stood there.");

    EXPECT_EQ(bt::apply_pinned_terms("鳥谷は黙っていた。", "Toriya said nothing."),
              "Toritani said nothing.");
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷の絵。", "Toriya's painting."),
              "Toritani's painting.");
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷は黙っていた。", "Toriyama said nothing."),
              "Toriyama said nothing.");
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷は黙っていた。", "Ask Toriya, ask Toriya2."),
              "Ask Toritani, ask Toriya2.");
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷と鳥谷。", "Toriya and Toriya."),
              "Toritani and Toritani.");
}

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

    const ScopedTempDir scratch("snu_ut_repin");
    const std::string in =
        (scratch.path() / fs::u8path("repin.json")).u8string();
    const std::string out =
        (scratch.path() / fs::u8path("repin.tsv")).u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    const auto rows = read_tsv(out);
    EXPECT_EQ(rows.at(stale_jp).front(),
              "Beyond 'The Reclining Sakura,' that man never exhibited another.");
    EXPECT_EQ(rows.at(good_jp).front(), "The man who painted 'The Reclining Sakura'.");
    EXPECT_EQ(rows.at("朝が来た。").front(), "Morning came.");
    EXPECT_EQ(rows.at("直哉").front(), "Naoya");
}

TEST(BuildTsv, leading_space_is_copied_from_the_japanese_key) {
    EXPECT_EQ(bt::normalize_leading_space(IDEO + "桜が舞う。", "The blossoms scatter."),
              IDEO + "The blossoms scatter.");
    EXPECT_EQ(bt::normalize_leading_space(IDEO + "桜が舞う。", IDEO + "The blossoms scatter."),
              IDEO + "The blossoms scatter.");
    EXPECT_EQ(bt::normalize_leading_space("桜が舞う。", IDEO + "The blossoms scatter."),
              "The blossoms scatter.");
    EXPECT_EQ(bt::normalize_leading_space("桜が舞う。", "The blossoms scatter."),
              "The blossoms scatter.");
    EXPECT_EQ(bt::normalize_leading_space("「この美しさは本物である」",
                                          IDEO + "\"This beauty is real.\""),
              "\"This beauty is real.\"");
    EXPECT_EQ(bt::normalize_leading_space(IDEO + IDEO + "深い場所。", "A deep place."),
              IDEO + IDEO + "A deep place.");
    EXPECT_EQ(bt::normalize_leading_space("桜。", "  Cherry."), "Cherry.");
    EXPECT_EQ(bt::normalize_leading_space(IDEO + "桜。", " \tCherry."), IDEO + "Cherry.");
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
        entry(IDEO + "桜が舞う。", "The blossoms scatter."),
        entry("「本物である」", IDEO + "\"It is real.\""),
        entry(IDEO + "春の空。", IDEO + "The spring sky."),
        entry("朝が来た。", "Morning came."),
    };
    bj::object root;
    root["sc01"] = script;

    const ScopedTempDir scratch("snu_ut_indent");
    const std::string in =
        (scratch.path() / fs::u8path("indent.json")).u8string();
    const std::string out =
        (scratch.path() / fs::u8path("indent.tsv")).u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    const auto rows = read_tsv(out);
    EXPECT_EQ(rows.at(IDEO + "桜が舞う。").front(), IDEO + "The blossoms scatter.");
    EXPECT_EQ(rows.at("「本物である」").front(), "\"It is real.\"");
    EXPECT_EQ(rows.at(IDEO + "春の空。").front(), IDEO + "The spring sky.");
    EXPECT_EQ(rows.at("朝が来た。").front(), "Morning came.");
    EXPECT_EQ(rows.at("直哉").front(), "Naoya");
}

TEST(BuildTsv, coverage_counts_real_english_against_identity) {
    auto entry = [](const char* jp, const char* en) {
        bj::object o;
        o["text"] = jp;
        o["translated"] = en;
        return o;
    };
    bj::object bare;
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

    const auto report = snu::count_coverage(root, "some_document.json");
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

    const ScopedTempDir scratch("snu_ut_coverage");
    const std::string out =
        (scratch.path() / fs::u8path("coverage.json")).u8string();
    snu::write_coverage_report(report, out);
    const auto back = snu::json_parse_file(out).get_object();
    EXPECT_EQ(back.at("source_file").get_string(), "some_document.json");
    EXPECT_EQ(back.at("totals").get_object().at("translated").get_int64(), 2);
    EXPECT_EQ(back.at("totals").get_object().at("total").get_int64(), 4);
    EXPECT_EQ(back.at("scripts").get_object().at("sc02").get_object()
                  .at("translated").get_int64(), 1);
}

TEST(BuildTsv, coverage_separates_exempt_lines_from_real_misses) {
    auto entry = [](const char* jp, const char* en) {
        bj::object o;
        o["text"] = jp;
        o["translated"] = en;
        return o;
    };
    bj::object sc01;
    sc01["strings"] = bj::array{
        entry("こんにちは", "Hello"),
        entry("未翻訳", "未翻訳"),
        entry("　・・・", "　・・・"),
        entry("？？？", "？？？"),
        entry("「……」", "\"......\""),
    };
    bj::object root;
    root["sc01"] = sc01;

    const auto report = snu::count_coverage(root, "doc.json");
    EXPECT_EQ(report.totals.translated, 2);
    EXPECT_EQ(report.totals.identity, 1);
    EXPECT_EQ(report.totals.exempt, 2);
    EXPECT_EQ(report.totals.total, 5);
    EXPECT_EQ(report.totals.translatable(), 3);

    const ScopedTempDir scratch("snu_ut_exempt");
    const std::string out =
        (scratch.path() / fs::u8path("exempt.json")).u8string();
    snu::write_coverage_report(report, out);
    const auto totals =
        snu::json_parse_file(out).get_object().at("totals").get_object();
    EXPECT_EQ(totals.at("exempt").get_int64(), 2);
    EXPECT_EQ(totals.at("translatable").get_int64(), 3);
    EXPECT_EQ(totals.at("identity").get_int64(), 1);
}

// Strings chained by 0x0A share one box's lines, so overflow must be
// checked in draw order, not row by row.
TEST(BoxBudget, one_string_per_box_uses_the_whole_box) {
    auto table = runtime_table({{"あ", std::string(200, 'x')}});
    bt::BoxBudgetReport r;
    bt::check_box_budgets("s", {jp_segment("あ")}, table, r);
    EXPECT_EQ(r.boxes, 1u);
    EXPECT_EQ(r.chained_boxes, 0u);
    EXPECT_EQ(r.over_budget_boxes, 0u);
}

TEST(BoxBudget, a_chain_of_strings_shares_the_three_lines) {
    auto table = runtime_table({{"あ\n", "one"},
                                {"い\n", "two"},
                                {"う", "three"}});
    bt::BoxBudgetReport r;
    bt::check_box_budgets(
        "s", {jp_segment("あ\n"), jp_segment("い\n"), jp_segment("う")},
        table, r);
    EXPECT_EQ(r.boxes, 1u);
    EXPECT_EQ(r.chained_boxes, 1u);
    EXPECT_EQ(r.segments, 3u);
    EXPECT_EQ(r.over_budget_boxes, 0u);
}

TEST(BoxBudget, a_chain_that_runs_out_of_lines_is_reported) {
    const std::string long_value(
        translator_logic::SINGLE_LINE_MAX * (translator_logic::MAX_LINES + 1),
        'x');
    auto table = runtime_table({{"あ\n", "one"},
                                {"い\n", "two"},
                                {"う", long_value}});
    bt::BoxBudgetReport r;
    bt::check_box_budgets(
        "s", {jp_segment("あ\n"), jp_segment("い\n"), jp_segment("う")},
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
    const std::string fills_the_box(
        translator_logic::SINGLE_LINE_MAX +
            (translator_logic::MAX_LINES - 1) * translator_logic::WRAP_AT, 'x');
    auto table = runtime_table({{"あ\n", fills_the_box},
                                {"い", "cannot be drawn"}});
    bt::BoxBudgetReport r;
    bt::check_box_budgets("s", {jp_segment("あ\n"), jp_segment("い")},
                          table, r);
    EXPECT_EQ(r.dropped_segments, 1u);
    EXPECT_EQ(r.over_budget_boxes, 1u);
}

TEST(BoxBudget, the_name_plate_does_not_spend_the_dialogue_box) {
    bt::DrawnSegment name;
    name.jp = "直哉";
    name.is_name = true;
    auto table = runtime_table({{"あ\n", "one"},
                                {"直哉", "Naoya"},
                                {"い", std::string(130, 'x')}});
    bt::BoxBudgetReport r;
    bt::check_box_budgets("s", {jp_segment("あ\n"), name, jp_segment("い")},
                          table, r);
    EXPECT_EQ(r.over_budget_boxes, 0u)
        << "the name is a box of its own and must not use a dialogue line";
    EXPECT_EQ(r.boxes, 1u);
}

TEST(BoxBudget, an_untranslated_string_still_spends_its_lines) {
    const std::string needs_every_line(
        translator_logic::SINGLE_LINE_MAX +
            (translator_logic::MAX_LINES - 1) * translator_logic::WRAP_AT, 'x');
    auto table = runtime_table({{"い", needs_every_line}});
    bt::BoxBudgetReport r;
    bt::check_box_budgets("s", {jp_segment("あ\n"), jp_segment("い")},
                          table, r);
    EXPECT_EQ(r.over_budget_boxes, 1u);
    EXPECT_EQ(r.truncated_segments, 1u);
}

// The backlog redraws a whole box in one call, so the lookup key is the
// concatenation of all its strings.
TEST(BoxBudget, chained_boxes_group_the_strings_the_backlog_runs_together) {
    std::vector<bt::DrawnSegment> script;
    auto add = [&](const char* jp, bool is_name = false) {
        bt::DrawnSegment s;
        s.jp = jp;
        s.is_name = is_name;
        script.push_back(std::move(s));
    };
    add("一つ目\n");
    add("直哉", /*is_name=*/true);
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

TEST(BuildTsv, run_build_emits_the_whole_box_the_backlog_redraws) {
    bj::object first;
    first["text"] = "　美しい桜。\n";
    first["translated"] = "Beautiful cherry blossoms.";
    bj::object second;
    second["text"] = "醜い死骸。";
    second["translated"] = "An ugly corpse.";
    bj::object script;
    script["strings"] = bj::array{first, second};
    bj::object root;
    root["sc01"] = script;

    const ScopedTempDir scratch("snu_ut_backlog_box");
    const std::string in =
        (scratch.path() / fs::u8path("backlog_box.json")).u8string();
    const std::string out =
        (scratch.path() / fs::u8path("backlog_box.tsv")).u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    const auto rows = read_tsv(out);
    const std::string key = "　美しい桜。\\n醜い死骸。";
    ASSERT_EQ(rows.count(key), 1u) << "no row for the box the backlog redraws";
    EXPECT_EQ(rows.at(key).front(),
              "　Beautiful cherry blossoms. An ugly corpse.");
    EXPECT_EQ(rows.at("　美しい桜。\\n").front(),
              "　Beautiful cherry blossoms.");
    EXPECT_EQ(rows.at("醜い死骸。").front(), "An ugly corpse.");
}

TEST(BuildTsv, the_backlog_key_matches_the_buffer_the_engine_hands_over) {
    bj::object first;
    first["text"] = "　桜の森の下。\n";
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

    const ScopedTempDir scratch("snu_ut_backlog_wire");
    const std::string in =
        (scratch.path() / fs::u8path("backlog_wire.json")).u8string();
    const std::string out =
        (scratch.path() / fs::u8path("backlog_wire.tsv")).u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    namespace tl = translator_logic;
    const tl::TranslationMap table = bt::load_runtime_table(out);
    const std::string wire =
        tl::Utf8ToCp932("　桜の森の下。\n") +
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
    EXPECT_LE(sub.bytes.size(),
              tl::DrawBudget(wire.size(), tl::RECTS_PER_WIRE_BYTE));
}

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

    const ScopedTempDir scratch("snu_ut_backlog_flow");
    const std::string in =
        (scratch.path() / fs::u8path("backlog_flow.json")).u8string();
    const std::string out =
        (scratch.path() / fs::u8path("backlog_flow.tsv")).u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    const auto rows = read_tsv(out);
    const std::string key = "一行目\\n二行目\\n三行目";
    ASSERT_EQ(rows.count(key), 1u);
    const std::string value = rows.at(key).front();
    EXPECT_EQ(value.find("\\n"), std::string::npos)
        << "a line each would have cost the third string its place";
    EXPECT_NE(value.find(" third."), std::string::npos)
        << "every string of the box still reaches the backlog";
}

TEST(BuildTsv, run_build_flows_every_break_inside_the_english) {
    const std::string first = "Short line.";
    const std::size_t kWordsPerLine =
        (translator_logic::WRAP_AT + 1) / kWordyTokenWidth;
    const std::size_t kWords = kWordsPerLine * (translator_logic::MAX_LINES - 1) + 1;
    std::string second;
    for (std::size_t i = 0; i < kWords; ++i) {
        second += "wordy ";
    }
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

    const ScopedTempDir scratch("snu_ut_flow");
    const std::string in =
        (scratch.path() / fs::u8path("flow.json")).u8string();
    const std::string out =
        (scratch.path() / fs::u8path("flow.tsv")).u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    namespace tl = translator_logic;
    const auto rows = read_tsv(out);
    ASSERT_EQ(rows.count("溢れる"), 1u);
    const std::string flowed = rows.at("溢れる").front();
    EXPECT_EQ(flowed.find("\\n"), std::string::npos)
        << "the break stayed and the box has to cut the sentence";
    EXPECT_NE(flowed.find(first), std::string::npos) << "no word may be lost";
    EXPECT_NE(flowed.find(second), std::string::npos) << "no word may be lost";
    EXPECT_EQ(flowed, first + " " + second);

    const tl::WrappedText w = tl::WrapForMessageBox(tl::EnglishToCp932(flowed),
                                                    tl::MAX_LINES);
    EXPECT_FALSE(w.truncated);
    EXPECT_EQ(rows.at("収まる").front(), "One line. And a second.");
    ASSERT_EQ(rows.count("一行目\\n二行目"), 1u);
    EXPECT_EQ(rows.at("一行目\\n二行目").front(), "First line second line");
    ASSERT_EQ(rows.count("続く\\n"), 1u);
    EXPECT_EQ(rows.at("続く\\n").front(), "Goes on.\\n");
}

TEST(BuildTsv, updated_json_entries_at_boundary_positions_replace_tsv_rows) {
    static constexpr std::size_t kEntryCount = 9;
    auto make_root = [](const std::set<std::size_t>& updated_indices,
                        bool update_singleton) {
        bj::array strings;
        for (std::size_t i = 0; i < kEntryCount; ++i) {
            bj::object entry;
            entry["text"] = "境界テスト文_" + std::to_string(i);
            const bool is_updated = updated_indices.count(i) != 0;
            entry["translated"] =
                (is_updated ? "Updated boundary line " : "Initial line ") +
                std::to_string(i);
            strings.push_back(std::move(entry));
        }
        bj::object sc01;
        sc01["strings"] = std::move(strings);

        bj::object singleton_entry;
        singleton_entry["text"] = "単一スクリプト文";
        singleton_entry["translated"] =
            update_singleton ? "Updated singleton script." : "Initial singleton script.";
        bj::object sc02;
        sc02["strings"] = bj::array{std::move(singleton_entry)};

        bj::object root;
        root["sc01"] = std::move(sc01);
        root["sc02"] = std::move(sc02);
        return root;
    };

    const ScopedTempDir scratch("snu_ut_bva_update");
    const std::string in =
        (scratch.path() / fs::u8path("bva_update.json")).u8string();
    const std::string out =
        (scratch.path() / fs::u8path("bva_update.tsv")).u8string();

    snu::write_file(in, bj::serialize(bj::value(make_root({}, false))));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    const std::vector<std::size_t> bva = boundary_indices(kEntryCount);
    const std::set<std::size_t> updated(bva.begin(), bva.end());
    snu::write_file(in, bj::serialize(bj::value(make_root(updated, true))));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    const auto rows = read_tsv(out);
    for (std::size_t i = 0; i < kEntryCount; ++i) {
        const std::string jp = "境界テスト文_" + std::to_string(i);
        ASSERT_EQ(rows.count(jp), 1u) << "missing key at index " << i;
        const std::string expected =
            (updated.count(i) ? "Updated boundary line " : "Initial line ") +
            std::to_string(i);
        EXPECT_EQ(rows.at(jp).front(), expected) << "mismatch at index " << i;
    }
    ASSERT_EQ(rows.count("単一スクリプト文"), 1u);
    EXPECT_EQ(rows.at("単一スクリプト文").front(), "Updated singleton script.");
}

TEST(BoxBudget, load_runtime_table_and_document_audit_handle_japanese_paths) {
    bj::object first;
    first["text"] = "　桜の森の満開の下。\n";
    first["translated"] = "Beneath the cherry forest in full bloom.";
    bj::object second;
    second["text"] = "風びゅうびゅうと吹いていた。";
    second["translated"] = "The wind blew hard and cold.";
    bj::object script;
    script["strings"] = bj::array{first, second};
    bj::object root;
    root["sc_jp_path"] = script;

    const ScopedTempDir scratch("snu_ut_アマカノ_テスト");
    const fs::path nested = scratch.path() / fs::u8path("櫻の詩_データ");
    fs::create_directories(nested);

    const std::string in =
        (nested / fs::u8path("翻訳済み.json")).u8string();
    const std::string out =
        (nested / fs::u8path("translations.tsv")).u8string();
    snu::write_file(in, bj::serialize(bj::value(root)));
    ASSERT_EQ(bt::run_build(in, out, /*include_untranslated=*/false), 0);

    namespace tl = translator_logic;
    const tl::TranslationMap table = bt::load_runtime_table(out);
    ASSERT_FALSE(table.empty());

    const std::string cp932_second =
        tl::Utf8ToCp932("風びゅうびゅうと吹いていた。");
    ASSERT_EQ(table.count(cp932_second), 1u);
    EXPECT_EQ(table.at(cp932_second), "The wind blew hard and cold.");

    const bt::BoxBudgetReport report =
        bt::check_box_budgets_of_document(in, table);
    EXPECT_EQ(report.boxes, 1u);
    EXPECT_EQ(report.chained_boxes, 1u);
    EXPECT_EQ(report.boxes_with_english, 1u);
    EXPECT_EQ(report.over_budget_boxes, 0u);
}
