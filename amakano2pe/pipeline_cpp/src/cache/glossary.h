// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Amakano 2 ~Perfect Edition~ character glossary.
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

// Sliding context window: how many previous translations to include.
constexpr int CONTEXT_WINDOW = 50;

}  // namespace ama::translate
