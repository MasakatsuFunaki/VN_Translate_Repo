// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Coverage of the translated document.  The document keeps the Japanese in
// `translated` for a line nothing answered for, so a run that lost half its
// answers still writes a complete-looking file; these counts are the only
// thing that tells the two apart.
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>

#include <boost/json.hpp>

#include "common/coverage.h"
#include "common/util.h"

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

bj::object entry(const char* jp, const char* en) {
    bj::object o;
    o["text"] = jp;
    o["translated"] = en;
    return o;
}

}  // namespace

// A line of punctuation and symbols translates to itself, so it can never
// register as translated.  The purge cannot tell it from a failure, which is
// why coverage counts it apart instead.
TEST(Coverage, the_exempt_predicate_fires_on_symbols_only) {
    EXPECT_TRUE(snt::is_exempt_line("？？？"));
    EXPECT_TRUE(snt::is_exempt_line("……"));
    EXPECT_TRUE(snt::is_exempt_line("「……」"));
    EXPECT_TRUE(snt::is_exempt_line("　・・・"));
    EXPECT_TRUE(snt::is_exempt_line("――"));

    EXPECT_FALSE(snt::is_exempt_line(""));            // nothing to exempt
    EXPECT_FALSE(snt::is_exempt_line("こんにちは"));  // kana
    EXPECT_FALSE(snt::is_exempt_line("桜"));          // kanji
    EXPECT_FALSE(snt::is_exempt_line("「あ……」"));   // one kana is enough
    EXPECT_FALSE(snt::is_exempt_line("OK"));          // latin
    EXPECT_FALSE(snt::is_exempt_line("Ａ"));          // fullwidth latin
    EXPECT_FALSE(snt::is_exempt_line("１２３"));      // fullwidth digits
    EXPECT_FALSE(snt::is_exempt_line("ア"));          // katakana letter
}

// Real English is counted apart from the Japanese a line still carries, per
// script and in total, and the report names the document it counted.
TEST(Coverage, real_english_is_counted_against_the_japanese_left_behind) {
    bj::object bare;  // no 'translated' field at all -- nothing replaced it yet
    bare["text"] = "未処理";

    bj::object sc01;
    sc01["strings"] = bj::array{entry("こんにちは", "Hello"), entry("未翻訳", "未翻訳"), bare};
    bj::object sc02;
    sc02["strings"] = bj::array{entry("朝が来た。", "Morning came.")};
    bj::object empty;
    empty["strings"] = bj::array{};
    bj::object root;
    root["script\\a.ast"] = sc01;
    root["script\\b.ast"] = sc02;
    root["script\\c.ast"] = empty;

    const auto report = snt::count_coverage(root, "some_document.json");
    EXPECT_EQ(report.source_file, "some_document.json");
    EXPECT_EQ(report.totals.translated, 2);
    EXPECT_EQ(report.totals.identity, 2);
    EXPECT_EQ(report.totals.exempt, 0);
    EXPECT_EQ(report.totals.total, 4);
    ASSERT_EQ(report.per_script.size(), 3u);
    EXPECT_EQ(report.per_script.at("script\\a.ast").translated, 1);
    EXPECT_EQ(report.per_script.at("script\\a.ast").identity, 2);
    EXPECT_EQ(report.per_script.at("script\\a.ast").total, 3);
    EXPECT_EQ(report.per_script.at("script\\b.ast").translated, 1);
    EXPECT_EQ(report.per_script.at("script\\c.ast").total, 0);

    const std::string out =
        (fs::temp_directory_path() / "snt_ut_coverage.json").u8string();
    snt::write_coverage_report(report, out);
    const auto back = snt::json_parse_file(out).get_object();
    EXPECT_EQ(back.at("source_file").get_string(), "some_document.json");
    EXPECT_EQ(back.at("totals").get_object().at("translated").get_int64(), 2);
    EXPECT_EQ(back.at("totals").get_object().at("total").get_int64(), 4);
    EXPECT_EQ(back.at("scripts")
                  .get_object()
                  .at("script\\b.ast")
                  .get_object()
                  .at("translated")
                  .get_int64(),
              1);
    fs::remove(fs::u8path(out));
}

// An exempt line leaves the denominator rather than counting as a miss, so a
// document nothing can improve does not read as permanently incomplete.
TEST(Coverage, exempt_lines_are_separated_from_real_misses) {
    bj::object sc01;
    sc01["strings"] = bj::array{
        entry("こんにちは", "Hello"),    // translated
        entry("未翻訳", "未翻訳"),       // a real miss
        entry("？？？", "？？？"),       // exempt: symbols only
        entry("　・・・", "　・・・"),   // exempt
        entry("「……」", "\"......\""),  // symbols, but genuinely translated
    };
    bj::object root;
    root["script\\a.ast"] = sc01;

    const auto report = snt::count_coverage(root, "doc.json");
    EXPECT_EQ(report.totals.translated, 2);
    EXPECT_EQ(report.totals.identity, 1);
    EXPECT_EQ(report.totals.exempt, 2);
    EXPECT_EQ(report.totals.total, 5);
    EXPECT_EQ(report.totals.translatable(), 3);

    const std::string out = (fs::temp_directory_path() / "snt_ut_exempt.json").u8string();
    snt::write_coverage_report(report, out);
    const auto totals =
        snt::json_parse_file(out).get_object().at("totals").get_object();
    EXPECT_EQ(totals.at("exempt").get_int64(), 2);
    EXPECT_EQ(totals.at("translatable").get_int64(), 3);
    EXPECT_EQ(totals.at("identity").get_int64(), 1);
    fs::remove(fs::u8path(out));
}

// The report sits beside the document it counted, because a run can hold two.
TEST(Coverage, the_report_is_written_beside_the_document_it_counted) {
    const std::string doc =
        (fs::temp_directory_path() / "script_output" / "translated_text.json").u8string();
    const std::string report = snt::coverage_report_path(doc);
    EXPECT_EQ(fs::u8path(report).parent_path(), fs::u8path(doc).parent_path());
    EXPECT_EQ(fs::u8path(report).filename().u8string(), "coverage_report.json");
}

// The exempt set of the deployed corpus, pinned rather than left to a side
// script.  This engine captures a bare string only when it holds Japanese, so
// the only exempt entries are the plates the engine shows before it names a
// speaker.  Skips when the extraction has not been produced yet.
TEST(Coverage, the_exempt_set_over_the_current_extraction) {
    const fs::path extracted = fs::u8path(std::string(SNT_PROJECT_DIR) +
                                          "\\script_output\\extracted_text.json");
    if (!fs::exists(extracted)) GTEST_SKIP() << "extracted_text.json not built yet";

    const bj::value root = snt::json_parse_file(extracted.u8string());
    std::set<std::string> distinct;
    std::int64_t entries = 0, total = 0;
    for (const auto& script : root.get_object()) {
        const auto* strings = script.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (const auto& ev : strings->get_array()) {
            const std::string text(ev.get_object().at("text").get_string());
            ++total;
            if (snt::is_exempt_line(text)) {
                ++entries;
                distinct.insert(text);
            }
        }
    }
    EXPECT_EQ(total, 92774) << "corpus changed -- re-measure before trusting the rest";
    EXPECT_EQ(distinct.size(), 1u);
    EXPECT_EQ(entries, 6);
    EXPECT_EQ(*distinct.begin(), "？？？");
}
