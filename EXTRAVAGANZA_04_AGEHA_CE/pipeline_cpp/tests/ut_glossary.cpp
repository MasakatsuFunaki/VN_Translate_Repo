// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for src/cache/glossary.{h,cpp}: AGEHA_CE speaker mappings,
// story order, and system prompt invariants.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cache/glossary.h"

namespace tr = exm::translate;

namespace {

constexpr std::size_t kExpectedSpeakerCount = 32;
constexpr std::size_t kExpectedStoryOrderCount = 4;

}  // namespace

TEST(Glossary, speaker_table_contains_all_32_unique_ageha_speakers) {
    const auto& ordered = tr::name_translations_ordered();
    const auto& map = tr::name_translations();

    EXPECT_EQ(ordered.size(), kExpectedSpeakerCount);
    EXPECT_EQ(map.size(), kExpectedSpeakerCount);

    EXPECT_EQ(map.at("アゲハ"), "Ageha");
    EXPECT_EQ(map.at("シュミ"), "Shumi");
    EXPECT_EQ(map.at("史郎"), "Shirou");
    EXPECT_EQ(map.at("レン"), "Ren");
    EXPECT_EQ(map.at("西"), "Nishi");
    EXPECT_EQ(map.at("？？？"), "???");
    EXPECT_EQ(map.at("男Ａ"), "Man A");
    EXPECT_EQ(map.at("男Ｚ"), "Man Z");
}

TEST(Glossary, story_order_places_main_spt_first) {
    const std::vector<std::string> expected = {
        "main.spt",
        "agh_backscript.spt",
        "agh_test01.spt",
        "sys.spt",
    };
    EXPECT_EQ(tr::story_order().size(), kExpectedStoryOrderCount);
    EXPECT_EQ(tr::story_order(), expected);
}

TEST(Glossary, glossary_prompt_contains_ageha_characters_and_terms) {
    const std::string& prompt = tr::glossary_prompt();
    EXPECT_FALSE(prompt.empty());
    EXPECT_EQ(prompt, std::string(tr::SYSTEM_PROMPT));
    EXPECT_NE(prompt.find("Ageha Kujou"), std::string::npos);
    EXPECT_NE(prompt.find("九条アゲハ"), std::string::npos);
    EXPECT_NE(prompt.find("Shirou Kitabatake"), std::string::npos);
    EXPECT_NE(prompt.find("北畠史郎"), std::string::npos);
    EXPECT_NE(prompt.find("Shumi"), std::string::npos);
    EXPECT_NE(prompt.find("Yumemi Sakurai"), std::string::npos);
    EXPECT_NE(prompt.find("Jinmu"), std::string::npos);
    EXPECT_NE(prompt.find("AnzU"), std::string::npos);
}
