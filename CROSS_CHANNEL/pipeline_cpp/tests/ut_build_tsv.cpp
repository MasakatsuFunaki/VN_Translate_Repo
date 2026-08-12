// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-3 tests: escape order, the dedup rules, glossary top-up, and the fact
// that the TSV is the ONE file in the pipeline written with LF rather than
// CRLF.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "translate/glossary.h"

namespace bj = boost::json;
namespace fs = std::filesystem;
namespace bt = crc::build_tsv;
using namespace crc;

namespace {

struct Scratch {
    fs::path dir;
    Scratch() {
        static int seq = 0;
        dir = fs::temp_directory_path() / fs::u8path("crc_ut_tsv_" + std::to_string(++seq));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Scratch() { fs::remove_all(dir); }
};

// Writes a minimal translated_text.json and runs the builder; returns the TSV.
std::string build(const Scratch& s, const std::vector<std::pair<std::string, std::string>>& rows) {
    bj::array strings;
    for (const auto& [jp, en] : rows) {
        bj::object o;
        o["offset"] = 0;
        o["type"] = "dialogue";
        o["speaker"] = "";
        o["text"] = jp;
        o["translated"] = en;
        strings.push_back(std::move(o));
    }
    bj::object sn;
    sn["strings"] = std::move(strings);
    bj::object root;
    root["sn.bin"] = std::move(sn);

    const std::string in = (s.dir / "translated_text.json").u8string();
    const std::string out = (s.dir / "translations.tsv").u8string();
    write_file_text(in, json_pretty(bj::value(std::move(root)), 1));
    // A game dir that does not exist keeps the deploy branch out of the test.
    EXPECT_EQ(bt::run_build(in, out, (s.dir / "no_such_game" / "translations.tsv").u8string()), 0);
    const Bytes raw = read_file(out);
    return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
}

std::vector<std::string> lines_of(const std::string& tsv) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < tsv.size()) {
        const std::size_t nl = tsv.find('\n', pos);
        if (nl == std::string::npos) break;
        out.push_back(tsv.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

std::size_t glossary_rows() {
    std::size_t n = 0;
    for (const auto& [jp, en] : crc::translate::name_translations_ordered())
        if (!jp.empty() && !en.empty() && jp != en) ++n;
    return n;
}

}  // namespace

TEST(BuildTsv, EscapeOrderTable) {
    EXPECT_EQ(bt::esc("plain"), "plain");
    EXPECT_EQ(bt::esc("a\\b"), "a\\\\b");
    EXPECT_EQ(bt::esc("a\tb"), "a\\tb");
    EXPECT_EQ(bt::esc("a\rb"), "a\\rb");
    EXPECT_EQ(bt::esc("a\nb"), "a\\nb");
    EXPECT_EQ(bt::esc("\xE6\x97\xA5\xE6\x9C\xAC\t\xE8\xAA\x9E"),
              "\xE6\x97\xA5\xE6\x9C\xAC\\t\xE8\xAA\x9E");
    // Backslash MUST be escaped first: otherwise the \t escape's own
    // backslash gets doubled by a later pass.
    EXPECT_EQ(bt::esc("\\\t"), "\\\\\\t");
}

TEST(BuildTsv, DropsPassthroughAndEmptyTranslations) {
    Scratch s;
    const std::string tsv = build(s, {{"\xE3\x81\x82", "\xE3\x81\x82"},  // en == jp
                                      {"\xE3\x81\x84", ""},              // empty en
                                      {"", "orphan"},                    // empty jp
                                      {"\xE3\x81\x86", "u"}});
    const auto rows = lines_of(tsv);
    ASSERT_EQ(rows.size(), 1u + glossary_rows());
    EXPECT_EQ(rows[0], "\xE3\x81\x86\tu");
}

TEST(BuildTsv, DuplicateJpLastWinsFirstPosition) {
    Scratch s;
    const std::string tsv = build(s, {{"\xE3\x81\x82", "first"},
                                      {"\xE3\x81\x84", "middle"},
                                      {"\xE3\x81\x82", "last"}});
    const auto rows = lines_of(tsv);
    ASSERT_GE(rows.size(), 2u);
    EXPECT_EQ(rows[0], "\xE3\x81\x82\tlast");    // value updated ...
    EXPECT_EQ(rows[1], "\xE3\x81\x84\tmiddle");  // ... position kept
}

TEST(BuildTsv, NameTranslationsAppendedOnlyWhenAbsent) {
    Scratch s;
    // 太一 already has a dialogue translation -- it must NOT be overwritten by
    // the glossary, and must not be appended a second time.
    const std::string tsv = build(s, {{"\xE5\xA4\xAA\xE4\xB8\x80", "Taichi-from-dialogue"}});
    const auto rows = lines_of(tsv);
    ASSERT_EQ(rows.size(), 1u + glossary_rows() - 1u);
    EXPECT_EQ(rows[0], "\xE5\xA4\xAA\xE4\xB8\x80\tTaichi-from-dialogue");
    // Glossary rows follow, in glossary order, starting after 太一.
    EXPECT_EQ(rows[1], "\xE8\xA6\x8B\xE9\x87\x8C\tMisato");  // 見里
    for (const auto& r : rows) EXPECT_NE(r, "\xE5\xA4\xAA\xE4\xB8\x80\tTaichi");
}

TEST(BuildTsv, TsvUsesLfNotCrlf) {
    Scratch s;
    // A literal CR in the source becomes the two characters '\' 'r', never a
    // raw 0x0D -- so the produced file must contain no 0x0D at all.
    const std::string tsv = build(s, {{"\xE3\x81\x82\r\n\xE3\x81\x84", "a\r\nb"}});
    EXPECT_EQ(tsv.find('\r'), std::string::npos);
    EXPECT_NE(tsv.find("\\r\\n"), std::string::npos);
}

TEST(BuildTsv, MissingInputIsAnError) {
    Scratch s;
    EXPECT_EQ(bt::run_build((s.dir / "nope.json").u8string(), (s.dir / "o.tsv").u8string(),
                            (s.dir / "no_such_game" / "t.tsv").u8string()),
              1);
}

// End-to-end against the committed 11 MB translated_text.json.
TEST(BuildTsv, RealTranslatedJsonProducesReferenceTsv) {
    const fs::path in =
        fs::u8path(std::string(CRC_PROJECT_DIR) + "\\script_output\\translated_text.json");
    if (!fs::exists(in)) GTEST_SKIP() << "translated_text.json not present";

    Scratch s;
    const std::string out = (s.dir / "translations.tsv").u8string();
    ASSERT_EQ(bt::run_build(in.u8string(), out, (s.dir / "no_such_game" / "t.tsv").u8string()), 0);

    const Bytes raw = read_file(out);
    EXPECT_EQ(raw.size(), 4066022u);
    std::size_t rows = 0;
    for (std::uint8_t b : raw) {
        EXPECT_NE(b, 0x0D);
        if (b == '\n') ++rows;
    }
    EXPECT_EQ(rows, 43256u);
}
