// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for TSV builder escaping, flattening, conflict resolution, and
// cache backfill using synthetic temporary inputs.
#include "build_tsv/build_tsv.h"
#include "common/util.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/json.hpp>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
namespace bt = fav::build_tsv;
namespace bj = boost::json;
using namespace fav;

namespace {

constexpr int kSuccessExitCode = 0;
constexpr std::size_t kSingleRowCount = 1u;
constexpr std::ptrdiff_t kExpectedTabCount = 1;

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

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

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
    if (en != nullptr) {
        o["translated"] = en;
    }
    return o;
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
    // Backslash first, then tab -- an escaped tab must not be double-escaped.
    EXPECT_EQ(bt::escape_for_tsv("a\\b\tc"), "a\\\\b\\tc");
}

TEST(BuildTsv, flatten_collapses_newlines_and_strips) {
    EXPECT_EQ(bt::flatten("a\r\nb"), "a b");
    EXPECT_EQ(bt::flatten("a\rb\nc"), "a b c");
    EXPECT_EQ(bt::flatten("  padded  "), "padded");
    // U+3000 padding on FVP name-window strings must be stripped too.
    EXPECT_EQ(bt::flatten("　Kuro　"), "Kuro");
    EXPECT_EQ(bt::flatten("\n\n"), "");
}

TEST(BuildTsv, skips_untranslated_and_keeps_first_on_conflict) {
    const ScopedTempDir temp("fav_ut_build_tsv_conflict");
    const std::string in = (temp.path() / "translated.json").u8string();
    const std::string cache = (temp.path() / "cache.json").u8string();
    const std::string out = (temp.path() / "table.tsv").u8string();

    write_translated(in, bj::array{
        line("あ", "Ah"),
        line("い", "い"),
        line("う", nullptr),
        line("あ", "Aah"),
    });

    ASSERT_EQ(bt::run_build(in, cache, out), kSuccessExitCode);
    const auto rows = read_lines(out);
    ASSERT_EQ(rows.size(), kSingleRowCount);
    EXPECT_EQ(rows[0], "あ\tAh");
}

// translated_text.json can be stale when a resume run updates the cache without
// rebuilding the document; the resume cache is the live source of truth.
TEST(BuildTsv, fills_from_cache_when_json_is_stale) {
    const ScopedTempDir temp("fav_ut_build_tsv_stale");
    const std::string in = (temp.path() / "translated.json").u8string();
    const std::string cache = (temp.path() / "cache.json").u8string();
    const std::string out = (temp.path() / "table.tsv").u8string();

    write_translated(in, bj::array{line("あ", "あ")});
    bj::object c;
    c["あ"] = "Ah";
    write_file(cache, bj::serialize(bj::value(std::move(c))));

    ASSERT_EQ(bt::run_build(in, cache, out), kSuccessExitCode);
    const auto rows = read_lines(out);
    ASSERT_EQ(rows.size(), kSingleRowCount);
    EXPECT_EQ(rows[0], "あ\tAh");
}

TEST(BuildTsv, every_row_has_exactly_one_tab) {
    const ScopedTempDir temp("fav_ut_build_tsv_tabs");
    const std::string in = (temp.path() / "translated.json").u8string();
    const std::string cache = (temp.path() / "cache.json").u8string();
    const std::string out = (temp.path() / "table.tsv").u8string();

    write_translated(in, bj::array{
        line("あ\tpadded", "with\ttab"),
        line("い", "line\nbreak"),
    });

    ASSERT_EQ(bt::run_build(in, cache, out), kSuccessExitCode);
    for (const auto& row : read_lines(out)) {
        EXPECT_EQ(std::count(row.begin(), row.end(), '\t'), kExpectedTabCount) << row;
    }
}

TEST(BuildTsv, resolves_trimmed_cache_key_for_padded_nameplate_and_skips_sprite_tags) {
    const ScopedTempDir temp("fav_ut_build_tsv_nameplate");
    const std::string in = (temp.path() / "translated.json").u8string();
    const std::string cache = (temp.path() / "cache.json").u8string();
    const std::string out = (temp.path() / "table.tsv").u8string();

    write_translated(in, bj::array{
        line("　 ？？？ 　", "　 ？？？ 　"),
        line("_私服めがね_吹出R", "_Casual glasses_Bubble R"),
        line("graph_bs/CHR_ましろ", "graph_bs/CHR_Mashiro"),
    });
    bj::object c;
    c["？？？"] = "???";
    write_file(cache, bj::serialize(bj::value(std::move(c))));

    ASSERT_EQ(bt::run_build(in, cache, out), kSuccessExitCode);
    const auto rows = read_lines(out);
    ASSERT_EQ(rows.size(), kSingleRowCount);
    EXPECT_EQ(rows[0], "？？？\t???");
}
