// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-3 tests: escape correctness, the flatten/first-wins/cache-fill rules,
// and the deployed-TSV shape gate (skips when the TSV hasn't been built yet).
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <boost/json.hpp>

#include "build_tsv/build_tsv.h"
#include "common/util.h"

namespace fs = std::filesystem;
namespace bt = fav::build_tsv;
namespace bj = boost::json;
using namespace fav;

// The TSV escape MUST not let raw \t through -- that's the column separator;
// a leak corrupts the line-based parse on the DLL side.
TEST(BuildTsv, escape_function_strips_tab) {
    std::string out = bt::escape_for_tsv("a\tb");
    EXPECT_EQ(out.find('\t'), std::string::npos);
    EXPECT_EQ(out, "a\\tb");
}

TEST(BuildTsv, escape_function_full_set) {
    EXPECT_EQ(bt::escape_for_tsv("a\\b"), "a\\\\b");
    EXPECT_EQ(bt::escape_for_tsv("plain"), "plain");
    // Backslash first, then tab -- an escaped tab must not be double-escaped.
    EXPECT_EQ(bt::escape_for_tsv("a\\b\tc"), "a\\\\b\\tc");
}

TEST(BuildTsv, flatten_collapses_newlines_and_strips) {
    EXPECT_EQ(bt::flatten("a\r\nb"), "a b");
    EXPECT_EQ(bt::flatten("a\rb\nc"), "a b c");
    EXPECT_EQ(bt::flatten("  padded  "), "padded");
    // U+3000 padding on FVP name-window strings must be stripped too.
    EXPECT_EQ(bt::flatten("\xE3\x80\x80Kuro\xE3\x80\x80"), "Kuro");
    EXPECT_EQ(bt::flatten("\n\n"), "");
}

namespace {

// Writes a minimal translated_text.json and returns its path.
std::string write_translated(const std::string& path, bj::array lines) {
    bj::object script;
    script["lines"] = std::move(lines);
    bj::object root;
    root["Sakura.hcb"] = std::move(script);
    write_file(path, bj::serialize(bj::value(std::move(root))));
    return path;
}

bj::object line(const char* jp, const char* en) {
    bj::object o;
    o["content"] = jp;
    if (en) o["translated"] = en;
    return o;
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

}  // namespace

TEST(BuildTsv, skips_untranslated_and_keeps_first_on_conflict) {
    const std::string in = (fs::temp_directory_path() / "fav_ut_translated.json").u8string();
    const std::string cache = (fs::temp_directory_path() / "fav_ut_cache.json").u8string();
    const std::string out = (fs::temp_directory_path() / "fav_ut_table.tsv").u8string();
    fs::remove(fs::u8path(cache));

    write_translated(in, bj::array{
        line("\xE3\x81\x82", "Ah"),               // translated
        line("\xE3\x81\x84", "\xE3\x81\x84"),     // EN == JP -> skipped
        line("\xE3\x81\x86", nullptr),            // no translation -> skipped
        line("\xE3\x81\x82", "Aah"),              // conflict -> first kept
    });

    ASSERT_EQ(bt::run_build(in, cache, out), 0);
    auto rows = read_lines(out);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], "\xE3\x81\x82\tAh");

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

// translated_text.json can be stale if step 2 ran again without rebuilding
// it; the resume cache is the live source of truth.
TEST(BuildTsv, fills_from_cache_when_json_is_stale) {
    const std::string in = (fs::temp_directory_path() / "fav_ut_translated2.json").u8string();
    const std::string cache = (fs::temp_directory_path() / "fav_ut_cache2.json").u8string();
    const std::string out = (fs::temp_directory_path() / "fav_ut_table2.tsv").u8string();

    write_translated(in, bj::array{line("\xE3\x81\x82", "\xE3\x81\x82")});
    bj::object c;
    c["\xE3\x81\x82"] = "Ah";
    write_file(cache, bj::serialize(bj::value(std::move(c))));

    ASSERT_EQ(bt::run_build(in, cache, out), 0);
    auto rows = read_lines(out);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], "\xE3\x81\x82\tAh");

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(cache));
    fs::remove(fs::u8path(out));
}

TEST(BuildTsv, every_row_has_exactly_one_tab) {
    const std::string in = (fs::temp_directory_path() / "fav_ut_translated3.json").u8string();
    const std::string cache = (fs::temp_directory_path() / "fav_ut_cache3.json").u8string();
    const std::string out = (fs::temp_directory_path() / "fav_ut_table3.tsv").u8string();
    fs::remove(fs::u8path(cache));

    write_translated(in, bj::array{
        line("\xE3\x81\x82\tpadded", "with\ttab"),
        line("\xE3\x81\x84", "line\nbreak"),
    });

    ASSERT_EQ(bt::run_build(in, cache, out), 0);
    for (const auto& row : read_lines(out))
        EXPECT_EQ(std::count(row.begin(), row.end(), '\t'), 1) << row;

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

TEST(BuildTsv, resolves_trimmed_cache_key_for_padded_nameplate_and_skips_sprite_tags) {
    const std::string in = (fs::temp_directory_path() / "fav_ut_translated4.json").u8string();
    const std::string cache = (fs::temp_directory_path() / "fav_ut_cache4.json").u8string();
    const std::string out = (fs::temp_directory_path() / "fav_ut_table4.tsv").u8string();

    write_translated(in, bj::array{
        line("\xE3\x80\x80 ？？？ \xE3\x80\x80", "\xE3\x80\x80 ？？？ \xE3\x80\x80"),
        line("_私服めがね_吹出R", "_Casual glasses_Bubble R"),
        line("graph_bs/CHR_ましろ", "graph_bs/CHR_Mashiro"),
    });
    bj::object c;
    c["？？？"] = "???";
    write_file(cache, bj::serialize(bj::value(std::move(c))));

    ASSERT_EQ(bt::run_build(in, cache, out), 0);
    auto rows = read_lines(out);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], "？？？\t???");

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(cache));
    fs::remove(fs::u8path(out));
}

// Deployed-TSV gate: every non-empty line of the real translation_table.tsv
// is <jp>\t<en>.  Skips when the TSV hasn't been built yet.
TEST(BuildTsv, deployed_tsv_has_one_tab_per_line) {
    fs::path tsv = fs::u8path("C:\\Favorite\\translation_table.tsv");
    if (!fs::exists(tsv)) GTEST_SKIP() << "translation_table.tsv not built yet";
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
