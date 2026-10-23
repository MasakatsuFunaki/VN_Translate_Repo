// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Table-builder tests: escape correctness, glossary name rows, the pin repair,
// indentation, the flow switch, the cache-fill fallback, and the deployed-TSV
// shape gate.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

namespace fs = std::filesystem;
namespace bt = snt::build_tsv;
namespace tr = snt::translate;
namespace bj = boost::json;
using namespace snt;

namespace {

const std::string IDEO = "\xE3\x80\x80";  // U+3000, the paragraph indent

bj::object mk(const std::string& jp, const std::string& en) {
    bj::object o;
    o["text"] = jp;
    o["translated"] = en;
    return o;
}

bj::object mk_name(const std::string& jp, const std::string& en) {
    bj::object o;
    o["text"] = jp;
    o["translated"] = en;
    o["type"] = "name";
    return o;
}

std::string write_translated(const std::string& path, bj::array strings) {
    bj::object script;
    script["strings"] = std::move(strings);
    bj::object root;
    root["script\\01_01.ast"] = std::move(script);
    write_file(path, bj::serialize(bj::value(std::move(root))));
    return path;
}

std::vector<std::string> read_lines(const std::string& p) {
    std::ifstream f(fs::u8path(p), std::ios::binary);
    std::vector<std::string> out;
    std::string l;
    while (std::getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty()) out.push_back(l);
    }
    return out;
}

// jp -> the values that key ships with, in file order.
std::map<std::string, std::vector<std::string>> read_tsv(const std::string& path) {
    std::map<std::string, std::vector<std::string>> rows;
    for (const std::string& line : read_lines(path)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        rows[line.substr(0, tab)].push_back(line.substr(tab + 1));
    }
    return rows;
}

// A temporary document, cache and table that clean themselves up.
struct Fixture {
    explicit Fixture(const std::string& stem)
        : doc((fs::temp_directory_path() / (stem + ".json")).u8string()),
          cache((fs::temp_directory_path() / (stem + ".cache.json")).u8string()),
          tsv((fs::temp_directory_path() / (stem + ".tsv")).u8string()) {
        fs::remove(fs::u8path(cache));
    }
    ~Fixture() {
        for (const std::string* p : {&doc, &cache, &tsv})
            fs::remove(fs::u8path(*p));
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    std::string doc, cache, tsv;
};

std::size_t glossary_names() { return tr::name_translations_ordered().size(); }

}  // namespace

// The TSV escape MUST not let raw \t through -- that's the column separator;
// a leak corrupts the line-based parse on the DLL side.
TEST(BuildTsv, escape_function_strips_tab) {
    std::string out = bt::esc("a\tb");
    EXPECT_EQ(out.find('\t'), std::string::npos);
    EXPECT_EQ(out, "a\\tb");
}

TEST(BuildTsv, escape_function_full_set) {
    EXPECT_EQ(bt::esc("a\\b"), "a\\\\b");
    EXPECT_EQ(bt::esc("a\rb\nc"), "a\\rb\\nc");
    EXPECT_EQ(bt::esc("plain"), "plain");
}

// A break travels through the cache into the row, and the row still has
// exactly one column separator and no physical line break.
TEST(BuildTsv, a_line_break_survives_from_the_cache_to_the_key) {
    const std::string jp = "一行目\n二行目";

    tr::Cache cache;
    cache.set(jp, "First line\nsecond line");
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
}

TEST(BuildTsv, skips_untranslated_and_last_wins_on_conflict) {
    Fixture f("snt_ut_tr");
    write_translated(f.doc, bj::array{
        mk("\xE3\x81\x82", "Ah"),
        mk("\xE3\x81\x84", "\xE3\x81\x84"),  // EN == JP -> skipped
        mk("\xE3\x81\x82", "Aah"),           // conflict -> LAST wins
    });

    ASSERT_EQ(bt::run_build(f.doc, f.cache, f.tsv, /*include_untranslated=*/false), 0);
    auto rows = read_tsv(f.tsv);
    ASSERT_EQ(rows.count("\xE3\x81\x82"), 1u);
    EXPECT_EQ(rows.at("\xE3\x81\x82").size(), 1u);
    EXPECT_EQ(rows.at("\xE3\x81\x82").front(), "Aah");
    EXPECT_EQ(rows.count("\xE3\x81\x84"), 0u);
}

TEST(BuildTsv, include_untranslated_keeps_jp_equals_en) {
    Fixture f("snt_ut_tr1");
    write_translated(f.doc, bj::array{mk("\xE3\x81\x84", "\xE3\x81\x84")});
    ASSERT_EQ(bt::run_build(f.doc, f.cache, f.tsv, /*include_untranslated=*/true), 0);
    EXPECT_EQ(read_tsv(f.tsv).count("\xE3\x81\x84"), 1u);
}

// translated_text.json can be stale if step 2 ran again without rebuilding
// it; the resume cache is the live source of truth.
TEST(BuildTsv, fills_from_cache_when_json_is_stale) {
    Fixture f("snt_ut_tr2");
    write_translated(f.doc, bj::array{mk("\xE3\x81\x82", "\xE3\x81\x82")});
    bj::object c;
    c["\xE3\x81\x82"] = "Ah";
    write_file(f.cache, bj::serialize(bj::value(std::move(c))));

    ASSERT_EQ(bt::run_build(f.doc, f.cache, f.tsv, false), 0);
    auto rows = read_tsv(f.tsv);
    ASSERT_EQ(rows.count("\xE3\x81\x82"), 1u);
    EXPECT_EQ(rows.at("\xE3\x81\x82").front(), "Ah");
}

TEST(BuildTsv, every_row_has_exactly_one_tab) {
    Fixture f("snt_ut_tr3");
    write_translated(f.doc, bj::array{
        mk("\xE3\x81\x82\ttabbed", "with\ttab"),
        mk("\xE3\x81\x84", "line\nbreak"),
    });

    ASSERT_EQ(bt::run_build(f.doc, f.cache, f.tsv, false), 0);
    for (const auto& row : read_lines(f.tsv))
        EXPECT_EQ(std::count(row.begin(), row.end(), '\t'), 1) << row;
}

// Every speaker plate needs a row whether or not the scripts draw it as a
// string, because the name window looks the plate up like any other text.
TEST(BuildTsv, run_build_emits_every_glossary_name_once) {
    Fixture f("snt_ut_names");
    write_translated(f.doc, bj::array{
        mk_name("直哉", "Naoya"),
        mk("未翻訳", "未翻訳"),      // identity -> still dropped
        mk("こんにちは", "Hello"),
    });
    ASSERT_EQ(bt::run_build(f.doc, f.cache, f.tsv, false), 0);

    auto rows = read_tsv(f.tsv);
    for (const auto& [jp, en] : tr::name_translations_ordered()) {
        ASSERT_EQ(rows.count(jp), 1u) << "missing glossary row for " << jp;
        EXPECT_EQ(rows.at(jp).size(), 1u) << "duplicate glossary row for " << jp;
        EXPECT_EQ(rows.at(jp).front(), en) << "wrong English for " << jp;
    }
    EXPECT_EQ(rows.count("未翻訳"), 0u);
    EXPECT_EQ(rows.at("こんにちは").front(), "Hello");
    // 直哉 is shared with the glossary, so only こんにちは adds a row.
    EXPECT_EQ(rows.size(), glossary_names() + 1);
}

// A name entry the glossary does not hold still ships, beside the glossary's.
TEST(BuildTsv, two_document_name_entries_ship_beside_the_glossary_names) {
    Fixture f("snt_ut_two_names");
    write_translated(f.doc, bj::array{
        mk_name("静流", "Shizuru"),
        mk_name("村人", "Villager"),   // not a glossary label
        mk("こんにちは", "Hello"),
    });
    ASSERT_EQ(bt::run_build(f.doc, f.cache, f.tsv, false), 0);

    auto rows = read_tsv(f.tsv);
    ASSERT_EQ(rows.count("静流"), 1u);
    EXPECT_EQ(rows.at("静流").front(), "Shizuru");
    ASSERT_EQ(rows.count("村人"), 1u);
    EXPECT_EQ(rows.at("村人").front(), "Villager");
    for (const auto& [jp, en] : tr::name_translations_ordered())
        EXPECT_EQ(rows.count(jp), 1u) << "missing glossary row for " << jp;
    // The glossary names, plus 村人 and こんにちは.
    EXPECT_EQ(rows.size(), glossary_names() + 2);
}

// When the document and the glossary disagree on a name, the document wins:
// it was answered with the scene in front of it.
TEST(BuildTsv, run_build_keeps_document_english_over_glossary) {
    const std::string jp_name = "直哉";
    const std::string document_english = "Naoya Kusanagi";
    const std::string glossary_english = tr::name_translations().at(jp_name);
    ASSERT_NE(document_english, glossary_english)
        << "the test needs the two sides to disagree";

    Fixture f("snt_ut_precedence");
    write_translated(f.doc, bj::array{mk_name(jp_name, document_english)});
    ASSERT_EQ(bt::run_build(f.doc, f.cache, f.tsv, false), 0);

    auto rows = read_tsv(f.tsv);
    ASSERT_EQ(rows.count(jp_name), 1u);
    EXPECT_EQ(rows.at(jp_name).size(), 1u) << "the glossary added a second row";
    EXPECT_EQ(rows.at(jp_name).front(), document_english);
    for (const auto& [jp, en] : tr::name_translations_ordered())
        if (jp != jp_name) EXPECT_EQ(rows.at(jp).front(), en) << jp;
}

// Two plates that draw alike are one character to the player.
TEST(BuildTsv, colliding_name_plates_are_reported_and_nothing_else_is) {
    const std::vector<std::pair<std::string, std::string>> rows = {
        {"圭", "Kei"},
        {"直哉", "Naoya"},
        {"女性教員", "Female Teacher"},
        {"女教師", "Female Teacher"},
        {"老人", "Old Man"},
    };
    const auto shared = bt::name_plates_sharing_english(rows);
    ASSERT_EQ(shared.size(), 1u);
    EXPECT_EQ(shared.front(), "女性教員 / 女教師 -> Female Teacher");

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

// The same check over the shipped table, where a clash is permanent.
TEST(BuildTsv, no_two_glossary_names_are_spelled_alike) {
    std::vector<std::pair<std::string, std::string>> rows;
    for (const auto& [jp, en] : tr::name_translations_ordered())
        rows.emplace_back(jp, en);

    const auto shared = bt::name_plates_sharing_english(rows);
    std::string listed;
    for (const std::string& s : shared) listed += "\n  " + s;
    EXPECT_TRUE(shared.empty())
        << shared.size() << " English name(s) stand for more than one "
        << "character:" << listed;
}

// The two tables serve different contracts: a name is a whole string the
// engine draws, a term is a fragment inside one.  A key in both would make the
// term a runtime row.
TEST(BuildTsv, glossary_tables_are_key_disjoint) {
    const auto& names = tr::name_translations_ordered();
    const auto& terms = tr::terminology_translations();
    ASSERT_FALSE(names.empty());
    ASSERT_FALSE(terms.empty());

    std::map<std::string, std::string> seen;  // key -> which table
    auto add = [&](const auto& table, const char* table_name) {
        for (const auto& [jp, en] : table) {
            auto [it, fresh] = seen.emplace(jp, table_name);
            EXPECT_TRUE(fresh) << jp << " is in both " << it->second << " and "
                               << table_name << "; it belongs in exactly one";
        }
    };
    add(names, "name_translations_ordered");
    add(terms, "terminology_translations");

    // Anchors: if a table is emptied or repurposed, the loops above go quiet
    // but these do not.
    EXPECT_EQ(seen.at("直哉"), "name_translations_ordered");
    EXPECT_EQ(seen.at("鳥谷"), "terminology_translations");
    EXPECT_EQ(seen.size(), names.size() + terms.size());
}

// Terminology is prompt-side only: it names fragments of a line, so a row keyed
// on one would answer for text the engine never hands over whole.
TEST(BuildTsv, terminology_never_becomes_a_runtime_row) {
    Fixture f("snt_ut_terms");
    write_translated(f.doc, bj::array{mk("こんにちは", "Hello")});
    ASSERT_EQ(bt::run_build(f.doc, f.cache, f.tsv, false), 0);

    auto rows = read_tsv(f.tsv);
    for (const auto& [jp, en] : tr::terminology_translations())
        EXPECT_EQ(rows.count(jp), 0u) << "terminology leaked into the table: " << jp;
}

// Every superseded rendering must name a live pin and an English it no longer
// uses, or the repair is a silent no-op.
TEST(BuildTsv, superseded_renderings_name_live_pins) {
    const auto& superseded = tr::superseded_renderings();
    ASSERT_FALSE(superseded.empty());
    std::map<std::string, std::string> pins;
    for (const auto& [jp, en] : tr::terminology_translations()) pins.emplace(jp, en);
    for (const auto& [jp, old_en] : superseded) {
        ASSERT_EQ(pins.count(jp), 1u) << jp << " is no longer a pinned term";
        EXPECT_NE(pins.at(jp), old_en)
            << jp << " lists its own current rendering as superseded";
        EXPECT_FALSE(old_en.empty());
    }
    // The correction the table exists for: the pilot table ships "Toriya".
    EXPECT_EQ(pins.at("鳥谷"), "Toritani");
}

// Same contract for the name table.
TEST(BuildTsv, superseded_name_renderings_name_live_names) {
    const auto& superseded = tr::superseded_name_renderings();
    ASSERT_FALSE(superseded.empty());
    const auto& names = tr::name_translations();

    for (const auto& [jp, old_en] : superseded) {
        ASSERT_EQ(names.count(jp), 1u) << jp << " is not a name in the table";
        EXPECT_NE(names.at(jp), old_en)
            << jp << " lists its own current spelling as superseded";
        EXPECT_FALSE(old_en.empty());
    }
    EXPECT_EQ(names.at("寧"), "Nei");
    EXPECT_EQ(names.at("女教師"), "Woman Teacher");
    // The English the split freed stays live on the other plate.
    EXPECT_EQ(names.at("女性教員"), "Female Teacher");
}

TEST(BuildTsv, pinned_terms_repair_only_what_the_japanese_vouches_for) {
    // The rendering the pin replaced, in a row whose source carries the term.
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷は黙っていた。", "Toriya said nothing."),
              "Toritani said nothing.");
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷の絵。", "Toriya's painting."),
              "Toritani's painting.");
    // Already correct -> untouched.  This is the common row.
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷の絵。", "Toritani's painting."),
              "Toritani's painting.");
    // Nothing pinned in the row at all -> byte-for-byte the same string.
    EXPECT_EQ(bt::apply_pinned_terms("朝が来た。", "Morning came."), "Morning came.");
    EXPECT_EQ(bt::apply_pinned_terms("", ""), "");
    // The old English without the Japanese to vouch for it is left alone: the
    // rewrite may not invent a correction the source does not support.
    EXPECT_EQ(bt::apply_pinned_terms("朝が来た。", "Toriya said nothing."),
              "Toriya said nothing.");
    // Whole terms only -- a longer word that merely starts the same survives.
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷は黙っていた。", "Toriyama said nothing."),
              "Toriyama said nothing.");
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷は黙っていた。", "Ask Toriya, ask Toriya2."),
              "Ask Toritani, ask Toriya2.");
    // Surrounding text is preserved exactly, including repeats.
    EXPECT_EQ(bt::apply_pinned_terms("鳥谷と鳥谷。", "Toriya and Toriya."),
              "Toritani and Toritani.");
}

// Pinned names are repaired in the name-window row and in the prose around it.
TEST(BuildTsv, pinned_names_repair_the_row_and_the_prose_around_it) {
    EXPECT_EQ(bt::apply_pinned_terms("寧", "Yasushi"), "Nei");
    EXPECT_EQ(bt::apply_pinned_terms("女教師", "Female Teacher"), "Woman Teacher");
    EXPECT_EQ(bt::apply_pinned_terms("寧の飴。", "Yasushi's candy."), "Nei's candy.");
    // The other plate keeps the English the split freed.
    EXPECT_EQ(bt::apply_pinned_terms("女性教員", "Female Teacher"), "Female Teacher");
    // Already correct -> untouched, which is every row of a fresh run.
    EXPECT_EQ(bt::apply_pinned_terms("寧は笑った。", "Nei laughed."), "Nei laughed.");
    // The old spelling without the Japanese to vouch for it is left alone.
    EXPECT_EQ(bt::apply_pinned_terms("朝が来た。", "Yasushi laughed."),
              "Yasushi laughed.");
}

// The table is rebuilt on every run, so a repair has to give the same answer
// the second time it sees its own output.
TEST(BuildTsv, a_repair_gives_the_same_answer_every_time_it_runs) {
    struct Row { std::string jp, en; };
    const std::vector<Row> rows = {
        {"鳥谷は黙っていた。", "Toriya said nothing."},
        {"寧の飴。", "Yasushi's candy."},
        {"女教師", "Female Teacher"},
    };
    for (const Row& r : rows) {
        const std::string once = bt::apply_pinned_terms(r.jp, r.en);
        EXPECT_NE(once, r.en) << r.jp << " was supposed to be repaired";
        EXPECT_EQ(bt::apply_pinned_terms(r.jp, once), once) << r.jp;
        EXPECT_EQ(bt::apply_pinned_terms(r.jp, bt::apply_pinned_terms(r.jp, once)),
                  once) << r.jp;
    }
    // No live pin replaces a spelling with one that contains it, which is the
    // case that would grow without the guard.  Prove the guard anyway.
    for (const auto& [jp, old_en] : tr::superseded_renderings())
        EXPECT_EQ(bt::apply_pinned_terms(jp, old_en),
                  bt::apply_pinned_terms(jp, bt::apply_pinned_terms(jp, old_en)));
}

// End to end: the shipped table carries the corrected spelling.
TEST(BuildTsv, run_build_ships_the_corrected_name) {
    const std::string prose = "「あ、いや、その前、鳥谷真琴って";
    Fixture f("snt_ut_repin");
    write_translated(f.doc, bj::array{
        mk(prose, "\"Ah, no -- before that, back when someone named Toriya Makoto"),
        mk("鳥谷の絵。", "Toriya's painting."),
        mk("朝が来た。", "Morning came."),
    });
    ASSERT_EQ(bt::run_build(f.doc, f.cache, f.tsv, false), 0);

    auto rows = read_tsv(f.tsv);
    EXPECT_EQ(rows.at(prose).front(),
              "\"Ah, no -- before that, back when someone named Toritani Makoto");
    EXPECT_EQ(rows.at("鳥谷の絵。").front(), "Toritani's painting.");
    EXPECT_EQ(rows.at("朝が来た。").front(), "Morning came.");
    EXPECT_EQ(rows.at("直哉").front(), "Naoya");  // name rows pass through
}

// The English carries the key's paragraph indent and no other.
TEST(BuildTsv, leading_space_is_copied_from_the_japanese_key) {
    // Indented key -> indented English.
    EXPECT_EQ(bt::normalize_leading_space(IDEO + "桜が舞う。", "The blossoms scatter."),
              IDEO + "The blossoms scatter.");
    // Already correct -> unchanged.
    EXPECT_EQ(bt::normalize_leading_space(IDEO + "桜が舞う。",
                                          IDEO + "The blossoms scatter."),
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
    Fixture f("snt_ut_indent");
    write_translated(f.doc, bj::array{
        mk(IDEO + "桜が舞う。", "The blossoms scatter."),        // must gain it
        mk("「本物である」", IDEO + "\"It is real.\""),          // must lose it
        mk(IDEO + "春の空。", IDEO + "The spring sky."),         // must keep it
        mk("朝が来た。", "Morning came."),                       // untouched
    });
    ASSERT_EQ(bt::run_build(f.doc, f.cache, f.tsv, false), 0);

    auto rows = read_tsv(f.tsv);
    EXPECT_EQ(rows.at(IDEO + "桜が舞う。").front(), IDEO + "The blossoms scatter.");
    EXPECT_EQ(rows.at("「本物である」").front(), "\"It is real.\"");
    EXPECT_EQ(rows.at(IDEO + "春の空。").front(), IDEO + "The spring sky.");
    EXPECT_EQ(rows.at("朝が来た。").front(), "Morning came.");
    // Glossary names carry no indent and must not acquire one.
    EXPECT_EQ(rows.at("直哉").front(), "Naoya");
}

// The switch decides layout, and its default decides nothing: the breaks the
// answer introduced stay in the table until the runtime asks otherwise.
TEST(BuildTsv, the_flow_switch_keeps_breaks_by_default_and_flows_when_asked) {
    EXPECT_EQ(bt::run_together("First line.\nSecond line."),
              "First line. Second line.");
    EXPECT_EQ(bt::run_together("First line.\n\r\nSecond line."),
              "First line. Second line.");
    EXPECT_EQ(bt::run_together("Ends here.\n"), "Ends here.\n")
        << "a run of breaks with nothing after it is not layout inside a sentence";
    EXPECT_EQ(bt::run_together("No break at all."), "No break at all.");

    const std::string jp = "続く";
    Fixture kept("snt_ut_flow_keep");
    write_translated(kept.doc, bj::array{mk(jp, "First line.\nSecond line.")});
    ASSERT_EQ(bt::run_build(kept.doc, kept.cache, kept.tsv, false), 0);
    EXPECT_EQ(read_tsv(kept.tsv).at(jp).front(), "First line.\\nSecond line.")
        << "the default keeps the break, escaped so the row stays one line";

    Fixture flowed("snt_ut_flow_on");
    write_translated(flowed.doc, bj::array{mk(jp, "First line.\nSecond line.")});
    ASSERT_EQ(bt::run_build(flowed.doc, flowed.cache, flowed.tsv, false,
                            /*flow_breaks=*/true), 0);
    auto rows = read_tsv(flowed.tsv);
    EXPECT_EQ(rows.at(jp).front(), "First line. Second line.");
    EXPECT_EQ(rows.at(jp).front().find("\\n"), std::string::npos)
        << "the break became a space, so the engine's own wrap places the lines";
}

// Deployed-TSV gate: every non-empty line of the real translations.tsv is
// <jp>\t<en>.  Skips when the TSV hasn't been built yet.
TEST(BuildTsv, deployed_tsv_has_one_tab_per_line) {
    fs::path tsv = fs::u8path(std::string(SNT_PROJECT_DIR) + "\\translations.tsv");
    if (!fs::exists(tsv)) GTEST_SKIP() << "translations.tsv not built yet";
    std::ifstream f(tsv, std::ios::binary);
    std::string line;
    std::size_t ln = 0, bad = 0;
    while (std::getline(f, line) && bad < 5) {
        ++ln;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (std::count(line.begin(), line.end(), '\t') != 1) {
            ++bad;
            GTEST_LOG_(INFO) << "bad line " << ln << ": " << line.substr(0, 60);
        }
    }
    EXPECT_EQ(bad, 0u);
}
