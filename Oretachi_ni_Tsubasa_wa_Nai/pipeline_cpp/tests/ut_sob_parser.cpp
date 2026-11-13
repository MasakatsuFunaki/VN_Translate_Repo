// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "common/util.h"
#include "sob/sob_parser.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace {

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
constexpr std::size_t kChoiceOptionRecordOverhead = 3U;
constexpr std::uint32_t kSingleRelocEntryCount = 1U;
constexpr std::uint32_t kTwoRelocEntriesCount = 2U;
constexpr std::uint32_t kFourRelocEntriesCount = 4U;
constexpr std::size_t kExpectedSyntheticRecords = 5U;
constexpr std::size_t kTitleRecordIdx = 0U;
constexpr std::size_t kSynopsisRecordIdx = 1U;
constexpr std::size_t kDialogueRecordIdx = 2U;
constexpr std::size_t kFirstChoiceRecordIdx = 3U;
constexpr std::size_t kSecondChoiceRecordIdx = 4U;

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
        const auto stride =
            static_cast<std::uint8_t>(cp932.size() + kChoiceOptionRecordOverhead);
        payload.push_back(stride);
        payload.push_back(0U);
        payload.insert(payload.end(), cp932.begin(), cp932.end());
        payload.push_back(0U);
    }

    oretsuba::Bytes reloc;
    AppendU32Le(reloc, kSingleRelocEntryCount);
    AppendU32Le(reloc, kSyntheticSynopsisCodeOff);
    AppendU32Le(reloc, oretsuba::sob::kGlobalVarSynopsis);

    AppendU32Le(reloc, kTwoRelocEntriesCount);
    AppendU32Le(reloc, kSyntheticMsgCodeOff);
    AppendU32Le(reloc, oretsuba::sob::kSyscallMessage);
    AppendU32Le(reloc, kSyntheticChoiceCodeOff);
    AppendU32Le(reloc, oretsuba::sob::kSyscallChoice);

    AppendU32Le(reloc, 0U);

    AppendU32Le(reloc, kSingleRelocEntryCount);
    AppendU32Le(reloc, kSyntheticTitleCodeOff);
    AppendU32Le(reloc, oretsuba::sob::kExtFuncSceneTitle);

    AppendU32Le(reloc, kFourRelocEntriesCount);
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

    EXPECT_EQ(parsed.records[kTitleRecordIdx].type, oretsuba::sob::SobEntryType::kTitle);
    EXPECT_EQ(parsed.records[kTitleRecordIdx].text, "第１章　テストタイトル");

    EXPECT_EQ(parsed.records[kSynopsisRecordIdx].type, oretsuba::sob::SobEntryType::kSynopsis);
    EXPECT_EQ(parsed.records[kSynopsisRecordIdx].text, "　テスト用のあらすじテキストです。");

    EXPECT_EQ(parsed.records[kDialogueRecordIdx].type, oretsuba::sob::SobEntryType::kDialogue);
    EXPECT_EQ(parsed.records[kDialogueRecordIdx].speaker, "鷲介");
    EXPECT_EQ(parsed.records[kDialogueRecordIdx].voice_id, "SHU0010");
    EXPECT_EQ(parsed.records[kDialogueRecordIdx].text, "「いらっしゃいませ」");
    EXPECT_EQ(parsed.records[kDialogueRecordIdx].record_index, kSyntheticMsgIndex);

    EXPECT_EQ(parsed.records[kFirstChoiceRecordIdx].type, oretsuba::sob::SobEntryType::kChoice);
    EXPECT_EQ(parsed.records[kFirstChoiceRecordIdx].text, "選択肢その一");
    EXPECT_EQ(parsed.records[kSecondChoiceRecordIdx].type, oretsuba::sob::SobEntryType::kChoice);
    EXPECT_EQ(parsed.records[kSecondChoiceRecordIdx].text, "選択肢その二");
}
