// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// EXTRAVAGANZA ~Mushi Mederu Shoujo~ Complete Edition character glossary and
// story order.
//
// The name table is the pipeline's single source of truth for character
// names: it seeds the translation cache, resolves the speaker plate, and is
// what makes the same character read the same way across all 16 scripts.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exc::translate {

// Insertion-ordered (JP, EN) pairs.  The order is load-bearing: it is the
// cache seeding order and therefore the key order of the cache file, and it
// is the scan order of the longest-suffix speaker match.
const std::vector<std::pair<std::string, std::string>>& name_translations_ordered();

// JP -> EN lookup map over the same data.
const std::unordered_map<std::string, std::string>& name_translations();

// Files are translated in this order; anything not listed follows in the
// order it appears in extracted_text.json.
const std::vector<std::string>& story_order();

}  // namespace exc::translate
