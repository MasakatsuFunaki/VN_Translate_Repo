// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// FAVORITE cast glossary + Gemini system prompt.
//
// Game: さくら、もゆ。 -as the Night's, Reincarnation- (FAVORITE / FVP, 2019).
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fav::translate {

// Insertion-ordered (JP, EN) pairs.  The order is load-bearing: these entries
// seed the resume cache in this order, so they decide the key order of
// translation_cache_gemini.json.
const std::vector<std::pair<std::string, std::string>>& name_translations_ordered();

// JP -> EN lookup map over the same data.
const std::unordered_map<std::string, std::string>& name_translations();

extern const char* const SYSTEM_PROMPT;

// ── Model ──
inline const std::string MODEL = "gemini-4-argon";

constexpr int CONTEXT_WINDOW = 40;  // previous EN lines for continuity
constexpr int CONTEXT_AFTER = 20;   // upcoming JP lines for lookahead

}  // namespace fav::translate
