// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Component tests for src/cache/glossary.{h,cpp} and src/cache/speaker_gate.{h,cpp}:
// YUMEMIRYOJOKU_CE speaker mappings, story order, system prompt, and 5-check
// speaker gate verification.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/util.h"

namespace exm::translate {
namespace {

constexpr std::size_t kExpectedStoryOrderCount = 3;

TEST(GlossaryContract, SpeakerTableAndStoryOrderAndSpeakerGatePass) {
    const auto& ordered = name_translations_ordered();
    const auto& map = name_translations();
    EXPECT_EQ(ordered.size(), map.size());
    ASSERT_NE(map.find("夢美"), map.end());
    EXPECT_EQ(map.at("夢美"), "Yumemi");
    ASSERT_NE(map.find("夢美１"), map.end());
    EXPECT_EQ(map.at("夢美１"), "Yumemi 1");

    const std::vector<std::pair<std::string, std::string>> expected_choices = {
        {"使用する", "Use"},
        {"使用しない", "Do not use"},
        {"口を責める", "Torment her mouth"},
        {"胸を責める", "Torment her breasts"},
        {"秘所を責める", "Torment her privates"},
        {"尻を責める", "Torment her ass"},
        {"何もしない", "Do nothing"},
        {"触手で責める", "Torment with tentacles"},
        {"触手を外す", "Remove tentacles"},
        {"陰核を責める", "Torment her clitoris"},
        {"秘所を開く", "Spread her privates"},
        {"外す", "Remove"},
        {"粘液一斉放出", "Simultaneous Mucus Discharge"},
        {"白噴水の刑", "White Fountain Punishment"},
        {"触手貫通波", "Tentacle Piercing Wave"},
        {"夢美破壊乱舞", "Yumemi Destruction Barrage"},
    };
    for (const auto& [jp, en] : expected_choices) {
        ASSERT_NE(map.find(jp), map.end()) << "Missing short choice key: " << jp;
        EXPECT_EQ(map.at(jp), en);
    }

    const std::vector<std::string> expected_order = {
        "m_tokuten.spt",
        "backscript.spt",
        "sys.spt",
    };
    EXPECT_EQ(story_order().size(), kExpectedStoryOrderCount);
    EXPECT_EQ(story_order(), expected_order);

    const std::string& prompt = glossary_prompt();
    EXPECT_FALSE(prompt.empty());
    EXPECT_NE(prompt.find("Yumemi Sakurai"), std::string::npos);

    std::filesystem::path extracted_path;
    std::filesystem::path cur = std::filesystem::current_path();
    for (int i = 0; i < 6; ++i) {
        if (std::filesystem::exists(cur / "script_output" / "extracted_text.json")) {
            extracted_path = cur / "script_output" / "extracted_text.json";
            break;
        }
        if (std::filesystem::exists(cur / "EXTRAVAGANZA_06_YUMEMI_CE" / "script_output" / "extracted_text.json")) {
            extracted_path = cur / "EXTRAVAGANZA_06_YUMEMI_CE" / "script_output" / "extracted_text.json";
            break;
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) break;
        cur = cur.parent_path();
    }
    if (!extracted_path.empty() && std::filesystem::exists(extracted_path)) {
        const auto root = json_parse_file(extracted_path.u8string());
        ASSERT_TRUE(root.is_object());
        const auto report = run_speaker_checks(root.get_object());
        EXPECT_TRUE(report.passed) << report.reason;
    } else {
        boost::json::array strings;
        for (int i = 0; i < 12; ++i) {
            boost::json::object s;
            s["type"] = "dialogue";
            s["text"] = "夢美\r\n「やめてください……」";
            strings.push_back(std::move(s));
        }
        boost::json::object file_obj;
        file_obj["strings"] = std::move(strings);
        boost::json::object synthetic_root;
        synthetic_root["m_tokuten.spt"] = std::move(file_obj);
        const auto report = run_speaker_checks(synthetic_root);
        EXPECT_TRUE(report.passed) << report.reason;
    }
}

}  // namespace
}  // namespace exm::translate
