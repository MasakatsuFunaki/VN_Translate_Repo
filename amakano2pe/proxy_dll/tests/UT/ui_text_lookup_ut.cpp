// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "ui_text_lookup.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace {

using translator_logic::LookupStatelessUiText;
using translator_logic::NormalizeProtagonistDisplayName;
using translator_logic::TranslateProtagonistNameToken;
using translator_logic::TryFormatProtagonistConfirmationPrompt;
using translator_logic::TryTranslateStatelessUiText;

constexpr std::string_view kWakuraJp = "和倉";
constexpr std::string_view kKenichiJp = "賢一";
constexpr std::string_view kFullNameAsciiSpaceJp = "和倉 賢一";
constexpr std::string_view kFullNameIdeographicSpaceJp = "和倉\u3000賢一";
constexpr std::string_view kFullNameCompactJp = "和倉賢一";

constexpr std::string_view kConfirmSuffixSanJp =
    "さんでお間違いないですか？";
constexpr std::string_view kConfirmSuffixYoroshiiJp =
    "さんでよろしいですか？";

TEST(UiTextLookup, NormalizesProtagonistDefaultAndCustomNameCombinations) {
    EXPECT_EQ(TranslateProtagonistNameToken(kWakuraJp), "Wakura");
    EXPECT_EQ(TranslateProtagonistNameToken(kKenichiJp), "Kenichi");
    EXPECT_EQ(TranslateProtagonistNameToken("Alex"), "Alex");

    EXPECT_EQ(NormalizeProtagonistDisplayName(kFullNameAsciiSpaceJp),
              "Wakura Kenichi");
    EXPECT_EQ(NormalizeProtagonistDisplayName(kFullNameIdeographicSpaceJp),
              "Wakura Kenichi");
    EXPECT_EQ(NormalizeProtagonistDisplayName(kFullNameCompactJp),
              "Wakura Kenichi");
    EXPECT_EQ(NormalizeProtagonistDisplayName(
                  std::string(kWakuraJp) + " Alex"),
              "Wakura Alex");
    EXPECT_EQ(NormalizeProtagonistDisplayName(
                  "Smith " + std::string(kKenichiJp)),
              "Smith Kenichi");
    EXPECT_EQ(NormalizeProtagonistDisplayName("John Doe"), "John Doe");
}

TEST(UiTextLookup,
     FormatsDynamicProtagonistConfirmationPromptsAcrossDelimitersAndWrappers) {
    const std::string default_prompt =
        std::string(kFullNameAsciiSpaceJp) + " " +
        std::string(kConfirmSuffixSanJp);
    EXPECT_EQ(TryFormatProtagonistConfirmationPrompt(default_prompt),
              std::optional<std::string>("Is Wakura Kenichi correct?"));

    const std::string patched_fes_default_prompt =
        "Is " + std::string(kFullNameAsciiSpaceJp) + " correct?";
    EXPECT_EQ(TryTranslateStatelessUiText(patched_fes_default_prompt),
              std::optional<std::string>("Is Wakura Kenichi correct?"));

    const std::string compact_prompt =
        std::string(kFullNameCompactJp) + std::string(kConfirmSuffixSanJp);
    EXPECT_EQ(TryFormatProtagonistConfirmationPrompt(compact_prompt),
              std::optional<std::string>("Is Wakura Kenichi correct?"));

    const std::string bracketed_newline_prompt =
        "「" + std::string(kFullNameIdeographicSpaceJp) +
        "」\\n" + std::string(kConfirmSuffixYoroshiiJp);
    EXPECT_EQ(LookupStatelessUiText(bracketed_newline_prompt),
              std::optional<std::string>("Is Wakura Kenichi correct?"));

    const std::string custom_prompt =
        "Alex Mercer " + std::string(kConfirmSuffixSanJp);
    EXPECT_EQ(LookupStatelessUiText(custom_prompt),
              std::optional<std::string>("Is Alex Mercer correct?"));

    EXPECT_EQ(LookupStatelessUiText(
                  "[選択肢]\u3000Go Home"),
              std::optional<std::string>("[Choice] Go Home"));
    EXPECT_EQ(LookupStatelessUiText(""), std::nullopt);
    EXPECT_EQ(LookupStatelessUiText("Ordinary story dialogue line."),
              std::nullopt);
}

}  // namespace
