// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "runtime_hooks.h"
#include "translator_logic.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace oretsuba::proxy {
namespace {

void AppendU32Le(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    std::array<std::uint8_t, sizeof(v)> bytes = {};
    std::memcpy(bytes.data(), &v, sizeof(v));
    buf.insert(buf.end(), bytes.begin(), bytes.end());
}

TEST(TsvParserUnitTest, LoadsEntriesAndNormalizesKeysAcrossUtf8AndCp932) {
    TranslationCatalog catalog;
    const std::string tsv_data =
        "\xEF\xBB\xBF"
        "# comment line\n"
        "タカシ\tTakashi\n"
        "「ここはグレタガルドなのか？」\t\"Is this really Gretagard?\"\n"
        "冬の冷たい風が吹いていた。\tA cold winter wind was blowing.\n";

    const std::size_t loaded = catalog.LoadFromString(tsv_data);
    EXPECT_EQ(loaded, 3U);
    EXPECT_EQ(catalog.Size(), 3U);

    const auto utf8_hit = catalog.LookupTranslation("「ここはグレタガルドなのか？」");
    ASSERT_TRUE(utf8_hit.has_value());
    EXPECT_EQ(*utf8_hit, "\"Is this really Gretagard?\"");

    const std::string cp932_key =
        TranslationCatalog::Utf8ToCp932("　「ここはグレタガルドなのか？」");
    const auto cp932_hit = catalog.LookupTranslation(cp932_key);
    ASSERT_TRUE(cp932_hit.has_value());
    EXPECT_EQ(*cp932_hit, "\"Is this really Gretagard?\"");
}

TEST(TsvParserUnitTest, FormatsSpeakerVoiceTagAndBodyPreservingVoiceId) {
    TranslationCatalog catalog;
    catalog.LoadFromString(
        "タカシ\tTakashi\n"
        "「ここはどこだ？」\t\"Where am I right now?\"\n");

    const std::string raw_cp932 = TranslationCatalog::Utf8ToCp932(
        "#タカシ\\00_01_001$&$「ここはどこだ？」$");

    std::array<char, kDefaultScratchBufferCapacity> out_buf = {};
    EXPECT_TRUE(catalog.FormatDialogueOrNarration(
        raw_cp932, out_buf.data(), out_buf.size()));
    EXPECT_STREQ(out_buf.data(), "#Takashi\\00_01_001$&$\"Where am I right now?\"$");
}

TEST(TsvParserUnitTest, InPlaceDollarMutationDoesNotCorruptCatalogOrRepeatLookups) {
    TranslationCatalog catalog;
    catalog.LoadFromString(
        "明日香\tAsuka\n"
        "「おはよう、タカシくん！」\t\"Good morning, Takashi-kun!\"\n");

    const std::string raw_cp932 = TranslationCatalog::Utf8ToCp932(
        "#明日香\\01_02_003$&$「おはよう、タカシくん！」$");

    std::array<char, kDefaultScratchBufferCapacity> scratch_one = {};
    ASSERT_TRUE(catalog.FormatDialogueOrNarration(
        raw_cp932, scratch_one.data(), scratch_one.size()));

    // Simulate 0x00455b49: mov byte ptr [ebx], 0 on the first '$'
    char* first_dollar = std::strchr(scratch_one.data(), '$');
    ASSERT_NE(first_dollar, nullptr);
    *first_dollar = '\0';
    EXPECT_STREQ(scratch_one.data(), "#Asuka\\01_02_003");

    // Subsequent call with the same input must still produce the full string
    std::array<char, kDefaultScratchBufferCapacity> scratch_two = {};
    ASSERT_TRUE(catalog.FormatDialogueOrNarration(
        raw_cp932, scratch_two.data(), scratch_two.size()));
    EXPECT_STREQ(
        scratch_two.data(),
        "#Asuka\\01_02_003$&$\"Good morning, Takashi-kun!\"$");
}

TEST(TsvParserUnitTest, PreservesLeadingControlTagsAndStripsInlineRubyForLookup) {
    TranslationCatalog catalog;
    catalog.LoadFromString(
        "異世界の空を見上げた。\tI looked up at the sky of another world.\n");

    const std::string raw_with_controls_and_ruby = TranslationCatalog::Utf8ToCp932(
        "#小鳩\\02_01_010$&${t01}<+2>\\　<\"異世界|グレタガルド\">の空を見上げた。$");

    const std::string formatted =
        catalog.TranslateMessageRecord(raw_with_controls_and_ruby);
    EXPECT_EQ(
        formatted,
        "#Kobato\\02_01_010$&${t01}<+2>\\I looked up at the sky of another world.$");
}

TEST(TsvParserUnitTest, WrapsLongEnglishLinesWithBackslashAt56Columns) {
    const std::string long_line =
        "The cold autumn breeze swept across the bustling shopping district of Yanagihara.";
    const std::string wrapped = TranslationCatalog::WrapProportionalLines(long_line, 56);
    const std::size_t bs_pos = wrapped.find('\\');
    ASSERT_NE(bs_pos, std::string::npos);
    EXPECT_LE(bs_pos, 56U);
    EXPECT_LE(wrapped.size() - (bs_pos + 1U), 56U);
}

TEST(TsvParserUnitTest, RewritesBinaryChoiceBlockWithUpdatedStrides) {
    TranslationCatalog catalog;
    catalog.LoadFromString(
        "声をかける\tCall out to her\n"
        "黙って見守る\tWatch in silence\n");

    const std::string opt1_cp932 = TranslationCatalog::Utf8ToCp932("声をかける");
    const std::string opt2_cp932 = TranslationCatalog::Utf8ToCp932("黙って見守る");

    std::vector<std::uint8_t> raw_choices;
    raw_choices.push_back(2U);  // count = 2
    raw_choices.push_back(1U);  // has_inline = 1

    const auto stride1 = static_cast<std::uint8_t>(2U + opt1_cp932.size() + 1U);
    raw_choices.push_back(stride1);
    raw_choices.push_back(0x10U);
    raw_choices.insert(raw_choices.end(), opt1_cp932.begin(), opt1_cp932.end());
    raw_choices.push_back(0U);

    const auto stride2 = static_cast<std::uint8_t>(2U + opt2_cp932.size() + 1U);
    raw_choices.push_back(stride2);
    raw_choices.push_back(0x20U);
    raw_choices.insert(raw_choices.end(), opt2_cp932.begin(), opt2_cp932.end());
    raw_choices.push_back(0U);

    RuntimeHookManager hook_mgr(&catalog);
    std::vector<std::uint8_t> rewritten;
    ASSERT_TRUE(hook_mgr.RewriteChoiceBlock(raw_choices.data(), rewritten));
    ASSERT_GE(rewritten.size(), 6U);
    EXPECT_EQ(rewritten[0], 2U);
    EXPECT_EQ(rewritten[1], 1U);

    const std::uint8_t new_stride1 = rewritten[2];
    EXPECT_EQ(rewritten[3], 0x10U);
    const char* str1 = reinterpret_cast<const char*>(rewritten.data() + 4);
    EXPECT_STREQ(str1, "Call out to her");
    EXPECT_EQ(new_stride1, 2U + std::strlen(str1) + 1U);

    const std::size_t off2 = 2U + new_stride1;
    ASSERT_LT(off2 + 2U, rewritten.size());
    const std::uint8_t new_stride2 = rewritten[off2];
    EXPECT_EQ(rewritten[off2 + 1U], 0x20U);
    const char* str2 = reinterpret_cast<const char*>(rewritten.data() + off2 + 2U);
    EXPECT_STREQ(str2, "Watch in silence");
    EXPECT_EQ(new_stride2, 2U + std::strlen(str2) + 1U);
}

TEST(TsvParserUnitTest, PatchesSob0Subtable4StringPointersAndSkipsSceneTitleHeaders) {
    TranslationCatalog catalog;
    catalog.LoadFromString(
        "第１章　プロローグ\tChapter 1: Prologue\n"
        "設定画面を開く\tOpen Configuration\n");

    const std::string s1_cp932 = TranslationCatalog::Utf8ToCp932("第１章　プロローグ");
    const std::string s2_cp932 = TranslationCatalog::Utf8ToCp932("設定画面を開く");

    constexpr std::uint32_t kCodeSize = 0x40;
    const std::uint32_t s1_off = kCodeSize;
    const std::uint32_t s2_off = s1_off + static_cast<std::uint32_t>(s1_cp932.size()) + 1U;
    const std::uint32_t total_size = s2_off + static_cast<std::uint32_t>(s2_cp932.size()) + 1U;

    std::vector<std::uint8_t> loaded_image(total_size, 0);
    std::memcpy(loaded_image.data() + s1_off, s1_cp932.c_str(), s1_cp932.size() + 1U);
    std::memcpy(loaded_image.data() + s2_off, s2_cp932.c_str(), s2_cp932.size() + 1U);

    // Build SOB0 header with 5 subtables; subtables 0..3 empty, subtable 4 has 1 entry
    std::vector<std::uint8_t> reloc_payload;
    for (int t = 0; t < 4; ++t) {
        AppendU32Le(reloc_payload, 0U);
    }
    AppendU32Le(reloc_payload, 1U);
    AppendU32Le(reloc_payload, 0x10U);
    AppendU32Le(reloc_payload, s2_off);

    std::vector<std::uint8_t> sob_file = {'S', 'O', 'B', '0'};
    AppendU32Le(sob_file, static_cast<std::uint32_t>(reloc_payload.size()));
    sob_file.insert(sob_file.end(), reloc_payload.begin(), reloc_payload.end());
    AppendU32Le(sob_file, kCodeSize);
    AppendU32Le(sob_file, total_size);

    RuntimeHookManager hook_mgr(&catalog);
    const std::size_t patched =
        hook_mgr.PatchLoadedSobStringTable(loaded_image.data(), sob_file.data());
    EXPECT_EQ(patched, 1U);

    std::uint32_t patched_ptr_val = 0;
    std::memcpy(&patched_ptr_val, loaded_image.data() + 0x10U, sizeof(patched_ptr_val));
    const auto* patched_str = reinterpret_cast<const char*>(
        static_cast<std::uintptr_t>(patched_ptr_val));
    ASSERT_NE(patched_str, nullptr);
    EXPECT_STREQ(patched_str, "Open Configuration");
}

TEST(TsvParserUnitTest, AppliesProportionalFontAndMessageWindowFlags) {
    std::array<std::uint8_t, 128> fake_font_ggo = {};
    RuntimeHookManager::ApplyProportionalFontOverride(fake_font_ggo.data());
    EXPECT_EQ(fake_font_ggo[kFontGgoLfCharSetOffset], kShiftJisCharset);
    EXPECT_STREQ(
        reinterpret_cast<const char*>(fake_font_ggo.data() + kFontGgoLfFaceNameOffset),
        kProportionalFontFace);

    std::array<std::uint8_t, 128> fake_msg_wnd = {};
    RuntimeHookManager::ApplyMessageWindowProportionalFlag(fake_msg_wnd.data());
    EXPECT_EQ(fake_msg_wnd[kMsgWndProportionalFlagOffset], 1U);
}

TEST(TsvParserUnitTest, PreservesBackslashBeforeWordsStartingWithTAndNWithoutTabCorruption) {
    TranslationCatalog catalog;
    const std::string tsv_data =
        "行１\tI'll stroll off to\\the neighborhood park and\\their temporary world.\n"
        "行２\tI immediately\\think of vanishing from\\this world.\n"
        "行３\tI've been calling your name for a minute\\now, so do\\not ignore me.\n";
    ASSERT_EQ(catalog.LoadFromString(tsv_data), 3U);

    const auto hit1 = catalog.LookupTranslation("行１");
    ASSERT_TRUE(hit1.has_value());
    EXPECT_EQ(*hit1, "I'll stroll off to\\the neighborhood park and\\their temporary world.");
    EXPECT_EQ(hit1->find('\t'), std::string::npos);
    EXPECT_NE(hit1->find("\\the"), std::string::npos);
    EXPECT_NE(hit1->find("\\their"), std::string::npos);

    const auto hit2 = catalog.LookupTranslation("行２");
    ASSERT_TRUE(hit2.has_value());
    EXPECT_EQ(*hit2, "I immediately\\think of vanishing from\\this world.");
    EXPECT_EQ(hit2->find('\t'), std::string::npos);
    EXPECT_NE(hit2->find("\\think"), std::string::npos);
    EXPECT_NE(hit2->find("\\this"), std::string::npos);

    const auto hit3 = catalog.LookupTranslation("行３");
    ASSERT_TRUE(hit3.has_value());
    EXPECT_EQ(*hit3, "I've been calling your name for a minute\\now, so do\\not ignore me.");
    EXPECT_EQ(hit3->find('\t'), std::string::npos);
    EXPECT_NE(hit3->find("\\now"), std::string::npos);
    EXPECT_NE(hit3->find("\\not"), std::string::npos);

    const std::string rec1 = catalog.TranslateMessageRecord(
        TranslationCatalog::Utf8ToCp932("#タカシ\\01_01_001$&$行１$"));
    EXPECT_EQ(rec1.find('\t'), std::string::npos);
    EXPECT_NE(rec1.find("\\the"), std::string::npos);
    EXPECT_NE(rec1.find("\\their"), std::string::npos);

    const std::string rec3 = catalog.TranslateMessageRecord(
        TranslationCatalog::Utf8ToCp932("#タカシ\\01_01_003$&$行３$"));
    EXPECT_EQ(rec3.find('\t'), std::string::npos);
    EXPECT_NE(rec3.find("\\now"), std::string::npos);
    EXPECT_NE(rec3.find("\\not"), std::string::npos);
}

TEST(TsvParserUnitTest, PreservesTwoCharHeaderDelimiterAndHandlesCp932TrailByteBackslashInSpeaker) {
    TranslationCatalog catalog;
    catalog.LoadFromString(
        "ソラ\tSora\n"
        "「こんにちは」\t\"Hello there.\"\n"
        "「前半」$&「後半」\t\"First half.\"$&\"Second half.\"\n");

    const std::string two_char_delim_record = TranslationCatalog::Utf8ToCp932(
        "#タカシ\\01_02_003$&「こんにちは」$");
    EXPECT_EQ(
        catalog.TranslateMessageRecord(two_char_delim_record),
        "#Takashi\\01_02_003$&\"Hello there.\"$");

    const std::string single_dollar_with_body_pause = TranslationCatalog::Utf8ToCp932(
        "#タカシ\\01_02_004$「前半」$&「後半」$");
    EXPECT_EQ(
        catalog.TranslateMessageRecord(single_dollar_with_body_pause),
        "#Takashi\\01_02_004$\"First half.\"$&\"Second half.\"$");

    // In CP932, 'ソ' is 0x83 0x5C (where the second byte is ASCII '\\').
    const std::string sora_cp932 = TranslationCatalog::Utf8ToCp932("ソラ");
    ASSERT_EQ(sora_cp932.size(), 4U);
    ASSERT_EQ(static_cast<std::uint8_t>(sora_cp932[0]), 0x83U);
    ASSERT_EQ(static_cast<std::uint8_t>(sora_cp932[1]), 0x5CU);

    const std::string sora_record_cp932 = TranslationCatalog::Utf8ToCp932(
        "#ソラ\\02_03_004$&$「こんにちは」$");
    EXPECT_EQ(
        catalog.TranslateMessageRecord(sora_record_cp932),
        "#Sora\\02_03_004$&$\"Hello there.\"$");
}

}  // namespace
}  // namespace oretsuba::proxy
