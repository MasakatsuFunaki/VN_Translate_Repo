// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pure unit tests for src/glossary.cpp, src/cache/speaker_gate.{h,cpp},
// src/cache/cache_core.{h,cpp}, and src/build_tsv/build_tsv.{h,cpp}:
// speaker mappings, story order, synthetic speaker-gate checks, and
// speaker-plate postprocess-to-word-wrap lifecycle.
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace exm::translate {
namespace {

namespace bj = boost::json;

constexpr std::size_t kExpectedStoryOrderCount = 3;
constexpr std::size_t kSyntheticMappedSpeakerCount = 5;
constexpr std::size_t kSyntheticMappedDialogueCount = 12;
constexpr int kExtractSpeakerCheckId = 1;
constexpr int kExhaustiveCoverageCheckId = 5;

bj::object make_dialogue_entry(const std::string& text) {
    bj::object entry;
    entry["type"] = "dialogue";
    entry["has_jp"] = true;
    entry["text"] = text;
    return entry;
}

bj::object make_synthetic_extracted_root(const std::vector<std::string>& lines) {
    bj::array strings;
    for (const std::string& line : lines) {
        strings.push_back(make_dialogue_entry(line));
    }
    bj::object script_obj;
    script_obj["strings"] = std::move(strings);
    bj::object root;
    root["tantai3.spt"] = std::move(script_obj);
    return root;
}

TEST(GlossaryContract, SpeakerTableStoryOrderAndPromptInvariants) {
    const auto& ordered = name_translations_ordered();
    const auto& map = character_names();
    EXPECT_EQ(ordered.size(), map.size());
    ASSERT_NE(map.find("水菜"), map.end());
    EXPECT_EQ(map.at("水菜"), "Mizuna");
    ASSERT_NE(map.find("瑞菜"), map.end());
    EXPECT_EQ(map.at("瑞菜"), "Mizuna");
    ASSERT_NE(map.find("史郎"), map.end());
    EXPECT_EQ(map.at("史郎"), "Shirou");
    ASSERT_NE(map.find("？？？"), map.end());
    EXPECT_EQ(map.at("？？？"), "???");
    ASSERT_NE(map.find("男Ａ"), map.end());
    EXPECT_EQ(map.at("男Ａ"), "Man A");

    const std::vector<std::string> expected_order = {
        "tantai3.spt",
        "koiEX_backscript.spt",
        "sys.spt",
    };
    EXPECT_EQ(story_order().size(), kExpectedStoryOrderCount);
    EXPECT_EQ(story_order(), expected_order);

    const std::string& prompt = glossary_prompt();
    EXPECT_FALSE(prompt.empty());
    EXPECT_NE(prompt.find("Mizuna"), std::string::npos);
    EXPECT_NE(prompt.find("Shirou"), std::string::npos);
}

TEST(GlossaryContract, SyntheticSpeakerGatePassesMappedAndRejectsUnmappedOrSparse) {
    const std::vector<std::string> valid_lines = {
        "水菜\r\n「史郎くん、おはよう……」",
        "史郎\r\n「ああ、おはよう。」",
        "瑞菜\r\n「今日もいい天気だね。」",
        "？？？\r\n「……見つけたぞ。」",
        "男Ａ\r\n「逃がすな！」",
        "水菜\r\n「っ……誰なの！？」",
        "史郎\r\n「下がっていろ、水菜。」",
        "瑞菜\r\n「わかった……気をつけて。」",
        "男Ａ\r\n「観念しろ！」",
        "？？？\r\n「無駄な抵抗はやめろ。」",
        "水菜\r\n「いや……近づかないで！」",
        "史郎\r\n「させるものか。」",
    };

    const bj::object valid_root = make_synthetic_extracted_root(valid_lines);
    const SpeakerCheckReport ok_report = run_speaker_checks(valid_root);
    EXPECT_TRUE(ok_report.passed) << ok_report.reason;
    EXPECT_EQ(ok_report.dialogue_total, kSyntheticMappedDialogueCount);
    EXPECT_EQ(ok_report.speakers_in_data, kSyntheticMappedSpeakerCount);

    std::vector<std::string> with_unmapped = valid_lines;
    with_unmapped.push_back("未知の人物\r\n「名簿にない話者だ。」");
    const bj::object unmapped_root = make_synthetic_extracted_root(with_unmapped);
    const SpeakerCheckReport unmapped_report = run_speaker_checks(unmapped_root);
    EXPECT_FALSE(unmapped_report.passed);
    EXPECT_EQ(unmapped_report.failed_check, kExhaustiveCoverageCheckId);

    const bj::object sparse_root = make_synthetic_extracted_root({
        "水菜\r\n「一行だけ……」",
    });
    const SpeakerCheckReport sparse_report = run_speaker_checks(sparse_root);
    EXPECT_FALSE(sparse_report.passed);
    EXPECT_EQ(sparse_report.failed_check, kExtractSpeakerCheckId);
}

TEST(GlossaryContract, SpeakerPrefixedDialoguePostprocessToWordWrapChain) {
    const std::string original = "水菜\r\n「……私をどうするつもり？」\r\n";
    const std::string expected_postprocessed =
        "Mizuna\r\n\"...What do you plan to do with me?\"\r\n";
    const std::string expected_final =
        "Mizuna: \"...What do you plan to do with me?\"\r\n";

    const std::string post1 =
        postprocess(original, "\"…What do you plan to do with me?\"");
    EXPECT_EQ(post1, expected_postprocessed);
    const std::string final1 =
        build_tsv::word_wrap(build_tsv::merge_speaker_line(build_tsv::sanitize_newlines(post1)));
    EXPECT_EQ(final1, expected_final);

    const std::string post2 =
        postprocess(original, "水菜\r\n\"…What do you plan to do with me?\"");
    EXPECT_EQ(post2, expected_postprocessed);
    const std::string final2 =
        build_tsv::word_wrap(build_tsv::merge_speaker_line(build_tsv::sanitize_newlines(post2)));
    EXPECT_EQ(final2, expected_final);

    const std::string post3 =
        postprocess(original, "Mizuna\r\n\"...What do you plan to do with me?\"\r\n");
    EXPECT_EQ(post3, expected_postprocessed);
    const std::string final3 =
        build_tsv::word_wrap(build_tsv::merge_speaker_line(build_tsv::sanitize_newlines(post3)));
    EXPECT_EQ(final3, expected_final);
}

}  // namespace
}  // namespace exm::translate
