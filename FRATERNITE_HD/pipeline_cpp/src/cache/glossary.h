// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// GENERATED character glossary. Do not hand-edit glossary.cpp.
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

const std::vector<std::string>& story_order();

}  // namespace frat::translate
