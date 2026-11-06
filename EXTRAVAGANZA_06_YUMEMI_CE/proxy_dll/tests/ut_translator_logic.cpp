// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Component tests for proxy_dll/src/translator_logic.{h,cpp}: BOM detection,
// backslash-escape expansion, and TSV buffer parsing.
#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using translator_logic::ComputeUiFontAdjustment;
using translator_logic::ExtractFxfWindowTitle;
using translator_logic::FlattenNewlinesInPlace;
using translator_logic::HasAsciiWords;
using translator_logic::LookupTranslation;
using translator_logic::ParseTsvBuffer;
using translator_logic::TranslationMap;
using translator_logic::UiFontAdjustment;
using translator_logic::UnescapeInPlace;
using translator_logic::Utf8BomLen;
using translator_logic::VisibleAsciiLen;

namespace {

constexpr std::size_t kExpectedBomLen = 3;
constexpr char kUtf8Bom0 = static_cast<char>(0xEF);
constexpr char kUtf8Bom1 = static_cast<char>(0xBB);
constexpr char kUtf8Bom2 = static_cast<char>(0xBF);

}  // namespace

TEST(TranslatorLogic, utf8_bom_detection) {
    std::string withBom;
    withBom.push_back(kUtf8Bom0);
    withBom.push_back(kUtf8Bom1);
    withBom.push_back(kUtf8Bom2);
    withBom.append("hello\tHello\n");
    EXPECT_EQ(Utf8BomLen(withBom.data(), withBom.size()), kExpectedBomLen);

    const char withoutBom[] = "hello\tHello\n";
    EXPECT_EQ(Utf8BomLen(withoutBom, sizeof(withoutBom) - 1), 0u);
    EXPECT_EQ(Utf8BomLen(nullptr, 0), 0u);
}

TEST(TranslatorLogic, unescape_basic_escapes) {
    std::string s = "a\\nb\\tc\\\\d\\re";
    UnescapeInPlace(s);
    EXPECT_EQ(s, "a\nb\tc\\d\re");
}

TEST(TranslatorLogic, unescape_preserves_unknown_and_trailing_backslashes) {
    std::string plain = "plain ascii";
    UnescapeInPlace(plain);
    EXPECT_EQ(plain, "plain ascii");

    std::string unknown = "a\\qb";
    UnescapeInPlace(unknown);
    EXPECT_EQ(unknown, "a\\qb");

    std::string trailing = "trailing\\";
    UnescapeInPlace(trailing);
    EXPECT_EQ(trailing, "trailing\\");
}

TEST(TranslatorLogic, parse_tsv_buffer_handles_empty_and_valid_rows) {
    TranslationMap emptyMap;
    EXPECT_EQ(ParseTsvBuffer("", 0, emptyMap), 0);
    EXPECT_TRUE(emptyMap.empty());

    TranslationMap map;
    const std::string tsv =
        "yumemi\\r\\nline1\tYumemi: \"Ah\"\\r\\n\n"
        "second\tSecond\r\n"
        "malformed_no_tab\n"
        "empty_val\t\n"
        "\tempty_key\n";
    const int count = ParseTsvBuffer(tsv.data(), tsv.size(), map);
    EXPECT_EQ(count, 2);
    EXPECT_EQ(map["yumemi\r\nline1"], "Yumemi: \"Ah\"\r\n");
    EXPECT_EQ(map["second"], "Second");
}

TEST(TranslatorLogic, lookup_translation_handles_choice_prompt_and_color_prefixes) {
    TranslationMap map;
    const std::string promptKey = "%#\x89\xA9\x83\x47\x83\x5E";
    map[promptKey] = "%Use Eternal Mode?";
    map["\x8E\x67\x97\x70"] = "Use\r\nMode";

    std::string out;
    const std::string runtimePrompt = "#\x89\xA9\x83\x47\x83\x5E";
    EXPECT_TRUE(LookupTranslation(map, runtimePrompt.c_str(), true, out));
    EXPECT_EQ(out, "#\x89\xA9Use Eternal Mode?");

    EXPECT_TRUE(LookupTranslation(map, "\x8E\x67\x97\x70", true, out));
    EXPECT_EQ(out, "Use Mode");

    EXPECT_TRUE(LookupTranslation(map, "\x8E\x67\x97\x70", false, out));
    EXPECT_EQ(out, "Use\r\nMode");

    EXPECT_FALSE(LookupTranslation(map, "unknown_key", true, out));
    EXPECT_FALSE(LookupTranslation(map, "", true, out));
    EXPECT_FALSE(LookupTranslation(map, nullptr, true, out));
}

TEST(TranslatorLogic, compute_ui_font_adjustment_scales_choices_and_skips_dialogue) {
    constexpr int kChoiceOrigFontSize = 24;
    constexpr int kChoiceOrigSpace = 1;
    constexpr int kAnimMaxChars = 12;
    constexpr int kFullMaxChars = 1000;
    constexpr int kExpectedStandardYOffset = 4;
    constexpr int kExpectedCompactYOffset = 5;

    const UiFontAdjustment shortAdj = ComputeUiFontAdjustment(
        "#\x89\xA9Use Eternal Mode?", true, kChoiceOrigFontSize, kChoiceOrigSpace, kAnimMaxChars);
    EXPECT_TRUE(shortAdj.shouldAdjust);
    EXPECT_EQ(shortAdj.newFontSize, translator_logic::kStandardChoiceFontSize);
    EXPECT_EQ(shortAdj.newSpace, translator_logic::kPatchedChoiceSpace);
    EXPECT_EQ(shortAdj.yOffset, kExpectedStandardYOffset);
    EXPECT_TRUE(shortAdj.expandMaxChars);

    const std::string longChoice(50, 'A');
    const UiFontAdjustment longAdj = ComputeUiFontAdjustment(
        longChoice.c_str(), true, kChoiceOrigFontSize, kChoiceOrigSpace, kFullMaxChars);
    EXPECT_TRUE(longAdj.shouldAdjust);
    EXPECT_EQ(longAdj.newFontSize, translator_logic::kCompactChoiceFontSize);
    EXPECT_EQ(longAdj.newSpace, translator_logic::kPatchedChoiceSpace);
    EXPECT_EQ(longAdj.yOffset, kExpectedCompactYOffset);
    EXPECT_FALSE(longAdj.expandMaxChars);

    const UiFontAdjustment dialogueAdj = ComputeUiFontAdjustment(
        "Already patched PrintMessage line", false, 14, -4, kFullMaxChars);
    EXPECT_FALSE(dialogueAdj.shouldAdjust);

    const UiFontAdjustment numericAdj = ComputeUiFontAdjustment(
        "100%", false, kChoiceOrigFontSize, kChoiceOrigSpace, kFullMaxChars);
    EXPECT_FALSE(numericAdj.shouldAdjust);
    EXPECT_TRUE(HasAsciiWords("Hello world"));
    EXPECT_FALSE(HasAsciiWords("2026/11/05"));
    EXPECT_EQ(VisibleAsciiLen("#\x89\xA9Test"), 4u);
}

TEST(TranslatorLogic, extract_fxf_window_title_decodes_xor_and_preserves_spaces) {
    const std::string plain =
        "\r\nwindowTitleTaiken EXTRAVAGANZA Yumemi Ryoujoku CE Trial\r\n"
        "windowTitle EXTRAVAGANZA Yumemi Ryoujoku CE\r\n";
    std::vector<std::uint8_t> encoded(plain.size());
    for (std::size_t i = 0; i < plain.size(); ++i) {
        encoded[i] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(plain[i]) ^ 0xFFu);
    }
    EXPECT_EQ(ExtractFxfWindowTitle(encoded.data(), encoded.size()),
              "EXTRAVAGANZA Yumemi Ryoujoku CE");
    EXPECT_EQ(ExtractFxfWindowTitle(nullptr, 0), "");
}
