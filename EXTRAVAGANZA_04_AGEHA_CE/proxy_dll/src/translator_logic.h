// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pure, Win32-free TSV parsing and escape expansion logic for the AGEHA_CE
// runtime translator.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace translator_logic {

using TranslationMap = std::unordered_map<std::string, std::string>;

constexpr int kMinUiUnpatchedFontSize = 18;
constexpr std::size_t kMediumChoiceCharLimit = 44;
constexpr int kStandardChoiceFontSize = 16;
constexpr int kCompactChoiceFontSize = 14;
constexpr int kPatchedChoiceSpace = -4;

struct UiFontAdjustment {
    bool shouldAdjust = false;
    int newFontSize = 0;
    int newSpace = 0;
    int yOffset = 0;
    bool expandMaxChars = false;
};

std::size_t Utf8BomLen(const char* buf, std::size_t len);

void UnescapeInPlace(std::string& s);

int ParseTsvBuffer(const char* buf, std::size_t len, TranslationMap& out);

void FlattenNewlinesInPlace(std::string& s);

bool LookupTranslation(const TranslationMap& map,
                       const char* originalText,
                       bool singleLineMode,
                       std::string& outTranslated);

bool HasAsciiWords(const char* text);

std::size_t VisibleAsciiLen(const char* text);

UiFontAdjustment ComputeUiFontAdjustment(const char* text,
                                         bool wasTranslated,
                                         int curFontSize,
                                         int curSpace,
                                         int curMaxChars);

std::string ExtractFxfWindowTitle(const std::uint8_t* fxfBytes, std::size_t len);

}  // namespace translator_logic
