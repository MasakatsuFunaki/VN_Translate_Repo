// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Runtime TSV builder unit tests: escape order, deduplication rules, glossary
// top-up, and LF line endings.
#include "build_tsv/build_tsv.h"
#include "cache/glossary.h"
#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace bj = boost::json;
namespace fs = std::filesystem;
namespace bt = crc::build_tsv;
using namespace crc;

namespace {

constexpr int kJsonIndent = 1;
constexpr int kBuildSuccessExitCode = 0;
constexpr int kBuildErrorExitCode = 1;
constexpr std::size_t kSingleDialogueRow = 1u;
constexpr std::size_t kMinDuplicateRows = 2u;

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
    write_file_text(in, json_pretty(bj::value(std::move(root)), kJsonIndent));
    // A game dir that does not exist keeps the deploy branch out of the test.
    EXPECT_EQ(bt::run_build(in, out, (s.dir / "no_such_game" / "translations.tsv").u8string()),
              kBuildSuccessExitCode);
    const Bytes raw = read_file(out);
    return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
}

std::vector<std::string> lines_of(const std::string& tsv) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < tsv.size()) {
        const std::size_t nl = tsv.find('\n', pos);
        if (nl == std::string::npos) {
            break;
        }
        out.push_back(tsv.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

std::size_t glossary_rows() {
    std::size_t n = 0;
    for (const auto& [jp, en] : crc::translate::name_translations_ordered()) {
        if (!jp.empty() && !en.empty() && jp != en) {
            ++n;
        }
    }
    return n;
}

}  // namespace

TEST(BuildTsv, EscapeOrderTable) {
    EXPECT_EQ(bt::esc("plain"), "plain");
    EXPECT_EQ(bt::esc("a\\b"), "a\\\\b");
    EXPECT_EQ(bt::esc("a\tb"), "a\\tb");
    EXPECT_EQ(bt::esc("a\rb"), "a\\rb");
    EXPECT_EQ(bt::esc("a\nb"), "a\\nb");
    EXPECT_EQ(bt::esc("日本\t語"), "日本\\t語");
    // Backslash is escaped first so subsequent escapes do not get doubled.
    EXPECT_EQ(bt::esc("\\\t"), "\\\\\\t");
}

TEST(BuildTsv, DropsPassthroughAndEmptyTranslations) {
    Scratch s;
    const std::string tsv = build(s, {{"あ", "あ"},
                                      {"い", ""},
                                      {"", "orphan"},
                                      {"う", "u"}});
    const auto rows = lines_of(tsv);
    ASSERT_EQ(rows.size(), kSingleDialogueRow + glossary_rows());
    EXPECT_EQ(rows[0], "う\tu");
}

TEST(BuildTsv, DuplicateJpLastWinsFirstPosition) {
    Scratch s;
    const std::string tsv = build(s, {{"あ", "first"},
                                      {"い", "middle"},
                                      {"あ", "last"}});
    const auto rows = lines_of(tsv);
    ASSERT_GE(rows.size(), kMinDuplicateRows);
    EXPECT_EQ(rows[0], "あ\tlast");
    EXPECT_EQ(rows[1], "い\tmiddle");
}

TEST(BuildTsv, NameTranslationsAppendedOnlyWhenAbsent) {
    Scratch s;
    const std::string tsv = build(s, {{"太一", "Taichi-from-dialogue"}});
    const auto rows = lines_of(tsv);
    ASSERT_EQ(rows.size(), kSingleDialogueRow + glossary_rows() - kSingleDialogueRow);
    EXPECT_EQ(rows[0], "太一\tTaichi-from-dialogue");
    EXPECT_EQ(rows[1], "見里\tMisato");
    for (const auto& r : rows) {
        EXPECT_NE(r, "太一\tTaichi");
    }
}

TEST(BuildTsv, TsvUsesLfNotCrlf) {
    Scratch s;
    const std::string tsv = build(s, {{"あ\r\nい", "a\r\nb"}});
    EXPECT_EQ(tsv.find('\r'), std::string::npos);
    EXPECT_NE(tsv.find("\\r\\n"), std::string::npos);
}

TEST(BuildTsv, MissingInputIsAnError) {
    Scratch s;
    EXPECT_EQ(bt::run_build((s.dir / "nope.json").u8string(), (s.dir / "o.tsv").u8string(),
                            (s.dir / "no_such_game" / "t.tsv").u8string()),
              kBuildErrorExitCode);
}
