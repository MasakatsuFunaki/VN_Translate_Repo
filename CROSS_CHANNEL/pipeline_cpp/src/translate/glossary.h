// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CROSS†CHANNEL -FINAL COMPLETE- character glossary, story order, and Gemini
// system prompts.
//
// The tables in glossary.cpp are pinned data: unit tests and the speaker gate
// assert their size, order, and exact pairs because editing an entry changes
// every name plate in the game and re-keys the cache.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crc::translate {

inline constexpr std::string_view kDefaultModel = "gemini-4-argon";
inline constexpr int kDefaultBatchSize = 120;
inline constexpr int kDefaultContextWindow = 60;
inline constexpr int kDefaultContextAfterWindow = 20;

// Insertion-ordered (JP, EN) pairs.  The order is load-bearing: the glossary
// seeds the cache first, so it decides the head of the cache file's key order.
const std::vector<std::pair<std::string, std::string>>& name_translations_ordered();

// JP -> EN lookup map over the same data.
const std::unordered_map<std::string, std::string>& name_translations();

// Files are translated in this order; anything not listed follows in the
// order it appears in extracted_text.json.  sn.bin is the only archive.
const std::vector<std::string>& story_order();

// Full literary translator system prompt (derived from TONE_OF_NOVEL.md and
// name_translations_ordered()).
const std::string& system_prompt();

// Neutral literary fallback prompt used when a batch triggers a safety block
// on the primary prompt (frames the task cleanly without explicit mature-theme
// trigger words while keeping the exact character glossary and formatting rules).
const std::string& neutral_system_prompt();

}  // namespace crc::translate
