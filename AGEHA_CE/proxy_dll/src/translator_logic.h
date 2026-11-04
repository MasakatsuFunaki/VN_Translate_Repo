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
#include <string>
#include <unordered_map>

namespace translator_logic {

using TranslationMap = std::unordered_map<std::string, std::string>;

std::size_t Utf8BomLen(const char* buf, std::size_t len);

void UnescapeInPlace(std::string& s);

int ParseTsvBuffer(const char* buf, std::size_t len, TranslationMap& out);

}  // namespace translator_logic
