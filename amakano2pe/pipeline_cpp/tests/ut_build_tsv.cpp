// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for build_tsv: TSV escaping and flattening rules, plus the $strNN
// substitution rules the runtime hook depends on.
#include "build_tsv/build_tsv.h"

#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace bt = ama::build_tsv;
namespace bj = boost::json;
using namespace ama;

namespace {

constexpr int kExitOk = 0;
constexpr std::size_t kExpectedFilteredRowCount = 3;
constexpr std::ptrdiff_t kExpectedTabsPerRow = 1;

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
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

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

bj::object mk_line(const char* type, const char* jp, const char* en) {
    bj::object o;
    o["type"] = type;
    o["content"] = jp;
    if (en != nullptr) {
        o["translated"] = en;
    }
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
        if (!l.empty() && l.back() == '\r') {
            l.pop_back();
        }
        if (!l.empty()) {
            out.push_back(l);
        }
    }
    return out;
}

}  // namespace

// The TSV escape MUST not let raw \t through -- that's the column separator;
// a leak corrupts the line-based parse on the DLL side.
TEST(BuildTsv, escape_function_strips_tab) {
    const std::string out = bt::escape_for_tsv("a\tb");
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
    EXPECT_EQ(bt::flatten("　padded　"), "padded");
    EXPECT_EQ(bt::flatten("\n\n"), "");
}

TEST(BuildTsv, keeps_only_message_and_name_and_first_on_conflict) {
    const ScopedTempDir temp("ama_ut_build_tsv_1");
    const std::string in = (temp.path() / "translated.json").u8string();
    const std::string out = (temp.path() / "table.tsv").u8string();

    write_translated(in, bj::array{
        mk_line("MESSAGE", "あ", "Ah"),
        mk_line("NAME", "玲", "Rei"),
        mk_line("COMMAND", "fw 0", "fw 0"),
        mk_line("MESSAGE", "い", "い"),
        mk_line("MESSAGE", "う", nullptr),
        mk_line("MESSAGE", "あ", "Aah"),
    });

    ASSERT_EQ(bt::run_build(in, out), kExitOk);
    const auto rows = read_lines(out);
    // 2 script rows + the injected runtime name 賢一.
    ASSERT_EQ(rows.size(), kExpectedFilteredRowCount);
    EXPECT_EQ(rows[0], "あ\tAh");
    EXPECT_EQ(rows[1], "玲\tRei");
    EXPECT_EQ(rows[2], "賢一\tKenichi") << "runtime name not injected";
}

// The engine resolves $str20 BEFORE the render hook, so a key that still
// contains the literal placeholder could never match at runtime.
TEST(BuildTsv, emits_substituted_sibling_for_str20) {
    const ScopedTempDir temp("ama_ut_build_tsv_2");
    const std::string in = (temp.path() / "translated.json").u8string();
    const std::string out = (temp.path() / "table.tsv").u8string();

    write_translated(in, bj::array{
        mk_line("MESSAGE", "$str20さん", "$str20-san"),
    });

    ASSERT_EQ(bt::run_build(in, out), kExitOk);
    const auto rows = read_lines(out);
    bool has_literal = false;
    bool has_resolved = false;
    for (const auto& r : rows) {
        if (r.rfind("$str20", 0) == 0) {
            has_literal = true;
        }
        if (r.rfind("賢一さん\t", 0) == 0) {
            has_resolved = true;
            EXPECT_EQ(r, "賢一さん\tKenichi-san");
        }
    }
    EXPECT_TRUE(has_literal) << "original row must be kept for safety";
    EXPECT_TRUE(has_resolved) << "resolved sibling row missing";
}

// $str10's runtime value is unknown, so a line containing it must NOT get a
// half-resolved sibling the engine would never produce.
TEST(BuildTsv, skips_substitution_when_an_unknown_var_is_present) {
    const ScopedTempDir temp("ama_ut_build_tsv_3");
    const std::string in = (temp.path() / "translated.json").u8string();
    const std::string out = (temp.path() / "table.tsv").u8string();

    write_translated(in, bj::array{
        mk_line("MESSAGE", "$str10$str20さん", "$str10 $str20-san"),
    });

    ASSERT_EQ(bt::run_build(in, out), kExitOk);
    bool has_original = false;
    for (const auto& r : read_lines(out)) {
        if (r.rfind("$str10$str20", 0) == 0) {
            has_original = true;
        }
        const bool mentions_str10 = r.find("$str10") != std::string::npos;
        const bool mentions_str20 = r.find("$str20") != std::string::npos;
        EXPECT_FALSE(mentions_str10 && !mentions_str20)
            << "half-resolved key emitted: " << r;
    }
    EXPECT_TRUE(has_original) << "original row must still be emitted verbatim";
}

TEST(BuildTsv, every_row_has_exactly_one_tab) {
    const ScopedTempDir temp("ama_ut_build_tsv_4");
    const std::string in = (temp.path() / "translated.json").u8string();
    const std::string out = (temp.path() / "table.tsv").u8string();

    write_translated(in, bj::array{
        mk_line("MESSAGE", "あ\ttabbed", "with\ttab"),
        mk_line("MESSAGE", "い", "line\nbreak"),
    });

    ASSERT_EQ(bt::run_build(in, out), kExitOk);
    for (const auto& row : read_lines(out)) {
        EXPECT_EQ(std::count(row.begin(), row.end(), '\t'), kExpectedTabsPerRow) << row;
    }
}

TEST(BuildTsv, strips_dangling_odd_trailing_backslash_while_preserving_escaped_pair) {
    const ScopedTempDir temp("ama_ut_build_tsv_5");
    const std::string in = (temp.path() / "translated.json").u8string();
    const std::string out = (temp.path() / "table.tsv").u8string();

    write_translated(in, bj::array{
        mk_line("MESSAGE", "肩が", "Our shoulders\\"),
        mk_line("MESSAGE", "パス", "C:\\\\"),
    });

    ASSERT_EQ(bt::run_build(in, out), kExitOk);
    const auto rows = read_lines(out);
    bool found_stripped = false;
    bool found_escaped_pair = false;
    for (const auto& row : rows) {
        if (row == "肩が\tOur shoulders") {
            found_stripped = true;
        }
        if (row == "パス\tC:\\\\\\\\") {
            found_escaped_pair = true;
        }
    }
    EXPECT_TRUE(found_stripped);
    EXPECT_TRUE(found_escaped_pair);
}

