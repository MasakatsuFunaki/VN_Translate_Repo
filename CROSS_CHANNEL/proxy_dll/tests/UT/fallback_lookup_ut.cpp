// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — translator_logic::LookupWithStrippedPrefix
//
// The TSV is built from the extract step's control-prefix strip, which keeps
// only 「」, …, ―, ～, and "real Japanese" chars at the start of a
// string and drops everything else. The engine still emits the
// un-stripped form at runtime, so a literal find() misses. The lookup
// mirrors that strip as a fallback: walk past leading non-kept bytes
// once, then retry. Tests both classes of field bug:
//
//   * "※男心を鷲掴む…"  (TSV key has no ※)
//   * "A定食は三時間目…" (TSV key has no leading A)

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <string>

using translator_logic::TranslationMap;
using translator_logic::LookupWithStrippedPrefix;

namespace {

// Helper to build CP932 byte strings without escape clutter.
std::string Bytes(std::initializer_list<unsigned char> bs) {
    std::string s;
    s.reserve(bs.size());
    for (auto b : bs) s.push_back(static_cast<char>(b));
    return s;
}

// CP932 byte sequences taken as ground truth from real proxy_log MISS
// lines (or the cp932 codec table), NEVER from memory. An earlier
// version of this file used 0x81 0xA8 for ※, which is actually ★ —
// both UT and CT happily passed while the game still showed raw JP.
const std::string kAnnoMark      = Bytes({0x81, 0xA6});   // ※ (U+203B)
const std::string kFullwidthA    = Bytes({0x82, 0x60});   // Ａ (U+FF21)
const std::string kBracketL      = Bytes({0x81, 0x75});   // 「 (U+300C)
const std::string kHiraganaA     = Bytes({0x82, 0xA0});   // あ (U+3042)
const std::string kKanjiTei      = Bytes({0x92, 0xE8});   // 定 (U+5B9A)

}  // namespace

TEST(FallbackLookup, LiteralKeyHitReturnsItsValue) {
    TranslationMap m{ {"PLAIN_JP", "plain EN"} };
    auto* en = LookupWithStrippedPrefix(m, "PLAIN_JP");
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(*en, "plain EN");
}

TEST(FallbackLookup, MissOnUnknownKeyReturnsNull) {
    TranslationMap m{ {"PLAIN_JP", "plain EN"} };
    EXPECT_EQ(LookupWithStrippedPrefix(m, "OTHER_JP"), nullptr);
}

TEST(FallbackLookup, AnnotationPrefixedKeyFallsBackToStrippedVariant_Regression) {
    // Field bug: TSV stored "男心を鷲掴…" (no ※) but engine sends
    // "※男心を鷲掴…". Literal find() missed → JP rendered raw.
    TranslationMap m{ {kKanjiTei + "FOO", "the-translation"} };
    auto* en = LookupWithStrippedPrefix(m, kAnnoMark + kKanjiTei + "FOO");
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(*en, "the-translation");
}

TEST(FallbackLookup, AsciiLetterPrefixedKeyFallsBackToStrippedVariant_Regression) {
    // Field bug: line "A定食は三時間目…" rendered as raw JP because
    // strip_control_prefix dropped the leading 'A' at extract time, so
    // the TSV key was "定食は三時間目…". The engine still emits the
    // un-stripped form. The walk-past-non-kept fallback recovers it.
    TranslationMap m{ {kKanjiTei + "FOO", "the-EN"} };
    auto* en = LookupWithStrippedPrefix(m, std::string("A") + kKanjiTei + "FOO");
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(*en, "the-EN");
}

TEST(FallbackLookup, FullwidthLetterPrefixedKeyFallsBackToStrippedVariant) {
    // Same class as ASCII A, but the leading char is a 2-byte CP932
    // fullwidth Ａ (0x82 0x60). Walk must advance by 2 bytes, not 1.
    TranslationMap m{ {kKanjiTei + "FOO", "the-EN"} };
    auto* en = LookupWithStrippedPrefix(m, kFullwidthA + kKanjiTei + "FOO");
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(*en, "the-EN");
}

TEST(FallbackLookup, MultipleNonKeptCharsAtFrontAreAllStripped) {
    // Engine sends "AB" + JP. The walk should skip BOTH leading ASCII
    // bytes before finding the kept-char start at position 2.
    TranslationMap m{ {kKanjiTei + "FOO", "the-EN"} };
    auto* en = LookupWithStrippedPrefix(m, std::string("AB") + kKanjiTei + "FOO");
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(*en, "the-EN");
}

TEST(FallbackLookup, LiteralBeatsStrippedWhenBothPresent) {
    // If a TSV key happens to literally start with ※ AND a separate
    // stripped variant also exists, the literal-key lookup must win.
    // Keeps stripping safe — never overrides correct entries.
    TranslationMap m{
        {kAnnoMark + kKanjiTei + "X", "decorated value"},
        {kKanjiTei + "X",              "stripped value"},
    };
    auto* en = LookupWithStrippedPrefix(m, kAnnoMark + kKanjiTei + "X");
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(*en, "decorated value");
}

TEST(FallbackLookup, LeadingBracketIsKeptNotStripped) {
    // 「 (0x81 0x75) is in the extractor's kept set — quoted dialogue
    // keeps its leading bracket. So the literal lookup is final; no
    // strip-and-retry, no false hit on the inner JP.
    TranslationMap m{ {kKanjiTei + "X", "should-not-leak"} };
    auto* en = LookupWithStrippedPrefix(m, kBracketL + kKanjiTei + "X");
    EXPECT_EQ(en, nullptr);
}

TEST(FallbackLookup, LeadingHiraganaIsKeptNotStripped) {
    // あ (0x82 0xA0) is hiragana → kept. Literal miss is final.
    TranslationMap m{ {kKanjiTei + "X", "should-not-leak"} };
    auto* en = LookupWithStrippedPrefix(m, kHiraganaA + kKanjiTei + "X");
    EXPECT_EQ(en, nullptr);
}

TEST(FallbackLookup, BareDecoratorByteIsNotStripped) {
    // jp == just the 2-byte decorator (no payload). Walk reaches end of
    // string with i == 2, so the substr would be empty — we don't search.
    TranslationMap m{ {"", "should not surface"} };
    EXPECT_EQ(LookupWithStrippedPrefix(m, kAnnoMark), nullptr);
}

TEST(FallbackLookup, KeyShorterThanPrefixIsNotMatched) {
    // jp is 1 ASCII byte. Walk skips it (non-kept), reaches end → no
    // retry. Just literal miss → null.
    TranslationMap m{ {"X", "x-en"} };
    EXPECT_EQ(LookupWithStrippedPrefix(m, "Z"), nullptr);
    auto* en = LookupWithStrippedPrefix(m, "X");
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(*en, "x-en");
}

TEST(FallbackLookup, SeededCaseFailureCardsBeatStrippedTsvFallback_Regression) {
    // In translations.tsv, strip_control_prefix stripped leading "ＣＡＳＥ"
    // so the TSV key became "　１　失敗" -> "1 - Failure". Seeding the full
    // unstripped CP932 key must make LookupWithStrippedPrefix return
    // "CASE 1 - Failure" while leaving the stripped TSV key intact.
    const std::string stripped_key = translator_logic::Utf8ToCp932("　１　失敗");
    const std::string full_key     = translator_logic::Utf8ToCp932("ＣＡＳＥ　１　失敗");

    TranslationMap m{ {stripped_key, "1 - Failure"} };
    ASSERT_GT(translator_logic::SeedScriptMenuTranslations(m), 0);

    const std::string* hit = LookupWithStrippedPrefix(m, full_key);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(*hit, "CASE 1 - Failure");
    EXPECT_EQ(m.at(stripped_key), "1 - Failure");
}

TEST(FallbackLookup, SeededNumericAndAsciiPrefixedLinesPreserveSemanticPrefix) {
    const std::string stripped_water = translator_logic::Utf8ToCp932("リットルの天然水。");
    const std::string full_water     = translator_logic::Utf8ToCp932("２リットルの天然水。");
    const std::string full_meal_a    = translator_logic::Utf8ToCp932(
        "Ａ定食は三時間目には売り切れるから、確実に食べたい時は一時間目の終わりと同時に食堂に赴くことになる。");

    TranslationMap m{ {stripped_water, "A liter of mineral water."} };
    translator_logic::SeedScriptMenuTranslations(m);

    const std::string* water_hit = LookupWithStrippedPrefix(m, full_water);
    ASSERT_NE(water_hit, nullptr);
    EXPECT_EQ(*water_hit, "Two liters of mineral water.");

    const std::string* meal_hit = LookupWithStrippedPrefix(m, full_meal_a);
    ASSERT_NE(meal_hit, nullptr);
    EXPECT_NE(meal_hit->find("Set meal A"), std::string::npos);
}

TEST(FallbackLookup, SeedScriptMenuTranslationsDoesNotOverwriteExistingEntries) {
    const std::string key = translator_logic::Utf8ToCp932("本編スタート");
    TranslationMap m{ {key, "User Custom Start"} };
    translator_logic::SeedScriptMenuTranslations(m);

    const std::string* hit = LookupWithStrippedPrefix(m, key);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(*hit, "User Custom Start");
}
