// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Component and functional-contract tests for Cache persistence, the 5-stage
// speaker gate on WelcomeToAnzU!_CE, and CP932 TSV generation.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>

#include <boost/json.hpp>

#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/util.h"
#include "extract/extractor.h"

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace exm;

namespace {

constexpr int kWrapColumns = 54;

}  // namespace

TEST(CacheComponent, CacheRoundTripAndPostprocessSpeakerPlate) {
    const fs::path tmp_cache =
        fs::temp_directory_path() / "anzu_ce_ut_cache.json";
    std::error_code ec;
    fs::remove(tmp_cache, ec);

    translate::Cache cache;
    cache.set("夢美\r\n「いらっしゃいませ」", "Yumemi\r\n\"Welcome!\"");
    cache.set("アゲハ\r\n「ふん」", "Ageha\r\n\"Hmph.\"");
    translate::save_cache(cache, tmp_cache.u8string());

    const translate::Cache reloaded = translate::load_cache(tmp_cache.u8string());
    ASSERT_EQ(reloaded.size(), 2u);
    ASSERT_NE(reloaded.get("夢美\r\n「いらっしゃいませ」"), nullptr);
    EXPECT_EQ(*reloaded.get("夢美\r\n「いらっしゃいませ」"), "Yumemi\r\n\"Welcome!\"");

    const std::string post =
        translate::postprocess("夢美\r\n「テスト」\r\n", "“Test…”");
    EXPECT_EQ(post, "Yumemi\r\n\"Test...\"\r\n");

    fs::remove(tmp_cache, ec);
}

TEST(CacheComponent, SpeakerGatePassesOnLiveExtractedCorpus) {
    const char* env_dir = std::getenv("VN_GAME_DIR");
    if (!env_dir || !*env_dir) {
        GTEST_SKIP() << "VN_GAME_DIR not set; skipping live SPT extraction check.";
    }
    const fs::path spt_dir = fs::u8path(env_dir) / "spt";
    if (!fs::is_directory(spt_dir)) {
        GTEST_SKIP() << "Live WelcomeToAnzU!_CE directory not present: " << spt_dir.u8string();
    }

    const fs::path tmp_extracted =
        fs::temp_directory_path() / "anzu_ce_ut_gate_extracted.json";
    std::error_code ec;
    fs::remove(tmp_extracted, ec);

    ASSERT_EQ(extract::run_extract(spt_dir.u8string(), tmp_extracted.u8string()), 0);
    const bj::value extracted_v = json_parse_file(tmp_extracted.u8string());
    const auto report = translate::run_speaker_checks(extracted_v.as_object());
    EXPECT_TRUE(report.passed) << report.reason;
    EXPECT_EQ(report.failed_check, 0);

    fs::remove(tmp_extracted, ec);
}

TEST(CacheComponent, BuildTsvFoldsSpeakerWrapsAndWritesValidCp932) {
    const fs::path tmp_json = fs::temp_directory_path() / "anzu_ce_ut_tr.json";
    const fs::path tmp_tsv = fs::temp_directory_path() / "anzu_ce_ut_tr.tsv";
    std::error_code ec;
    fs::remove(tmp_json, ec);
    fs::remove(tmp_tsv, ec);

    bj::object row;
    row["text"] = "夢美\r\n「いらっしゃいませ」";
    row["translated"] =
        "Yumemi\r\n\"Welcome to Cafe AnzU, please make yourself right at home today!\"";
    bj::object fdata;
    fdata["strings"] = bj::array{std::move(row)};
    bj::object root;
    root["tokuten.spt"] = std::move(fdata);
    write_file(tmp_json.u8string(), bj::serialize(bj::value(std::move(root))));

    ASSERT_EQ(build_tsv::run_build(tmp_json.u8string(), tmp_tsv.u8string()), 0);
    const Bytes raw = read_file(tmp_tsv.u8string());
    const auto utf8 = cp932_to_utf8_strict(raw);
    ASSERT_TRUE(utf8.has_value());
    EXPECT_EQ(std::count(utf8->begin(), utf8->end(), '\t'), 1);
    EXPECT_NE(utf8->find("Yumemi: \"Welcome to Cafe AnzU,"), std::string::npos);
    EXPECT_NE(utf8->find("\\r\\n"), std::string::npos);

    const std::string wrapped = build_tsv::word_wrap(
        "Yumemi: \"Welcome to Cafe AnzU, please make yourself right at home today!\"",
        kWrapColumns);
    EXPECT_NE(wrapped.find("\r\n"), std::string::npos);
    EXPECT_EQ(build_tsv::reflow_lines("line1 \r\n  line2\r\n"), "line1 line2\r\n");

    fs::remove(tmp_json, ec);
    fs::remove(tmp_tsv, ec);
}
