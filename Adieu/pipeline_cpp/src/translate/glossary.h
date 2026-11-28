// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Adieu character glossary, Gemini system prompt, and chronological story order.
#pragma once

#include "vn/llm/llm_types.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace adi::translate {

// Insertion-ordered (JP, EN) speaker pairs synchronized 1:1 with the distinct
// `speaker` tags emitted in extracted_text.json. Insertion order seeds the
// initial key order of translation_cache_gemini.json.
const std::vector<std::pair<std::string, std::string>>& name_translations_ordered();

// JP -> EN lookup map over `name_translations_ordered()`.
const std::unordered_map<std::string, std::string>& name_translations();

// Additional non-speaker default #NAMAE / prose name seeds (such as
// "広介" -> "Kousuke" and unspaced heroines' given names) seeded into the
// translation cache alongside `name_translations_ordered()` without polluting
// speaker-only coverage checks.
const std::vector<std::pair<std::string, std::string>>& default_seed_translations();

extern const char* const SYSTEM_PROMPT;

// Chronological scene translation order (`_start`, `_menu`, `_manual`,
// `seen900`, `seen020`, ...); unlisted scenes follow deterministically.
const std::vector<std::string>& story_order();

inline const std::string MODEL{vn::llm::kDefaultModel};
inline const std::string EFFORT{vn::llm::kDefaultThinkingLevel};

inline bool is_effort_model(const std::string& m) {
    return m == MODEL;
}

// Sliding context window of preceding translated lines per scene.
constexpr int CONTEXT_WINDOW = 50;

// Lookahead window of upcoming scene lines appended after <lines_to_translate>.
constexpr int CONTEXT_AFTER_WINDOW = 15;

}  // namespace adi::translate
