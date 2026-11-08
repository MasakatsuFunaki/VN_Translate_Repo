// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include <filesystem>
#include <string>

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include "apps/paths.h"
#include "common/util.h"
#include "extract/extractor.h"

namespace {

namespace fs = std::filesystem;
namespace bj = boost::json;

constexpr std::size_t kExpectedArchiveEntries = 418U;
constexpr std::size_t kExpectedSobFiles = 413U;
constexpr std::size_t kExpectedSob0Count = 411U;
constexpr std::size_t kExpectedSob1Count = 1U;
constexpr std::size_t kExpectedSob2Count = 1U;
constexpr std::size_t kExpectedScenarioScripts = 387U;
constexpr std::size_t kExpectedMessageCount = 57865U;
constexpr std::size_t kExpectedVoicedDialogueCount = 21424U;
constexpr std::size_t kExpectedUnvoicedDialogueCount = 13744U;
constexpr std::size_t kExpectedVoiceOnlyDialogueCount = 304U;
constexpr std::size_t kExpectedNarrationCount = 22393U;
constexpr std::size_t kExpectedChoiceBlockCount = 24U;
constexpr std::size_t kExpectedChoiceOptionCount = 54U;
constexpr std::size_t kExpectedTitleCount = 388U;
constexpr std::size_t kExpectedSynopsisCount = 388U;
constexpr std::size_t kExpectedUiStringCount = 156U;
constexpr std::size_t kExpectedTotalEntryCount = 58851U;
constexpr std::size_t kExpectedUniqueSpeakerCount = 324U;
constexpr std::size_t kMinExpectedUniqueJpStrings = 51914U;

std::string FindScriptLpkPath() {
    const std::string game_dir = oretsuba::apps::DefaultGameDirectory();
    if (game_dir.empty()) {
        return {};
    }
    const fs::path candidate = fs::u8path(game_dir) / "SCRIPT.LPK";
    if (!fs::is_regular_file(candidate)) {
        return {};
    }
    return candidate.u8string();
}

}  // namespace

TEST(ExtractorTest, ExtractsAllScriptLpkStringsWithExactCorpusCountsAndZeroReplacementChars) {
    const std::string lpk_path = FindScriptLpkPath();
    ASSERT_FALSE(lpk_path.empty()) << "SCRIPT.LPK not found in default game directory";

    const fs::path temp_json =
        fs::temp_directory_path() / "oretsuba_ut_extracted_text.json";
    std::error_code ec;
    fs::remove(temp_json, ec);

    const oretsuba::extract::Extractor extractor;
    const auto summary = extractor.ExtractArchive(lpk_path, temp_json.u8string());

    EXPECT_EQ(summary.total_archive_entries, kExpectedArchiveEntries);
    EXPECT_EQ(summary.sob_file_count, kExpectedSobFiles);
    EXPECT_EQ(summary.sob0_count, kExpectedSob0Count);
    EXPECT_EQ(summary.sob1_count, kExpectedSob1Count);
    EXPECT_EQ(summary.sob2_count, kExpectedSob2Count);
    EXPECT_EQ(summary.scenario_script_count, kExpectedScenarioScripts);
    EXPECT_EQ(summary.message_count, kExpectedMessageCount);
    EXPECT_EQ(summary.voiced_dialogue_count, kExpectedVoicedDialogueCount);
    EXPECT_EQ(summary.unvoiced_dialogue_count, kExpectedUnvoicedDialogueCount);
    EXPECT_EQ(summary.voice_only_dialogue_count, kExpectedVoiceOnlyDialogueCount);
    EXPECT_EQ(summary.narration_count, kExpectedNarrationCount);
    EXPECT_EQ(summary.choice_block_count, kExpectedChoiceBlockCount);
    EXPECT_EQ(summary.choice_option_count, kExpectedChoiceOptionCount);
    EXPECT_EQ(summary.title_count, kExpectedTitleCount);
    EXPECT_EQ(summary.synopsis_count, kExpectedSynopsisCount);
    EXPECT_EQ(summary.ui_string_count, kExpectedUiStringCount);
    EXPECT_EQ(summary.total_entry_count, kExpectedTotalEntryCount);
    EXPECT_EQ(summary.unique_speaker_count, kExpectedUniqueSpeakerCount);
    EXPECT_GE(summary.unique_jp_string_count, kMinExpectedUniqueJpStrings);
    EXPECT_EQ(summary.replacement_char_count, 0U);

    ASSERT_TRUE(fs::is_regular_file(temp_json));
    const bj::value root_val = oretsuba::JsonParseFile(temp_json.u8string());
    ASSERT_TRUE(root_val.is_object());
    const auto& root_obj = root_val.as_object();
    ASSERT_FALSE(root_obj.empty());
    EXPECT_EQ(root_obj.begin()->key(), "s00_00.sob");
    ASSERT_NE(root_obj.if_contains("s01_01a.sob"), nullptr);
    ASSERT_NE(root_obj.if_contains("user_var.sob"), nullptr);

    fs::remove(temp_json, ec);
}
