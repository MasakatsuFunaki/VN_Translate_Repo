// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Sakura no Uta character / location glossary.
// Names are confirmed against the official SCA-DI cast list.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace snu::translate {

// Insertion-ordered (JP, EN) pairs; seeds the cache, so order fixes JSON key order.
// Every entry is a speaker/name in the scripts and vice versa; extras go in extra_name_translations().
const std::vector<std::pair<std::string, std::string>>& name_translations_ordered();

// JP -> EN lookup map over the same data.
const std::unordered_map<std::string, std::string>& name_translations();

// Person-name rows from name_translations_ordered().
// Role/mob labels (少女, 客Ａ, …) are excluded so they stay lowercase in prose.
const std::vector<std::pair<std::string, std::string>>& prompt_name_translations();

// Cast names absent from the scripts; can't be speakers, but the name box still needs a row.
const std::vector<std::pair<std::string, std::string>>& extra_name_translations();

// Recurring non-name vocabulary (titles, surnames, places, motifs).
// Keys are disjoint from both name tables (enforced by test).
const std::vector<std::pair<std::string, std::string>>& terminology_translations();

// (JP term, old English rendering) -- rewritten in the derived table at build time.
// Replacement is the term's current entry in terminology_translations(); every key must exist there (test-enforced).
const std::vector<std::pair<std::string, std::string>>& superseded_renderings();

// (JP name, old English rendering) -- same repair as superseded_renderings(), for character names.
// Replacement is the name's current entry in the name tables; every key must exist there (test-enforced).
const std::vector<std::pair<std::string, std::string>>& superseded_name_renderings();

constexpr int CONTEXT_WINDOW = 50;  // previous EN lines for continuity

}  // namespace snu::translate
