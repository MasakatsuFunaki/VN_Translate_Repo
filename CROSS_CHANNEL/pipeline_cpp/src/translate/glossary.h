// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CROSS†CHANNEL -FINAL COMPLETE- character glossary (NAME_TRANSLATIONS) and
// the translator system prompt.
//
// The tables in glossary.cpp are pinned data, not code: the unit tests assert
// their size, their first entry and several exact pairs, because a silent edit
// here changes every name plate in the game and re-keys the cache.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crc::translate {

// Insertion-ordered (JP, EN) pairs.  The order is load-bearing: the glossary
// seeds the cache first, so it decides the head of the cache file's key order.
const std::vector<std::pair<std::string, std::string>>& name_translations_ordered();

// JP -> EN lookup map over the same data.
const std::unordered_map<std::string, std::string>& name_translations();

extern const char* const SYSTEM_PROMPT;

// Files are translated in this order; anything not listed follows in the
// order it appears in extracted_text.json.  sn.bin is the only archive.
const std::vector<std::string>& story_order();

// ── Available models (edit MODEL below to switch) ──
// Same rate card for Opus 4.6 and 4.7 ($5/$25 per Mtok), but 4.7's new
// tokenizer can emit up to 35% more tokens for the same text — 4.6 is
// effectively cheaper per request. Sonnet 4.6 stays the cheap baseline.
inline const std::string MODEL_SONNET = "claude-sonnet-4-6";
inline const std::string MODEL_OPUS_46 = "claude-opus-4-6";
inline const std::string MODEL_OPUS_47 = "claude-opus-4-7";
inline const std::string MODEL = MODEL_OPUS_47;  // ← swap as needed
inline const std::string EFFORT = "high";

inline bool is_effort_model(const std::string& m) {
    return m == MODEL_SONNET || m == MODEL_OPUS_46 || m == MODEL_OPUS_47;
}

// Rolling context sent with every batch.  CROSS_CHANNEL uses 50, not the 10
// the BLACKCyc ports use -- the script leans hard on scene continuity.
constexpr int CONTEXT_WINDOW = 50;
constexpr int BATCH_SIZE = 150;

}  // namespace crc::translate
