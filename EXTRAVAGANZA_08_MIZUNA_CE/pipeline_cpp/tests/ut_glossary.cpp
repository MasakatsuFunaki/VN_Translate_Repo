// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Component tests for src/glossary.cpp, src/cache/speaker_gate.{h,cpp},
// src/cache/cache_core.{h,cpp}, and src/build_tsv/build_tsv.{h,cpp} on
// EXTRA_VA_MIZUNA_CE speaker mappings, extracted_text.json speaker coverage,
// and speaker-plate lifecycle.
#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/util.h"

namespace exm::translate {
namespace {

constexpr std::size_t kExpectedStoryOrderCount = 3;
constexpr std::size_t kExpectedDistinctDialogueSpeakers = 29;
constexpr std::size_t kExpectedDialogueLineCount = 971;

TEST(GlossaryContract, SpeakerTableAndStoryOrderAndExtractedCoveragePass) {
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

    std::filesystem::path extracted_path;
    std::filesystem::path cur = std::filesystem::current_path();
    for (int i = 0; i < 6; ++i) {
        if (std::filesystem::exists(cur / "script_output" / "extracted_text.json")) {
            extracted_path = cur / "script_output" / "extracted_text.json";
            break;
        }
        if (std::filesystem::exists(cur / "EXTRAVAGANZA_08_MIZUNA_CE" / "script_output" / "extracted_text.json")) {
            extracted_path = cur / "EXTRAVAGANZA_08_MIZUNA_CE" / "script_output" / "extracted_text.json";
            break;
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) break;
        cur = cur.parent_path();
    }
    if (extracted_path.empty() || !std::filesystem::exists(extracted_path)) {
        GTEST_SKIP() << "script_output/extracted_text.json not present in checkout";
    }

    const auto root_val = json_parse_file(extracted_path.u8string());
    ASSERT_TRUE(root_val.is_object());
    const auto& root = root_val.get_object();

    std::size_t dialogue_count = 0;
    std::set<std::string> dialogue_speakers;
    std::vector<std::string> unmapped_speakers;

    for (const auto& fkv : root) {
        ASSERT_TRUE(fkv.value().is_object());
        const auto& fobj = fkv.value().get_object();
        const auto* strings = fobj.if_contains("strings");
        ASSERT_NE(strings, nullptr);
        ASSERT_TRUE(strings->is_array());
        for (const auto& sv : strings->get_array()) {
            ASSERT_TRUE(sv.is_object());
            const auto& s = sv.get_object();
            if (std::string(s.at("type").get_string()) != "dialogue") continue;
            ++dialogue_count;
            const std::string text(s.at("text").get_string());
            const std::size_t crlf = text.find("\r\n");
            ASSERT_NE(crlf, std::string::npos) << "Dialogue entry missing CRLF: " << text;
            const std::string jp_speaker = trim(text.substr(0, crlf));
            ASSERT_FALSE(jp_speaker.empty());
            dialogue_speakers.insert(jp_speaker);
            if (map.find(jp_speaker) == map.end()) {
                unmapped_speakers.push_back(jp_speaker);
            }
            const std::string resolved = extract_speaker(text);
            EXPECT_NE(resolved, "NARRATION") << "Failed to extract speaker from: " << text;
            EXPECT_EQ(resolved, map.at(jp_speaker));
        }
    }

    EXPECT_EQ(dialogue_count, kExpectedDialogueLineCount);
    ASSERT_EQ(dialogue_speakers.size(), kExpectedDistinctDialogueSpeakers);
    EXPECT_TRUE(unmapped_speakers.empty());

    const auto report = run_speaker_checks(root);
    EXPECT_TRUE(report.passed) << report.reason;
    EXPECT_EQ(report.dialogue_total, kExpectedDialogueLineCount);
    EXPECT_EQ(report.speakers_in_data, kExpectedDistinctDialogueSpeakers);
}

TEST(GlossaryContract, SpeakerPrefixedDialoguePostprocessToWordWrapChain) {
    const std::string original = "水菜\r\n「……私をどうするつもり？」\r\n";
    const std::string expected_postprocessed =
        "Mizuna\r\n\"...What do you plan to do with me?\"\r\n";
    const std::string expected_final =
        "Mizuna: \"...What do you plan to do with me?\"\r\n";

    // Case 1: Model outputs bare translated quote without speaker prefix or trailing CRLF.
    const std::string post1 =
        postprocess(original, "\"…What do you plan to do with me?\"");
    EXPECT_EQ(post1, expected_postprocessed);
    const std::string final1 =
        build_tsv::word_wrap(build_tsv::merge_speaker_line(build_tsv::sanitize_newlines(post1)));
    EXPECT_EQ(final1, expected_final);

    // Case 2: Model leaves Japanese speaker prefix in output.
    const std::string post2 =
        postprocess(original, "水菜\r\n\"…What do you plan to do with me?\"");
    EXPECT_EQ(post2, expected_postprocessed);
    const std::string final2 =
        build_tsv::word_wrap(build_tsv::merge_speaker_line(build_tsv::sanitize_newlines(post2)));
    EXPECT_EQ(final2, expected_final);

    // Case 3: Model already includes English speaker prefix and trailing CRLF (no doubling).
    const std::string post3 =
        postprocess(original, "Mizuna\r\n\"...What do you plan to do with me?\"\r\n");
    EXPECT_EQ(post3, expected_postprocessed);
    const std::string final3 =
        build_tsv::word_wrap(build_tsv::merge_speaker_line(build_tsv::sanitize_newlines(post3)));
    EXPECT_EQ(final3, expected_final);
}

}  // namespace
}  // namespace exm::translate
