// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-3: runtime-byte variant expansion and the CP932 TSV writer.
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "build_tsv/build_tsv.h"
#include "build_tsv/name_fixups.h"
#include "common/util.h"
#include "test_support.h"

using namespace frat;
using namespace frat::build_tsv;

namespace {

using Pairs = std::vector<std::pair<std::string, std::string>>;

std::string decode_tsv(const std::string& path) {
    const Bytes raw = read_file(path);
    return *cp932_to_utf8_strict(raw.data(), raw.size());
}

}  // namespace

TEST(BuildTsv, StripRuby_ReplacesWithHeadWordOnly) {
    EXPECT_EQ(strip_ruby("リビングに顔を出した≪美桜／姉さん≫が、≪瑛／あきら≫も"),
              "リビングに顔を出した美桜が、瑛も");
    EXPECT_EQ(strip_ruby("≪あ／い≫≪う／え≫"), "あう");
    // An unterminated ≪ is left verbatim.
    EXPECT_EQ(strip_ruby("≪未完"), "≪未完");
    EXPECT_EQ(strip_ruby("≪／い≫"), "≪／い≫");   // head word needs >= 1 cp
    EXPECT_EQ(strip_ruby("≪あ／≫"), "≪あ／≫");   // reading needs >= 1 cp
    EXPECT_EQ(strip_ruby("ruby-free"), "ruby-free");
}

TEST(BuildTsv, SpeakerMatch_NonGreedyNameThenQuote) {
    std::string name, quote;
    ASSERT_TRUE(speaker_match("／美桜「あ」", name, quote));
    EXPECT_EQ(name, "美桜");
    EXPECT_EQ(quote, "「あ」");
    ASSERT_TRUE(speaker_match("美桜「あ」", name, quote));
    EXPECT_EQ(name, "美桜");
    // The greedy ／? backtracks to empty, so the slash itself becomes group 1.
    ASSERT_TRUE(speaker_match("／「あ」", name, quote));
    EXPECT_EQ(name, "／");
    EXPECT_EQ(quote, "「あ」");
    // A quote containing a newline still matches to end of string.
    ASSERT_TRUE(speaker_match("美桜「あ\nい」", name, quote));
    EXPECT_EQ(quote, "「あ\nい」");
    // \s is excluded from group 1, and group 1 needs at least one character.
    EXPECT_FALSE(speaker_match("美 桜「あ」", name, quote));
    EXPECT_FALSE(speaker_match("「あ」", name, quote));
    EXPECT_FALSE(speaker_match("ふつう", name, quote));
}

TEST(BuildTsv, EnsureCloseBracket_OnlyWhenUnbalanced) {
    EXPECT_EQ(ensure_close_bracket("美桜「あ"), "美桜「あ」");
    EXPECT_EQ(ensure_close_bracket("美桜「あ」"), "美桜「あ」");
    EXPECT_EQ(ensure_close_bracket("no brackets"), "no brackets");
}

TEST(BuildTsv, Variants_EmissionOrderAndDedup) {
    // Emission order decides the TSV's line order, so it is pinned here: the
    // dedup runs through an insertion-ordered set, and a hash set would
    // reshuffle the closed-bracket block from run to run.
    const std::string jp = "／≪美桜／姉さん≫「大智、終わった？";
    const std::string en = "／Mio「Daichi, are you done?";
    const std::string mystery_en = "???「Daichi, are you done?";
    EXPECT_EQ(variants(jp, en),
              (Pairs{
                  {"／≪美桜／姉さん≫「大智、終わった？", en},
                  {"／美桜「大智、終わった？", en},
                  {"≪美桜／姉さん≫「大智、終わった？", en},
                  {"美桜「大智、終わった？", en},
                  {"／≪美桜／姉さん≫「大智、終わった？」", en},
                  {"／美桜「大智、終わった？」", en},
                  {"≪美桜／姉さん≫「大智、終わった？」", en},
                  {"美桜「大智、終わった？」", en},
                  {"？？？「大智、終わった？", mystery_en},
                  {"？？？「大智、終わった？」", mystery_en},
              }));
}

TEST(BuildTsv, Variants_TrailingPeriodOnlyWhenUnterminated) {
    EXPECT_EQ(variants("ふつうの地の文", "Plain narration"),
              (Pairs{{"ふつうの地の文", "Plain narration"},
                     {"ふつうの地の文。", "Plain narration"}}));
    // Already terminated -> no 。 variant.
    EXPECT_EQ(variants("ふつうの地の文。", "Plain narration."),
              (Pairs{{"ふつうの地の文。", "Plain narration."}}));
    EXPECT_EQ(variants("ふつうの地の文…", "Plain narration..."),
              (Pairs{{"ふつうの地の文…", "Plain narration..."}}));
}

TEST(BuildTsv, Variants_MysteryQuoteMinimum14Cp932Bytes) {
    // 「ああ」 is 8 CP932 bytes -- below the floor, so no ？？？ variant.
    EXPECT_EQ(variants("美桜「ああ」", "Mio「Ah ah」"),
              (Pairs{{"美桜「ああ」", "Mio「Ah ah」"}}));
    // 「あいうえおか」 is 16 bytes -- above it.
    EXPECT_EQ(variants("美桜「あいうえおか」", "Mio「Hello there friend」"),
              (Pairs{{"美桜「あいうえおか」", "Mio「Hello there friend」"},
                     {"？？？「あいうえおか」", "???「Hello there friend」"}}));
}

TEST(BuildTsv, Cp932Safe_UnmappableBecomesQuestionMark_NotBestFit) {
    // WideCharToMultiByte(932, 0, ...) would best-fit this to 'e'; an
    // unmappable character has to become '?' instead.
    EXPECT_EQ(cp932_safe("Fraternit\xC3\xA9"), "Fraternit?");
    EXPECT_EQ(cp932_safe("\xE2\x80\x94"), "\xE2\x80\x95");  // U+2014 -> U+2015
    EXPECT_EQ(cp932_safe("\xE2\x80\x98x\xE2\x80\x99"), "'x'");
    EXPECT_EQ(cp932_safe("a\xC2\xA0" "b"), "a b");  // U+00A0 -> ' '
    // Curly double quotes ARE CP932-encodable, so they survive verbatim.
    EXPECT_EQ(cp932_safe("\xE2\x80\x9C" "x" "\xE2\x80\x9D"), "\xE2\x80\x9C" "x" "\xE2\x80\x9D");
}

TEST(BuildTsv, EscapeForTsv_OrderBackslashThenTab) {
    // Backslash doubled FIRST, so the tab escape's own backslash is not
    // doubled again.  No newline escaping: flatten already removed them.
    EXPECT_EQ(escape_for_tsv("a\\b\tc"), "a\\\\b\\tc");
    EXPECT_EQ(escape_for_tsv("plain"), "plain");
}

TEST(BuildTsv, Flatten_StripsU3000AsWellAsAscii) {
    EXPECT_EQ(flatten("  a\r\nb\rc\nd\xE3\x80\x80"), "a b c d");
}

TEST(BuildTsv, CleanEn_UsesThe26EntryTable_AppliesAllNotJustFirst) {
    EXPECT_EQ(NameFixups().size(), 26u);
    EXPECT_EQ(clean_en("園田Ｈ and 大智 and 園田"), "Sonoda (H) and Daichi and Sonoda");
    // Fast path: nothing in [0x3040, 0x9FFF] -> returned untouched.
    EXPECT_EQ(clean_en("no japanese here"), "no japanese here");
}

TEST(BuildTsv, EndToEnd_KeyFallthroughSemanticsAndTsvBytes) {
    // The key fallback chains fall through on EMPTY values, not just on
    // missing keys.
    const char* json_text = R"JSON({
      "a.ypf": {
        "strings": [],
        "lines": [
          {"text": "", "content": "内容だ。", "translated": "Content."},
          {"text": "同じだ", "translated": "同じだ"},
          {"text": "タブ\tある。", "translated": "Has\ta tab."}
        ]
      }
    })JSON";
    frat_test::ScratchDir dir("tsv");
    const std::string in = dir / "translated_text.json";
    const std::string out = dir / "table.tsv";
    write_file(in, std::string(json_text));
    ASSERT_EQ(run_build(in, out), 0);

    // "同じだ" is skipped (jp == en); the empty "text" falls through to
    // "content"; the tab is escaped on BOTH sides.
    EXPECT_EQ(decode_tsv(out), "内容だ。\tContent.\nタブ\\tある。\tHas\\ta tab.\n");
}
