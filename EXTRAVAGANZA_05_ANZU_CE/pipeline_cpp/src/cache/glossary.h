// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Welcome to AnzU! CE character glossary and Gemini system prompt.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exm::translate {

const std::vector<std::pair<std::string, std::string>>& name_translations_ordered();
const std::unordered_map<std::string, std::string>& name_translations();
const std::vector<std::pair<std::string, std::string>>& character_names();
const std::vector<std::pair<std::string, std::string>>& glossary_terms();

extern const char* const SYSTEM_PROMPT;
const std::string& glossary_prompt();

const std::vector<std::string>& story_order();

inline const std::string MODEL = "gemini-4-argon";

constexpr int CONTEXT_WINDOW = 40;
constexpr int CONTEXT_AFTER = 20;

}  // namespace exm::translate
