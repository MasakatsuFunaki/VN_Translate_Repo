// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-3 tests: speaker merge, newline sanitising, word wrap, escaping, and
// the CP932 output contract the engine depends on.
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

TEST(BuildTsv, merge_speaker_line_folds_short_plate) {
    EXPECT_EQ(bt::merge_speaker_line("Miyaka\r\n\"Ah\""), "Miyaka: \"Ah\"");
    // A long first segment is prose, not a plate.
    const std::string longish(30, 'x');
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

TEST(BuildTsv, word_wrap_breaks_on_spaces_and_keeps_segments) {
    const std::string wrapped = bt::word_wrap("aaa bbb ccc ddd", 7);
    EXPECT_EQ(wrapped, "aaa bbb\r\nccc ddd");
    // Existing segment breaks survive.
    EXPECT_EQ(bt::word_wrap("short\r\nalso short", 20), "short\r\nalso short");
    // A single over-long word is emitted as-is rather than split.
    EXPECT_EQ(bt::word_wrap("abcdefghij", 4), "abcdefghij");
}

TEST(BuildTsv, escape_covers_backslash_cr_lf_tab) {
    EXPECT_EQ(bt::escape_for_tsv("a\\b"), "a\\\\b");
    EXPECT_EQ(bt::escape_for_tsv("a\r\nb"), "a\\r\\nb");
    EXPECT_EQ(bt::escape_for_tsv("a\tb"), "a\\tb");
    EXPECT_EQ(bt::escape_for_tsv("plain"), "plain");
}

namespace {

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

// The table is CP932, not UTF-8: the engine reads it directly.
TEST(BuildTsv, output_is_cp932_encoded) {
    const std::string in = (fs::temp_directory_path() / "exm_ut_tr.json").u8string();
    const std::string out = (fs::temp_directory_path() / "exm_ut.tsv").u8string();

    write_translated(in, bj::array{
        mk("\xE7\xBE\x8E\xE5\xBC\xA5\xE9\xA6\x99", "Miyaka"),  // 美弥香
        mk("\xE3\x81\x82", "\xE3\x81\x82"),                    // EN == JP -> skipped
        mk("\xE3\x81\x84", nullptr),                           // no EN -> skipped
    });

    ASSERT_EQ(bt::run_build(in, out), 0);
    const Bytes raw = read_file(out);
    // 美弥香 in CP932 is 94 FC 8D 76 8D 81 -- byte 0x94 would be part of a
    // 3-byte sequence if this had been written as UTF-8.
    ASSERT_GE(raw.size(), 6u);
    EXPECT_EQ(raw[0], 0x94);
    auto decoded = cp932_to_utf8_strict(raw);
    ASSERT_TRUE(decoded.has_value()) << "output is not valid CP932";
    EXPECT_EQ(*decoded, "\xE7\xBE\x8E\xE5\xBC\xA5\xE9\xA6\x99\tMiyaka\n");

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

TEST(BuildTsv, every_row_has_exactly_one_tab) {
    const std::string in = (fs::temp_directory_path() / "exm_ut_tr2.json").u8string();
    const std::string out = (fs::temp_directory_path() / "exm_ut2.tsv").u8string();

    write_translated(in, bj::array{
        mk("\xE3\x81\x82\ttabbed", "with\ttab"),
        mk("\xE3\x81\x84", "line\r\nbreak"),
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
