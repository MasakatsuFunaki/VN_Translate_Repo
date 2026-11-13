// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Runtime translation-table builder tests: speaker merge, newline sanitising,
// word wrap, TSV escaping, and the CP932 output contract.
#include "build_tsv/build_tsv.h"

#include "common/util.h"
#include "scoped_temp_dir.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace bt = mgi::build_tsv;
namespace bj = boost::json;
using namespace mgi;

namespace {

constexpr std::size_t kProseFirstSegmentLen = 30u;
constexpr int kNarrowWrapCols = 7;
constexpr int kWideWrapCols = 20;
constexpr int kTinyWrapCols = 4;
constexpr int kOverflowExtraLines = 4;
constexpr std::size_t kCrlfLength = 2u;
constexpr std::size_t kMiyakaCp932Bytes = 6u;
constexpr std::uint8_t kMiyakaFirstCp932LeadByte = 0x94u;
constexpr std::ptrdiff_t kExpectedTabsPerRow = 1;

std::string write_translated(const std::string& path, bj::array strings) {
    bj::object fd;
    fd["strings"] = std::move(strings);
    bj::object root;
    root["s100.spt"] = std::move(fd);
    write_file(path, bj::serialize(bj::value(std::move(root))));
    return path;
}

bj::object mk(const char* jp, const char* en) {
    bj::object o;
    o["text"] = jp;
    if (en) {
        o["translated"] = en;
    }
    return o;
}

}  // namespace

TEST(BuildTsv, merge_speaker_line_folds_short_plate) {
    EXPECT_EQ(bt::merge_speaker_line("Miyaka\r\n\"Ah\""), "Miyaka: \"Ah\"");
    // A long first segment is prose, not a plate.
    const std::string longish(kProseFirstSegmentLen, 'x');
    EXPECT_EQ(bt::merge_speaker_line(longish + "\r\nrest"), longish + "\r\nrest");
    // A quoted first segment is dialogue, not a plate.
    EXPECT_EQ(bt::merge_speaker_line("\"Ah\"\r\nrest"), "\"Ah\"\r\nrest");
    EXPECT_EQ(bt::merge_speaker_line("no break"), "no break");
}

TEST(BuildTsv, sanitize_newlines_normalises_to_crlf) {
    EXPECT_EQ(bt::sanitize_newlines("a\nb"), "a\r\nb");
    EXPECT_EQ(bt::sanitize_newlines("a\r\nb"), "a\r\nb");
    // Only leading '\n' characters are stripped -- not general whitespace.
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
    const std::string wrapped = bt::word_wrap("aaa bbb ccc ddd", kNarrowWrapCols);
    EXPECT_EQ(wrapped, "aaa bbb\r\nccc ddd");
    // Existing segment breaks survive.
    EXPECT_EQ(bt::word_wrap("short\r\nalso short", kWideWrapCols), "short\r\nalso short");
    // A single over-long word is emitted as-is rather than split.
    EXPECT_EQ(bt::word_wrap("abcdefghij", kTinyWrapCols), "abcdefghij");
}

// The engine caps a text block at MAX_LINES and would drop the overflow
// silently; we truncate here instead so the loss is at least deterministic.
TEST(BuildTsv, word_wrap_truncates_to_max_lines) {
    std::string many;
    for (int i = 0; i < bt::MAX_LINES + kOverflowExtraLines; ++i) {
        many += "line" + std::to_string(i) + "\r\n";
    }
    const std::string wrapped = bt::word_wrap(many, bt::MAX_LINE_CHARS);
    std::size_t lines = 1;
    for (std::size_t i = 0; i + 1 < wrapped.size(); ++i) {
        if (wrapped.compare(i, kCrlfLength, "\r\n") == 0) {
            ++lines;
        }
    }
    EXPECT_LE(lines, static_cast<std::size_t>(bt::MAX_LINES));
}

TEST(BuildTsv, escape_covers_backslash_cr_lf_tab) {
    EXPECT_EQ(bt::escape_for_tsv("a\\b"), "a\\\\b");
    EXPECT_EQ(bt::escape_for_tsv("a\r\nb"), "a\\r\\nb");
    EXPECT_EQ(bt::escape_for_tsv("a\tb"), "a\\tb");
    EXPECT_EQ(bt::escape_for_tsv("plain"), "plain");
}

// The table is CP932, not UTF-8: the engine reads it directly.
TEST(BuildTsv, output_is_cp932_encoded) {
    const test_util::ScopedTempDir temp_dir("mgi_ut_build_tsv_cp932");
    const std::string in = temp_dir.join_u8("tr.json");
    const std::string out = temp_dir.join_u8("out.tsv");

    write_translated(in, bj::array{
        mk("美弥香", "Miyaka"),
        mk("あ", "あ"),        // EN == JP -> skipped
        mk("い", nullptr),     // no EN -> skipped
    });

    ASSERT_EQ(bt::run_build(in, out), 0);
    const Bytes raw = read_file(out);
    // 美弥香 in CP932 is 94 FC 8D 76 8D 81 -- byte 0x94 would be part of a
    // 3-byte sequence if this had been written as UTF-8.
    ASSERT_GE(raw.size(), kMiyakaCp932Bytes);
    EXPECT_EQ(raw[0], kMiyakaFirstCp932LeadByte);
    auto decoded = cp932_to_utf8_strict(raw);
    ASSERT_TRUE(decoded.has_value()) << "output is not valid CP932";
    EXPECT_EQ(*decoded, "美弥香\tMiyaka\n");
}

TEST(BuildTsv, every_row_has_exactly_one_tab) {
    const test_util::ScopedTempDir temp_dir("mgi_ut_build_tsv_tabs");
    const std::string in = temp_dir.join_u8("tr2.json");
    const std::string out = temp_dir.join_u8("out2.tsv");

    write_translated(in, bj::array{
        mk("あ\ttabbed", "with\ttab"),
        mk("い", "line\r\nbreak"),
    });

    ASSERT_EQ(bt::run_build(in, out), 0);
    const Bytes raw = read_file(out);
    std::string line;
    for (std::uint8_t b : raw) {
        if (b == '\n') {
            EXPECT_EQ(std::count(line.begin(), line.end(), '\t'), kExpectedTabsPerRow) << line;
            line.clear();
            continue;
        }
        line += static_cast<char>(b);
    }
}
