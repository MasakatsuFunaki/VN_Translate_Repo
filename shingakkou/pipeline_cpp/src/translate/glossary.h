// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shingakkou ~Noli me tangere~ character glossary, system prompt and story
// order -- the pinned data behind step 2.
//
// The tables in glossary.cpp are data, not code: every entry is character-exact
// and any edit changes the cache keys and the prompt bytes for the whole game.
//
// build_tsv reads the SAME name table rather than keeping its own copy of the
// 75 pairs, so there is nothing for two copies to drift on.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace shin::translate {

// Insertion-ordered (JP, EN) pairs.  The order is load-bearing: it is the
// cache seeding order, and therefore the cache file's key order.
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

// NOT the template's 10 -- shingakkou keeps a 50-line rolling window, and the
// window size is baked into the prompt bytes.
constexpr int CONTEXT_WINDOW = 50;

}  // namespace shin::translate
