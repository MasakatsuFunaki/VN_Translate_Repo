// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CROSS†CHANNEL -FINAL COMPLETE- character glossary (NAME_TRANSLATIONS) and
// story order.
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

// Files are translated in this order; anything not listed follows in the
// order it appears in extracted_text.json.  sn.bin is the only archive.
const std::vector<std::string>& story_order();

}  // namespace crc::translate
