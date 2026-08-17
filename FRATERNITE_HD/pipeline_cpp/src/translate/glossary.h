// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// GENERATED character glossary + system prompt. Do not hand-edit glossary.cpp.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace frat::translate {

// Insertion order is load-bearing — it seeds the cache key order.
const std::vector<std::pair<std::string, std::string>>& name_translations_ordered();
const std::unordered_map<std::string, std::string>& name_translations();

// Descending codepoint length so 園田 cannot eat into 園田Ｈ.
const std::vector<std::pair<std::string, std::string>>& names_by_len_desc();

extern const char* const SYSTEM_PROMPT;
const std::vector<std::string>& story_order();
inline const std::string MODEL_HAIKU = "claude-haiku-4-5-20251001";
inline const std::string MODEL_SONNET = "claude-sonnet-4-6";
inline const std::string MODEL_OPUS_46 = "claude-opus-4-6";
inline const std::string MODEL_OPUS_47 = "claude-opus-4-7";
inline const std::string MODEL = MODEL_OPUS_47;  // ← swap as needed
inline const std::string EFFORT = "high";

inline bool is_effort_model(const std::string& m) {
    return m == MODEL_SONNET || m == MODEL_OPUS_46 || m == MODEL_OPUS_47;
}

constexpr int CONTEXT_WINDOW = 50;  // previous EN lines for continuity

}  // namespace frat::translate
