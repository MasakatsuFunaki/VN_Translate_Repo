// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-3 tests: the RAW key/value reconstruction (these three strings ARE the
// runtime lookup keys) and the utf-8-sig + CRLF output contract.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <boost/json.hpp>
#include <openssl/evp.h>

#include "build_tsv/build_tsv.h"
#include "common/util.h"

namespace fs = std::filesystem;
namespace bj = boost::json;
namespace bt = shin::build_tsv;
using namespace shin;

namespace {

bj::object mk(const char* type, const char* speaker, const char* text, const char* en) {
    bj::object o;
    o["offset"] = 0;
    o["type"] = type;
    o["speaker"] = speaker;
    o["text"] = text;
    if (en) o["translated"] = en;
    return o;
}

std::string write_fixture(const std::string& name, bj::array strings) {
    bj::object fd;
    fd["strings"] = std::move(strings);
    bj::object root;
    root["main05"] = std::move(fd);
    const std::string path = (fs::temp_directory_path() / name).u8string();
    write_file(path, bj::serialize(bj::value(std::move(root))));
    return path;
}

std::vector<std::string> tsv_rows(const std::string& path) {
    const Bytes raw = read_file(path);
    std::string s(reinterpret_cast<const char*>(raw.data()), raw.size());
    EXPECT_EQ(s.compare(0, 3, "\xEF\xBB\xBF"), 0) << "missing utf-8-sig BOM";
    s = s.substr(3);
    std::vector<std::string> rows;
    std::size_t pos = 0;
    while (pos < s.size()) {
        const std::size_t nl = s.find("\r\n", pos);
        if (nl == std::string::npos) break;
        rows.push_back(s.substr(pos, nl - pos));
        pos = nl + 2;
    }
    EXPECT_EQ(pos, s.size()) << "trailing bytes after the last CRLF";
    return rows;
}

std::string sha256_file(const std::string& path) {
    const Bytes raw = read_file(path);
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    EVP_Digest(raw.data(), raw.size(), md, &md_len, EVP_sha256(), nullptr);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < md_len; ++i) {
        out += hex[md[i] >> 4];
        out += hex[md[i] & 0xF];
    }
    return out;
}

}  // namespace

TEST(BuildTsv, escape_covers_tab_cr_lf_only) {
    EXPECT_EQ(bt::escape_for_tsv("a\tb"), "a{TAB}b");
    EXPECT_EQ(bt::escape_for_tsv("a\rb"), "a{CR}b");
    EXPECT_EQ(bt::escape_for_tsv("a\nb"), "a{LF}b");
    // A literal backslash-n is two ordinary characters and must survive: it is
    // the separator every dialogue and narration key is built around.
    EXPECT_EQ(bt::escape_for_tsv("Michael\\nHello"), "Michael\\nHello");
}

TEST(BuildTsv, raw_form_reconstruction_per_type) {
    const std::string in = write_fixture("shin_ut_bt1.json", bj::array{
        mk("choice", "", "屋上", "Go up to the roof"),
        mk("dialogue", "マイケル", "「あ」", "\"Ah\""),
        mk("narration", "", "そこには", "There was"),
        // A dialogue record whose speaker never resolved falls into the
        // narration branch, prefix and all.
        mk("dialogue", "", "誰か", "Someone"),
        // An unknown speaker passes through untranslated on the EN side.
        mk("dialogue", "知らない人", "「い」", "\"Ee\""),
    });
    const std::string out = (fs::temp_directory_path() / "shin_ut_bt1.tsv").u8string();
    ASSERT_EQ(bt::run_build(in, out), 0);

    const auto rows = tsv_rows(out);
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[0], "屋上\tGo up to the roof");
    EXPECT_EQ(rows[1], "マイケル\\n「あ」\tMichael\\n\"Ah\"");
    EXPECT_EQ(rows[2], "\\nそこには\t\\nThere was");
    EXPECT_EQ(rows[3], "\\n誰か\t\\nSomeone");
    EXPECT_EQ(rows[4], "知らない人\\n「い」\t知らない人\\n\"Ee\"");

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

TEST(BuildTsv, identical_rows_skipped_and_duplicates_deduped_first_wins) {
    const std::string in = write_fixture("shin_ut_bt2.json", bj::array{
        mk("choice", "", "帰る", "Go home"),
        mk("choice", "", "帰る", "Return"),          // duplicate key -> first wins
        mk("choice", "", "ABC", "ABC"),              // jp_raw == en_raw -> skipped
        mk("narration", "", "XYZ", "XYZ"),           // "\nXYZ" both sides -> skipped
        mk("choice", "", "untranslated", nullptr),   // no EN -> skipped silently
    });
    const std::string out = (fs::temp_directory_path() / "shin_ut_bt2.tsv").u8string();
    ASSERT_EQ(bt::run_build(in, out), 0);

    const auto rows = tsv_rows(out);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], "帰る\tGo home");

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

// The single most likely silent break in this port: it is the only utf-8-sig
// writer in the repo, and losing either half means every runtime lookup misses.
TEST(BuildTsv, output_has_bom_and_crlf_and_one_tab_per_row) {
    const std::string in = write_fixture("shin_ut_bt3.json", bj::array{
        mk("choice", "", "あ", "one"),
        mk("choice", "", "い", "two"),
    });
    const std::string out = (fs::temp_directory_path() / "shin_ut_bt3.tsv").u8string();
    ASSERT_EQ(bt::run_build(in, out), 0);

    const Bytes raw = read_file(out);
    ASSERT_GE(raw.size(), 3u);
    EXPECT_EQ(raw[0], 0xEF);
    EXPECT_EQ(raw[1], 0xBB);
    EXPECT_EQ(raw[2], 0xBF);
    // No bare LF anywhere.
    for (std::size_t i = 0; i < raw.size(); ++i)
        if (raw[i] == '\n') EXPECT_TRUE(i > 0 && raw[i - 1] == '\r') << "bare LF at " << i;
    for (const auto& row : tsv_rows(out))
        EXPECT_EQ(std::count(row.begin(), row.end(), '\t'), 1) << row;

    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

// END-TO-END over the real translated_text.json.
TEST(BuildTsv, golden_tsv_matches_reference_sha256) {
    const std::string translated =
        std::string(SHIN_PROJECT_DIR) + "\\script_output\\translated_text.json";
    if (!fs::exists(fs::u8path(translated))) GTEST_SKIP() << "no reference translation";

    const std::string out = (fs::temp_directory_path() / "shin_ut_golden.tsv").u8string();
    ASSERT_EQ(bt::run_build(translated, out), 0);
    EXPECT_EQ(fs::file_size(fs::u8path(out)), 6864701u);
    EXPECT_EQ(sha256_file(out),
              "a48daad072abf5d05f32b3d7489334dc86edd0a0ae28f55f5107ac8f324df70b");
    fs::remove(fs::u8path(out));
}
