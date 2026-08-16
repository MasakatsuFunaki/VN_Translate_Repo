// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Amakano 2 ~Perfect Edition~ character glossary + Anthropic prompt.
//
// The tables in glossary.cpp are the authoritative JP -> EN name mapping:
// speaker_gate asserts full bidirectional coverage against the NAME lines in
// extracted_text.json, so every entry has to match a real speaker and every
// speaker has to have an entry.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ama::translate {

// Insertion-ordered (JP, EN) pairs.  The order is load-bearing: it is the
// order the cache is seeded in, and therefore the key order of
// translation_cache_anthropic.json.
const std::vector<std::pair<std::string, std::string>>& name_translations_ordered();

// JP -> EN lookup map over the same data.
const std::unordered_map<std::string, std::string>& name_translations();

extern const char* const SYSTEM_PROMPT;

// ── Available models (edit MODEL below to switch) ──
// Same rate card for Opus 4.6 and 4.7 ($5/$25 per Mtok), but 4.7's new
// tokenizer can emit up to 35% more tokens for the same text — 4.6 is
// effectively cheaper per request. Sonnet 4.6 stays the cheap baseline.
inline const std::string MODEL_HAIKU = "claude-haiku-4-5-20251001";
inline const std::string MODEL_SONNET = "claude-sonnet-4-6";
inline const std::string MODEL_OPUS_46 = "claude-opus-4-6";
inline const std::string MODEL_OPUS_47 = "claude-opus-4-7";
inline const std::string MODEL = MODEL_OPUS_47;  // ← swap as needed

// Effort level: controls token spend.  Supported on the Sonnet/Opus models
// above; NOT supported on Haiku 4.5.
inline const std::string EFFORT = "high";

inline bool is_effort_model(const std::string& m) {
    return m == MODEL_SONNET || m == MODEL_OPUS_46 || m == MODEL_OPUS_47;
}

// Sliding context window: how many previous translations to include.
constexpr int CONTEXT_WINDOW = 50;

}  // namespace ama::translate
