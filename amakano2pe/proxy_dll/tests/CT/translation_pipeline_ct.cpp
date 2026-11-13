// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests for the amakano2pe translation pipeline.
//
// End-to-end: read the staged translation_table.tsv from disk via
// translator_logic::LoadTsvFile and validate post-load invariants that the
// proxy DLL depends on.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace {

using translator_logic::DefaultSpeakerNameKeys;
using translator_logic::HasDanglingTrailingBackslash;
using translator_logic::LoadTsvFile;
using translator_logic::SpeakerKeySet;
using translator_logic::TranslationMap;

constexpr std::size_t kMinExpectedTranslations = 10;

}  // namespace

class TranslationPipelineTest : public ::testing::Test {
protected:
    TranslationMap map;

    void SetUp() override {
        ASSERT_TRUE(LoadTsvFile(GAME_TSV_PATH, map))
            << "Missing or empty deployed TSV: " << GAME_TSV_PATH;
    }
};

TEST_F(TranslationPipelineTest, HasReasonableSize) {
    EXPECT_GT(map.size(), kMinExpectedTranslations);
}

TEST_F(TranslationPipelineTest, EveryValueIsNullFree) {
    std::size_t bad = 0;
    for (const auto& kv : map) {
        if (kv.second.find('\0') != std::string::npos) {
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0u);
}

TEST_F(TranslationPipelineTest, NoEmptyKeysOrValues) {
    for (const auto& kv : map) {
        EXPECT_FALSE(kv.first.empty());
        EXPECT_FALSE(kv.second.empty());
    }
}

TEST_F(TranslationPipelineTest, EveryDefaultSpeakerNameKeyExistsInTsv) {
    const SpeakerKeySet& speaker_keys = DefaultSpeakerNameKeys();
    ASSERT_FALSE(speaker_keys.empty());
    for (const std::string& key : speaker_keys) {
        EXPECT_NE(map.find(key), map.end())
            << "DefaultSpeakerNameKeys entry missing from TSV: " << key;
    }
}

TEST_F(TranslationPipelineTest, ZeroEntriesEndWithDanglingTrailingBackslash) {
    std::size_t dangling_count = 0;
    for (const auto& kv : map) {
        if (HasDanglingTrailingBackslash(kv.second)) {
            ++dangling_count;
        }
    }
    EXPECT_EQ(dangling_count, 0u);
}

