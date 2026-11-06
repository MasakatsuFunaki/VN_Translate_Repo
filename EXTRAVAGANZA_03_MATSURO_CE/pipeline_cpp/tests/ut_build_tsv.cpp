// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Component tests for src/build_tsv/build_tsv.{h,cpp}: speaker merge, newline
// sanitising, word wrap, TSV escaping, and CP932 translation_table.tsv output.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>

#include <boost/json.hpp>

#include "build_tsv/build_tsv.h"
#include "common/util.h"

namespace fs = std::filesystem;
namespace bt = exm::build_tsv;
namespace bj = boost::json;
using namespace exm;

namespace {

constexpr std::size_t kLongFirstLineLen = 30;
constexpr int kTestWrapWidthShort = 7;
constexpr int kTestWrapWidthWide = 20;
constexpr int kTestWrapWidthNarrow = 4;
constexpr std::size_t kMinExpectedTsvBytes = 6;

std::string write_translated(const std::string& path, bj::array strings) {
    bj::object fd;
    fd["strings"] = std::move(strings);
    bj::object root;
    root["mushi.spt"] = std::move(fd);
    write_file(path, bj::serialize(bj::value(std::move(root))));
    return path;
}

bj::object mk(const char* jp, const char* en) {
    bj::object o;
    o["text"] = jp;
    if (en) o["translated"] = en;
    return o;
}

}  // namespace

TEST(BuildTsv, merge_speaker_line_folds_short_plate) {
    EXPECT_EQ(bt::merge_speaker_line("Miyaka\r\n\"Ah\""), "Miyaka: \"Ah\"");
    const std::string longish(kLongFirstLineLen, 'x');
    EXPECT_EQ(bt::merge_speaker_line(longish + "\r\nrest"), longish + "\r\nrest");
    EXPECT_EQ(bt::merge_speaker_line("\"Ah\"\r\nrest"), "\"Ah\"\r\nrest");
    EXPECT_EQ(bt::merge_speaker_line("no break"), "no break");
}

TEST(BuildTsv, sanitize_newlines_normalises_to_crlf) {
    EXPECT_EQ(bt::sanitize_newlines("a\nb"), "a\r\nb");
    EXPECT_EQ(bt::sanitize_newlines("a\r\nb"), "a\r\nb");
    EXPECT_EQ(bt::sanitize_newlines("\n\na"), "a");
    EXPECT_EQ(bt::sanitize_newlines("  a"), "  a");
}

TEST(BuildTsv, reflow_lines_joins_internal_breaks_and_preserves_trailing_crlf) {
    EXPECT_EQ(
        bt::word_wrap(bt::reflow_lines(
            "Mizuna, who had been fighting the Distortions as the Priestess of Genbu,\r\n"
            "had been captured by a man calling himself Shirou Kitabatake.\r\n")),
        "Mizuna, who had been fighting the Distortions as the\r\n"
        "Priestess of Genbu, had been captured by a man calling\r\n"
        "himself Shirou Kitabatake.\r\n");
    EXPECT_EQ(bt::reflow_lines("para1\r\n\r\npara2\r\n"), "para1\r\n\r\npara2\r\n");
    EXPECT_EQ(bt::reflow_lines("line1 \r\n  line2"), "line1 line2");
}

TEST(BuildTsv, word_wrap_breaks_on_spaces_and_keeps_segments) {
    const std::string wrapped = bt::word_wrap("aaa bbb ccc ddd", kTestWrapWidthShort);
    EXPECT_EQ(wrapped, "aaa bbb\r\nccc ddd");
    EXPECT_EQ(bt::word_wrap("short\r\nalso short", kTestWrapWidthWide), "short\r\nalso short");
    EXPECT_EQ(bt::word_wrap("abcdefghij", kTestWrapWidthNarrow), "abcdefghij");
}

TEST(BuildTsv, escape_covers_backslash_cr_lf_tab) {
    EXPECT_EQ(bt::escape_for_tsv("a\\b"), "a\\\\b");
    EXPECT_EQ(bt::escape_for_tsv("a\r\nb"), "a\\r\\nb");
    EXPECT_EQ(bt::escape_for_tsv("a\tb"), "a\\tb");
    EXPECT_EQ(bt::escape_for_tsv("plain"), "plain");
}

TEST(BuildTsv, output_is_cp932_encoded) {
    const std::string in = (fs::temp_directory_path() / "matsuro_ut_tr.json").u8string();
    const std::string out = (fs::temp_directory_path() / "matsuro_ut.tsv").u8string();

    write_translated(in, bj::array{
        mk("美弥香", "Miyaka"),
        mk("あ", "あ"),
        mk("い", nullptr),
    });

    ASSERT_EQ(bt::run_build(in, out), 0);
    const Bytes raw = read_file(out);
    ASSERT_GE(raw.size(), kMinExpectedTsvBytes);
    auto decoded = cp932_to_utf8_strict(raw);
    ASSERT_TRUE(decoded.has_value()) << "output is not valid CP932";
    EXPECT_EQ(*decoded, "美弥香\tMiyaka\n");

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

TEST(BuildTsv, every_row_has_exactly_one_tab) {
    const std::string in = (fs::temp_directory_path() / "matsuro_ut_tr2.json").u8string();
    const std::string out = (fs::temp_directory_path() / "matsuro_ut2.tsv").u8string();

    write_translated(in, bj::array{
        mk("あ\ttabbed", "with\ttab"),
        mk("い", "line\r\nbreak"),
    });

    ASSERT_EQ(bt::run_build(in, out), 0);
    const Bytes raw = read_file(out);
    std::string line;
    for (std::uint8_t b : raw) {
        if (b == '\n') {
            EXPECT_EQ(std::count(line.begin(), line.end(), '\t'), 1) << line;
            line.clear();
        } else {
            line += static_cast<char>(b);
        }
    }

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}
