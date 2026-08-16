// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-3 tests: TSV escaping and flattening rules, plus the $strNN
// substitution rules the runtime hook depends on.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <boost/json.hpp>

#include "build_tsv/build_tsv.h"
#include "common/util.h"

namespace fs = std::filesystem;
namespace bt = ama::build_tsv;
namespace bj = boost::json;
using namespace ama;

namespace {

bj::object mk_line(const char* type, const char* jp, const char* en) {
    bj::object o;
    o["type"] = type;
    o["content"] = jp;
    if (en) o["translated"] = en;
    return o;
}

std::string write_translated(const std::string& path, bj::array lines) {
    bj::object script;
    script["lines"] = std::move(lines);
    bj::object root;
    root["s01.cst"] = std::move(script);
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

}  // namespace

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
    EXPECT_EQ(bt::escape_for_tsv("a\\b\tc"), "a\\\\b\\tc");
}

// The engine renders one char at a time and crashes on ANY embedded newline.
TEST(BuildTsv, flatten_removes_every_newline_form) {
    EXPECT_EQ(bt::flatten("a\r\nb"), "a b");
    EXPECT_EQ(bt::flatten("a\rb\nc"), "a b c");
    EXPECT_EQ(bt::flatten("  padded  "), "padded");
    EXPECT_EQ(bt::flatten("\xE3\x80\x80padded\xE3\x80\x80"), "padded");
    EXPECT_EQ(bt::flatten("\n\n"), "");
}

TEST(BuildTsv, keeps_only_message_and_name_and_first_on_conflict) {
    const std::string in = (fs::temp_directory_path() / "ama_ut_tr.json").u8string();
    const std::string out = (fs::temp_directory_path() / "ama_ut_tbl.tsv").u8string();

    write_translated(in, bj::array{
        mk_line("MESSAGE", "\xE3\x81\x82", "Ah"),
        mk_line("NAME", "\xE7\x8E\xB2", "Rei"),
        mk_line("COMMAND", "fw 0", "fw 0"),          // wrong type -> skipped
        mk_line("MESSAGE", "\xE3\x81\x84", "\xE3\x81\x84"),  // EN == JP -> skipped
        mk_line("MESSAGE", "\xE3\x81\x86", nullptr),         // no EN -> skipped
        mk_line("MESSAGE", "\xE3\x81\x82", "Aah"),           // conflict -> first kept
    });

    ASSERT_EQ(bt::run_build(in, out), 0);
    auto rows = read_lines(out);
    // 2 script rows + the injected runtime name 賢一.
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0], "\xE3\x81\x82\tAh");
    EXPECT_EQ(rows[1], "\xE7\x8E\xB2\tRei");
    EXPECT_EQ(rows[2], "\xE8\xB3\xA2\xE4\xB8\x80\tKenichi") << "runtime name not injected";

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

// The engine resolves $str20 BEFORE the render hook, so a key that still
// contains the literal placeholder could never match at runtime.
TEST(BuildTsv, emits_substituted_sibling_for_str20) {
    const std::string in = (fs::temp_directory_path() / "ama_ut_tr2.json").u8string();
    const std::string out = (fs::temp_directory_path() / "ama_ut_tbl2.tsv").u8string();

    write_translated(in, bj::array{
        mk_line("MESSAGE", "$str20\xE3\x81\x95\xE3\x82\x93", "$str20-san"),
    });

    ASSERT_EQ(bt::run_build(in, out), 0);
    auto rows = read_lines(out);
    bool has_literal = false, has_resolved = false;
    for (const auto& r : rows) {
        if (r.rfind("$str20", 0) == 0) has_literal = true;
        if (r.rfind("\xE8\xB3\xA2\xE4\xB8\x80\xE3\x81\x95\xE3\x82\x93\t", 0) == 0) {
            has_resolved = true;
            EXPECT_EQ(r, "\xE8\xB3\xA2\xE4\xB8\x80\xE3\x81\x95\xE3\x82\x93\tKenichi-san");
        }
    }
    EXPECT_TRUE(has_literal) << "original row must be kept for safety";
    EXPECT_TRUE(has_resolved) << "resolved sibling row missing";

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

// $str10's runtime value is unknown, so a line containing it must NOT get a
// half-resolved sibling the engine would never produce.
TEST(BuildTsv, skips_substitution_when_an_unknown_var_is_present) {
    const std::string in = (fs::temp_directory_path() / "ama_ut_tr3.json").u8string();
    const std::string out = (fs::temp_directory_path() / "ama_ut_tbl3.tsv").u8string();

    write_translated(in, bj::array{
        mk_line("MESSAGE", "$str10$str20\xE3\x81\x95\xE3\x82\x93", "$str10 $str20-san"),
    });

    ASSERT_EQ(bt::run_build(in, out), 0);
    bool has_original = false;
    for (const auto& r : read_lines(out)) {
        if (r.rfind("$str10$str20", 0) == 0) has_original = true;
        // A half-resolved key would have $str20 replaced but $str10 left in.
        const bool mentions_str10 = r.find("$str10") != std::string::npos;
        const bool mentions_str20 = r.find("$str20") != std::string::npos;
        EXPECT_FALSE(mentions_str10 && !mentions_str20)
            << "half-resolved key emitted: " << r;
    }
    EXPECT_TRUE(has_original) << "original row must still be emitted verbatim";

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

TEST(BuildTsv, every_row_has_exactly_one_tab) {
    const std::string in = (fs::temp_directory_path() / "ama_ut_tr4.json").u8string();
    const std::string out = (fs::temp_directory_path() / "ama_ut_tbl4.tsv").u8string();

    write_translated(in, bj::array{
        mk_line("MESSAGE", "\xE3\x81\x82\ttabbed", "with\ttab"),
        mk_line("MESSAGE", "\xE3\x81\x84", "line\nbreak"),
    });

    ASSERT_EQ(bt::run_build(in, out), 0);
    for (const auto& row : read_lines(out))
        EXPECT_EQ(std::count(row.begin(), row.end(), '\t'), 1) << row;

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}
