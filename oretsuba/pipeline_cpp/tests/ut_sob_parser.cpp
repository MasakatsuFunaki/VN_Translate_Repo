// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "apps/paths.h"
#include "common/util.h"
#include "lpk/lpk_archive.h"
#include "sob/sob_parser.h"

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kSyntheticCodeSize = 64U;
constexpr std::uint32_t kSyntheticTitleCodeOff = 16U;
constexpr std::uint32_t kSyntheticTitlePtrOff = 12U;
constexpr std::uint32_t kSyntheticSynopsisCodeOff = 24U;
constexpr std::uint32_t kSyntheticSynopsisPtrOff = 20U;
constexpr std::uint32_t kSyntheticMsgCodeOff = 36U;
constexpr std::uint32_t kSyntheticMsgPtrOff = 32U;
constexpr std::uint32_t kSyntheticChoiceCodeOff = 48U;
constexpr std::uint32_t kSyntheticChoicePtrOff = 44U;
constexpr std::uint8_t kSyntheticBlockId = 1U;
constexpr std::uint16_t kSyntheticMsgIndex = 7U;
constexpr std::uint16_t kSyntheticChoiceId = 3U;
constexpr std::uint8_t kSyntheticChoiceCount = 2U;
constexpr std::size_t kExpectedSyntheticRecords = 5U;
constexpr std::size_t kExpectedUserVarUiRecords = 10U;
constexpr std::size_t kExpectedS0102cChoices = 2U;

void AppendU32Le(oretsuba::Bytes& out, std::uint32_t val) {
    const std::size_t pos = out.size();
    out.resize(pos + sizeof(val));
    std::memcpy(out.data() + pos, &val, sizeof(val));
}

void AppendU16Le(oretsuba::Bytes& out, std::uint16_t val) {
    const std::size_t pos = out.size();
    out.resize(pos + sizeof(val));
    std::memcpy(out.data() + pos, &val, sizeof(val));
}

void AppendCp932NullTerminated(oretsuba::Bytes& out, std::string_view utf8) {
    const oretsuba::Bytes cp932 = oretsuba::Utf8ToCp932(utf8);
    out.insert(out.end(), cp932.begin(), cp932.end());
    out.push_back(0U);
}

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

TEST(SobParserTest, ParseMessageRecordStringSplitsSpeakerVoiceAndBody) {
    const auto voiced = oretsuba::sob::ParseMessageRecordString(
        "#鷹志\\TAK0001$&「おはよう、小鳩」$");
    EXPECT_EQ(voiced.type, oretsuba::sob::SobEntryType::kDialogue);
    EXPECT_EQ(voiced.speaker, "鷹志");
    EXPECT_EQ(voiced.voice_id, "TAK0001");
    EXPECT_EQ(voiced.text, "「おはよう、小鳩」");

    const auto unvoiced = oretsuba::sob::ParseMessageRecordString(
        "#聖騎士ホーク$&「む！　なぜ私はこのような場所にッ！？」$");
    EXPECT_EQ(unvoiced.type, oretsuba::sob::SobEntryType::kDialogue);
    EXPECT_EQ(unvoiced.speaker, "聖騎士ホーク");
    EXPECT_TRUE(unvoiced.voice_id.empty());
    EXPECT_EQ(unvoiced.text, "「む！　なぜ私はこのような場所にッ！？」");

    const auto voice_only = oretsuba::sob::ParseMessageRecordString(
        "#\\Y101038$&「ホーク卿！」$");
    EXPECT_EQ(voice_only.type, oretsuba::sob::SobEntryType::kDialogue);
    EXPECT_TRUE(voice_only.speaker.empty());
    EXPECT_EQ(voice_only.voice_id, "Y101038");
    EXPECT_EQ(voice_only.text, "「ホーク卿！」");

    const auto narration = oretsuba::sob::ParseMessageRecordString(
        "　光が満ちていた。$");
    EXPECT_EQ(narration.type, oretsuba::sob::SobEntryType::kNarration);
    EXPECT_TRUE(narration.speaker.empty());
    EXPECT_TRUE(narration.voice_id.empty());
    EXPECT_EQ(narration.text, "　光が満ちていた。");
}

TEST(SobParserTest, ParsesSyntheticSob0ScriptWithTitleSynopsisDialogueAndChoices) {
    oretsuba::Bytes payload(kSyntheticCodeSize, 0U);

    const auto title_off = static_cast<std::uint32_t>(payload.size());
    AppendCp932NullTerminated(payload, "第１章　テストタイトル");

    const auto synopsis_off = static_cast<std::uint32_t>(payload.size());
    AppendCp932NullTerminated(payload, "　テスト用のあらすじテキストです。");

    const auto msg_off = static_cast<std::uint32_t>(payload.size());
    payload.push_back(kSyntheticBlockId);
    AppendU16Le(payload, kSyntheticMsgIndex);
    AppendCp932NullTerminated(payload, "#鷲介\\SHU0010$&「いらっしゃいませ」$");

    const auto choice_off = static_cast<std::uint32_t>(payload.size());
    payload.push_back(kSyntheticBlockId);
    AppendU16Le(payload, kSyntheticChoiceId);
    payload.push_back(kSyntheticChoiceCount);
    payload.push_back(0U);

    for (std::string_view opt_text : {"選択肢その一", "選択肢その二"}) {
        const oretsuba::Bytes cp932 = oretsuba::Utf8ToCp932(opt_text);
        const auto stride = static_cast<std::uint8_t>(cp932.size() + 3U);
        payload.push_back(stride);
        payload.push_back(0U);
        payload.insert(payload.end(), cp932.begin(), cp932.end());
        payload.push_back(0U);
    }

    oretsuba::Bytes reloc;
    AppendU32Le(reloc, 1U);
    AppendU32Le(reloc, kSyntheticSynopsisCodeOff);
    AppendU32Le(reloc, oretsuba::sob::kGlobalVarSynopsis);

    AppendU32Le(reloc, 2U);
    AppendU32Le(reloc, kSyntheticMsgCodeOff);
    AppendU32Le(reloc, oretsuba::sob::kSyscallMessage);
    AppendU32Le(reloc, kSyntheticChoiceCodeOff);
    AppendU32Le(reloc, oretsuba::sob::kSyscallChoice);

    AppendU32Le(reloc, 0U);

    AppendU32Le(reloc, 1U);
    AppendU32Le(reloc, kSyntheticTitleCodeOff);
    AppendU32Le(reloc, oretsuba::sob::kExtFuncSceneTitle);

    AppendU32Le(reloc, 4U);
    AppendU32Le(reloc, kSyntheticTitlePtrOff);
    AppendU32Le(reloc, title_off);
    AppendU32Le(reloc, kSyntheticSynopsisPtrOff);
    AppendU32Le(reloc, synopsis_off);
    AppendU32Le(reloc, kSyntheticMsgPtrOff);
    AppendU32Le(reloc, msg_off);
    AppendU32Le(reloc, kSyntheticChoicePtrOff);
    AppendU32Le(reloc, choice_off);

    AppendU32Le(reloc, 0U);

    oretsuba::Bytes sob_file = {'S', 'O', 'B', '0'};
    AppendU32Le(sob_file, static_cast<std::uint32_t>(reloc.size()));
    sob_file.insert(sob_file.end(), reloc.begin(), reloc.end());
    AppendU32Le(sob_file, kSyntheticCodeSize);
    AppendU32Le(sob_file, static_cast<std::uint32_t>(payload.size()));
    sob_file.insert(sob_file.end(), payload.begin(), payload.end());

    const oretsuba::sob::SobScriptParser parser;
    const auto parsed = parser.Parse("synthetic.sob", sob_file);

    EXPECT_EQ(parsed.format, oretsuba::sob::SobFormat::kSob0);
    EXPECT_TRUE(parsed.has_messages);
    EXPECT_EQ(parsed.choice_block_count, 1U);
    ASSERT_EQ(parsed.records.size(), kExpectedSyntheticRecords);

    EXPECT_EQ(parsed.records[0].type, oretsuba::sob::SobEntryType::kTitle);
    EXPECT_EQ(parsed.records[0].text, "第１章　テストタイトル");

    EXPECT_EQ(parsed.records[1].type, oretsuba::sob::SobEntryType::kSynopsis);
    EXPECT_EQ(parsed.records[1].text, "　テスト用のあらすじテキストです。");

    EXPECT_EQ(parsed.records[2].type, oretsuba::sob::SobEntryType::kDialogue);
    EXPECT_EQ(parsed.records[2].speaker, "鷲介");
    EXPECT_EQ(parsed.records[2].voice_id, "SHU0010");
    EXPECT_EQ(parsed.records[2].text, "「いらっしゃいませ」");
    EXPECT_EQ(parsed.records[2].record_index, kSyntheticMsgIndex);

    EXPECT_EQ(parsed.records[3].type, oretsuba::sob::SobEntryType::kChoice);
    EXPECT_EQ(parsed.records[3].text, "選択肢その一");
    EXPECT_EQ(parsed.records[4].type, oretsuba::sob::SobEntryType::kChoice);
    EXPECT_EQ(parsed.records[4].text, "選択肢その二");
}

TEST(SobParserTest, ParsesRealScenarioAndUserVarScriptsFromScriptLpk) {
    const std::string lpk_path = FindScriptLpkPath();
    ASSERT_FALSE(lpk_path.empty()) << "SCRIPT.LPK not found in default game directory";

    const oretsuba::lpk::LpkArchiveReader reader(lpk_path);
    const oretsuba::sob::SobScriptParser parser;

    const auto s01_02c = parser.Parse("s01_02c.sob", reader.ReadEntryByName("s01_02c.sob"));
    EXPECT_EQ(s01_02c.format, oretsuba::sob::SobFormat::kSob0);
    EXPECT_TRUE(s01_02c.has_messages);
    EXPECT_EQ(s01_02c.choice_block_count, 1U);

    std::vector<std::uint16_t> msg_indices;
    std::size_t choice_count = 0;
    for (const auto& rec : s01_02c.records) {
        if (rec.type == oretsuba::sob::SobEntryType::kDialogue ||
            rec.type == oretsuba::sob::SobEntryType::kNarration) {
            msg_indices.push_back(rec.record_index);
        }
        if (rec.type == oretsuba::sob::SobEntryType::kChoice) {
            ++choice_count;
        }
    }
    EXPECT_EQ(choice_count, kExpectedS0102cChoices);
    ASSERT_FALSE(msg_indices.empty());
    EXPECT_EQ(msg_indices.front(), 0U);
    for (std::size_t i = 1; i < msg_indices.size(); ++i) {
        EXPECT_GT(msg_indices[i], msg_indices[i - 1]);
    }

    const auto user_var = parser.Parse("user_var.sob", reader.ReadEntryByName("user_var.sob"));
    EXPECT_EQ(user_var.format, oretsuba::sob::SobFormat::kSob1);
    ASSERT_EQ(user_var.records.size(), kExpectedUserVarUiRecords);
    EXPECT_EQ(user_var.records.front().type, oretsuba::sob::SobEntryType::kUi);
    EXPECT_EQ(user_var.records.front().text, "クイックセーブ");
}
