// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pure standalone unit tests for Cache persistence, the 5-stage speaker gate
// on synthetic WelcomeToAnzU!_CE corpora, and CP932 TSV generation.
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace exm;

namespace {

constexpr int kWrapColumns = 54;
constexpr int kMinSyntheticDialoguePadding = 50;
constexpr int kExpectedCoverageCheckId = 5;
constexpr std::size_t kExpectedReloadedEntries = 2;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
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

bj::object make_dialogue_entry(const std::string& text) {
    bj::object o;
    o["text"] = text;
    o["type"] = "dialogue";
    o["has_jp"] = true;
    return o;
}

bj::object make_synthetic_extracted() {
    bj::array strings;
    for (const auto& [jp, en] : translate::name_translations_ordered()) {
        (void)en;
        strings.push_back(make_dialogue_entry(jp + "\r\n「いらっしゃいませ」"));
    }
    while (static_cast<int>(strings.size()) < kMinSyntheticDialoguePadding) {
        strings.push_back(make_dialogue_entry("夢美\r\n「追加の台詞です」"));
    }

    bj::object tokuten_file;
    tokuten_file["strings"] = std::move(strings);
    bj::object root;
    root["tokuten.spt"] = std::move(tokuten_file);
    return root;
}

}  // namespace

TEST(CacheUnit, CacheRoundTripAndPostprocessSpeakerPlate) {
    const ScopedTempDir tmp("anzu_ce_ut_cache");
    const fs::path tmp_cache = tmp.path() / "anzu_ce_ut_cache.json";

    translate::Cache cache;
    cache.set("夢美\r\n「いらっしゃいませ」", "Yumemi\r\n\"Welcome!\"");
    cache.set("アゲハ\r\n「ふん」", "Ageha\r\n\"Hmph.\"");
    translate::save_cache(cache, tmp_cache.u8string());

    const translate::Cache reloaded = translate::load_cache(tmp_cache.u8string());
    ASSERT_EQ(reloaded.size(), kExpectedReloadedEntries);
    ASSERT_NE(reloaded.get("夢美\r\n「いらっしゃいませ」"), nullptr);
    EXPECT_EQ(*reloaded.get("夢美\r\n「いらっしゃいませ」"), "Yumemi\r\n\"Welcome!\"");

    const std::string post =
        translate::postprocess("夢美\r\n「テスト」\r\n", "“Test…”");
    EXPECT_EQ(post, "Yumemi\r\n\"Test...\"\r\n");
}

TEST(CacheUnit, SpeakerGatePassesOnSyntheticCorpusAndRejectsUnknownSpeaker) {
    bj::object extracted = make_synthetic_extracted();
    const auto pass_report = translate::run_speaker_checks(extracted);
    EXPECT_TRUE(pass_report.passed) << pass_report.reason;
    EXPECT_EQ(pass_report.failed_check, 0);

    extracted["tokuten.spt"].as_object()["strings"].as_array().push_back(
        make_dialogue_entry("謎の来訪者\r\n「誰かしら」"));
    const auto fail_report = translate::run_speaker_checks(extracted);
    EXPECT_FALSE(fail_report.passed);
    EXPECT_EQ(fail_report.failed_check, kExpectedCoverageCheckId);
}

TEST(CacheUnit, BuildTsvFoldsSpeakerWrapsAndWritesValidCp932) {
    const ScopedTempDir tmp("anzu_ce_ut_build_tsv");
    const fs::path tmp_json = tmp.path() / "anzu_ce_ut_tr.json";
    const fs::path tmp_tsv = tmp.path() / "anzu_ce_ut_tr.tsv";

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
}
