// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for coverage accounting over synthetic translated documents.
#include "common/coverage.h"
#include "common/util.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <boost/json.hpp>
#include <gtest/gtest.h>

namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;

constexpr std::int64_t kBasicTranslatedCount = 2;
constexpr std::int64_t kBasicIdentityCount = 2;
constexpr std::int64_t kBasicExemptCount = 0;
constexpr std::int64_t kBasicTotalCount = 4;
constexpr std::size_t kBasicScriptCount = 3;
constexpr std::int64_t kScriptATranslated = 1;
constexpr std::int64_t kScriptAIdentity = 2;
constexpr std::int64_t kScriptATotal = 3;
constexpr std::int64_t kScriptBTranslated = 1;
constexpr std::int64_t kScriptCTotal = 0;
constexpr std::int64_t kMixedTranslated = 2;
constexpr std::int64_t kMixedIdentity = 1;
constexpr std::int64_t kMixedExempt = 2;
constexpr std::int64_t kMixedTotal = 5;
constexpr std::int64_t kMixedTranslatable = 3;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view utf8_name)
        : path_(fs::temp_directory_path() / fs::u8path(utf8_name)) {
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

bj::object entry(const char* jp, const char* en) {
    bj::object o;
    o["text"] = jp;
    o["translated"] = en;
    return o;
}

}  // namespace

TEST(Coverage, the_exempt_predicate_fires_on_symbols_only) {
    EXPECT_TRUE(snt::is_exempt_line("？？？"));
    EXPECT_TRUE(snt::is_exempt_line("……"));
    EXPECT_TRUE(snt::is_exempt_line("「……」"));
    EXPECT_TRUE(snt::is_exempt_line("　・・・"));
    EXPECT_TRUE(snt::is_exempt_line("――"));

    EXPECT_FALSE(snt::is_exempt_line(""));
    EXPECT_FALSE(snt::is_exempt_line("こんにちは"));
    EXPECT_FALSE(snt::is_exempt_line("桜"));
    EXPECT_FALSE(snt::is_exempt_line("「あ……」"));
    EXPECT_FALSE(snt::is_exempt_line("OK"));
    EXPECT_FALSE(snt::is_exempt_line("Ａ"));
    EXPECT_FALSE(snt::is_exempt_line("１２３"));
    EXPECT_FALSE(snt::is_exempt_line("ア"));
}

TEST(Coverage, real_english_is_counted_against_the_japanese_left_behind) {
    bj::object bare;
    bare["text"] = "未処理";

    bj::object sc01;
    sc01["strings"] = bj::array{
        entry("こんにちは", "Hello"), entry("未翻訳", "未翻訳"), bare};
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
    EXPECT_EQ(report.totals.translated, kBasicTranslatedCount);
    EXPECT_EQ(report.totals.identity, kBasicIdentityCount);
    EXPECT_EQ(report.totals.exempt, kBasicExemptCount);
    EXPECT_EQ(report.totals.total, kBasicTotalCount);
    ASSERT_EQ(report.per_script.size(), kBasicScriptCount);
    EXPECT_EQ(report.per_script.at("script\\a.ast").translated, kScriptATranslated);
    EXPECT_EQ(report.per_script.at("script\\a.ast").identity, kScriptAIdentity);
    EXPECT_EQ(report.per_script.at("script\\a.ast").total, kScriptATotal);
    EXPECT_EQ(report.per_script.at("script\\b.ast").translated, kScriptBTranslated);
    EXPECT_EQ(report.per_script.at("script\\c.ast").total, kScriptCTotal);

    ScopedTempDir temp("snt_ut_coverage_basic");
    const std::string out =
        (temp.path() / fs::u8path("snt_ut_coverage.json")).u8string();
    snt::write_coverage_report(report, out);
    const auto back = snt::json_parse_file(out).get_object();
    EXPECT_EQ(back.at("source_file").get_string(), "some_document.json");
    EXPECT_EQ(
        back.at("totals").get_object().at("translated").get_int64(),
        kBasicTranslatedCount);
    EXPECT_EQ(
        back.at("totals").get_object().at("total").get_int64(),
        kBasicTotalCount);
    EXPECT_EQ(back.at("scripts")
                  .get_object()
                  .at("script\\b.ast")
                  .get_object()
                  .at("translated")
                  .get_int64(),
              kScriptBTranslated);
}

TEST(Coverage, exempt_lines_are_separated_from_real_misses) {
    bj::object sc01;
    sc01["strings"] = bj::array{
        entry("こんにちは", "Hello"),
        entry("未翻訳", "未翻訳"),
        entry("？？？", "？？？"),
        entry("　・・・", "　・・・"),
        entry("「……」", "\"......\""),
    };
    bj::object root;
    root["script\\a.ast"] = sc01;

    const auto report = snt::count_coverage(root, "doc.json");
    EXPECT_EQ(report.totals.translated, kMixedTranslated);
    EXPECT_EQ(report.totals.identity, kMixedIdentity);
    EXPECT_EQ(report.totals.exempt, kMixedExempt);
    EXPECT_EQ(report.totals.total, kMixedTotal);
    EXPECT_EQ(report.totals.translatable(), kMixedTranslatable);

    ScopedTempDir temp("snt_ut_coverage_exempt");
    const std::string out =
        (temp.path() / fs::u8path("snt_ut_exempt.json")).u8string();
    snt::write_coverage_report(report, out);
    const auto totals =
        snt::json_parse_file(out).get_object().at("totals").get_object();
    EXPECT_EQ(totals.at("exempt").get_int64(), kMixedExempt);
    EXPECT_EQ(totals.at("translatable").get_int64(), kMixedTranslatable);
    EXPECT_EQ(totals.at("identity").get_int64(), kMixedIdentity);
}

TEST(Coverage, the_report_is_written_beside_the_document_it_counted) {
    const std::string doc =
        (fs::temp_directory_path() / fs::u8path("script_output") /
         fs::u8path("translated_text.json"))
            .u8string();
    const std::string report = snt::coverage_report_path(doc);
    EXPECT_EQ(fs::u8path(report).parent_path(), fs::u8path(doc).parent_path());
    EXPECT_EQ(fs::u8path(report).filename().u8string(), "coverage_report.json");
}
