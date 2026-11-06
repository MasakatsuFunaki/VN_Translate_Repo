// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for src/cache/speaker_gate.{h,cpp}: verifies all 5 speaker checks
// and confirms story_order() sampling priority over alphabetical keys.
#include <gtest/gtest.h>

#include <string>

#include <boost/json.hpp>

#include "cache/glossary.h"
#include "cache/speaker_gate.h"

namespace bj = boost::json;
namespace tr = exm::translate;

namespace {

constexpr int kMinMainDialoguePadding = 50;
constexpr int kUnquotedTestLineCount = 3;
constexpr int kExpectedCoverageCheckId = 5;

bj::object make_dialogue_entry(const std::string& text) {
    bj::object o;
    o["text"] = text;
    o["type"] = "dialogue";
    o["has_jp"] = true;
    return o;
}

bj::object make_full_coverage_extracted() {
    bj::array main_strings;
    for (const auto& [jp, en] : tr::name_translations_ordered()) {
        (void)en;
        main_strings.push_back(make_dialogue_entry(jp + "\r\n「テスト台詞」"));
    }
    while (static_cast<int>(main_strings.size()) < kMinMainDialoguePadding) {
        main_strings.push_back(make_dialogue_entry("アゲハ\r\n「追加の台詞です」"));
    }

    bj::object main_file;
    main_file["strings"] = std::move(main_strings);

    bj::array test_strings;
    for (int i = 0; i < kUnquotedTestLineCount; ++i) {
        test_strings.push_back(make_dialogue_entry("アゲハ\r\nテスト用の引用符なし文字列"));
    }
    bj::object test_file;
    test_file["strings"] = std::move(test_strings);

    bj::object root;
    // Insert agh_test01.spt before main.spt so alphabetical iteration would
    // hit the 3 unquoted lines first if story_order() were not respected.
    root["agh_test01.spt"] = std::move(test_file);
    root["main.spt"] = std::move(main_file);
    return root;
}

}  // namespace

TEST(SpeakerGate, passes_when_all_32_speakers_present_and_samples_story_order_first) {
    const bj::object extracted = make_full_coverage_extracted();
    const tr::SpeakerCheckReport report = tr::run_speaker_checks(extracted);
    EXPECT_TRUE(report.passed) << report.reason;
    EXPECT_EQ(report.failed_check, 0);
    EXPECT_EQ(report.speakers_in_data, tr::name_translations_ordered().size());
    EXPECT_EQ(report.glossary_entries, tr::name_translations_ordered().size());
}

TEST(SpeakerGate, fails_check_1_when_extraction_is_too_small) {
    bj::object root;
    bj::object main_file;
    bj::array strings;
    strings.push_back(make_dialogue_entry("アゲハ\r\n「短い」"));
    main_file["strings"] = std::move(strings);
    root["main.spt"] = std::move(main_file);

    const tr::SpeakerCheckReport report = tr::run_speaker_checks(root);
    EXPECT_FALSE(report.passed);
    EXPECT_EQ(report.failed_check, 1);
}

TEST(SpeakerGate, fails_check_5_when_unknown_speaker_appears_in_data) {
    bj::object extracted = make_full_coverage_extracted();
    extracted["main.spt"].as_object()["strings"].as_array().push_back(
        make_dialogue_entry("謎の人物\r\n「誰だ？」"));

    const tr::SpeakerCheckReport report = tr::run_speaker_checks(extracted);
    EXPECT_FALSE(report.passed);
    EXPECT_EQ(report.failed_check, kExpectedCoverageCheckId);
}
