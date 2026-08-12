// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// EXTRAVAGANZA ~Mushi Mederu Shoujo~ Matsuro CE character glossary and
// Anthropic system prompt: the NAME_TRANSLATIONS / SYSTEM_PROMPT data that
// drives 02_translate.
//
// The tables live in glossary.cpp; they are the pinned reference data for the
// whole translation run, so edit them deliberately -- changing an entry
// invalidates every cached line that used it.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exm::translate {

// Insertion-ordered (JP, EN) pairs.  The order is load-bearing: it decides
// cache seeding order and therefore the key order of the cache file on disk.
const std::vector<std::pair<std::string, std::string>>& name_translations_ordered();

// JP -> EN lookup map over the same data.
const std::unordered_map<std::string, std::string>& name_translations();

extern const char* const SYSTEM_PROMPT;

// Files are translated in this order; anything not listed follows in the
// order it appears in extracted_text.json.
const std::vector<std::string>& story_order();

// ── Available models (edit MODEL below to switch) ──
// Same rate card for Opus 4.6 and 4.7 ($5/$25 per Mtok), but 4.7's new
// tokenizer can emit up to 35% more tokens for the same text — 4.6 is
// effectively cheaper per request. Sonnet 4.6 stays the cheap baseline.
inline const std::string MODEL_HAIKU = "claude-haiku-4-5-20251001";
inline const std::string MODEL_SONNET = "claude-sonnet-4-6";
inline const std::string MODEL_OPUS_46 = "claude-opus-4-6";
inline const std::string MODEL_OPUS_47 = "claude-opus-4-7";
inline const std::string MODEL = MODEL_OPUS_47;  // ← swap as needed
inline const std::string EFFORT = "high";

inline bool is_effort_model(const std::string& m) {
    return m == MODEL_SONNET || m == MODEL_OPUS_46 || m == MODEL_OPUS_47;
}

constexpr int CONTEXT_WINDOW = 10;  // previous EN lines for continuity

}  // namespace exm::translate
