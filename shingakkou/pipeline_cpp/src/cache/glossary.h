// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shingakkou ~Noli me tangere~ character glossary and story
// order -- the pinned data behind step 2.
//
// The tables in glossary.cpp are data, not code: every entry is character-exact
// and any edit changes the cache keys for the whole game.
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

// Files are translated in this order; anything not listed follows in the
// order it appears in extracted_text.json.
const std::vector<std::string>& story_order();

}  // namespace shin::translate
