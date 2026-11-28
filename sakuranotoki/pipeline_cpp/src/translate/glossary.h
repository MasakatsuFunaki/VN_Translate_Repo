// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Sakura no Toki character glossary + Gemini system prompt.
//
// The tables in glossary.cpp are pinned: they seed the translation cache, so
// editing an entry changes the English every downstream artifact already
// holds.  Change a name only deliberately, and re-run step 2 for the affected
// lines afterwards.
#pragma once

#include "vn/llm/llm_types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace snt::translate {

// Insertion-ordered (JP, EN) pairs.  The order is load-bearing: it is the
// cache seeding order, and therefore the key order of
// translation_cache_gemini.json.
const std::vector<std::pair<std::string, std::string>>& name_translations_ordered();

// JP -> EN lookup map over the same data.
const std::unordered_map<std::string, std::string>& name_translations();

// The person rows of name_translations_ordered(), in that order, for the
// prompt.  Role and walk-on labels (少年, 女子生徒A, 猫, ...) are left out so
// that they stay ordinary words in prose.  The English is read from the same
// table the runtime rows come from, so the prompt cannot drift away from the
// table.  Throws std::logic_error when a selected key is not in the table.
const std::vector<std::pair<std::string, std::string>>& prompt_name_translations();

// Recurring vocabulary that is not a speaker label: work titles, family names,
// places, prizes and motifs.  Prompt-side only -- never seeded into the cache
// and never a runtime table row, because these are substrings of a line rather
// than a line the engine draws.  Keys are disjoint from the name table.
const std::vector<std::pair<std::string, std::string>>& terminology_translations();

// (JP term, the English it used to be given).  The table builder rewrites the
// old spelling wherever the Japanese of that row carries the term.  The
// replacement is the term's current entry in terminology_translations(), so
// every key here must exist there.
const std::vector<std::pair<std::string, std::string>>& superseded_renderings();

// The same repair for a character name; the replacement comes from
// name_translations_ordered(), so every key here must exist there.
const std::vector<std::pair<std::string, std::string>>& superseded_name_renderings();

// The system prompt, with the person names and the terminology table rendered
// into it from the data above, so the prompt and the pins cannot drift apart.
const std::string& system_prompt();

// ── Model ──
inline const std::string MODEL{vn::llm::kDefaultModel};

// Thinking level sent in generationConfig.thinkingConfig.thinkingLevel.
inline const std::string THINKING_LEVEL{vn::llm::kDefaultThinkingLevel};

// The reasoning knob a model accepts.
enum class ReasoningControl { None, ThinkingLevel };

inline ReasoningControl reasoning_control(const std::string& m) {
    if (m.rfind("gemini-3", 0) == 0 || m.rfind("gemini-4", 0) == 0) {
        return ReasoningControl::ThinkingLevel;
    }
    return ReasoningControl::None;
}

// The banner wording names the field that goes on the wire.
inline std::string reasoning_control_label(const std::string& m) {
    switch (reasoning_control(m)) {
    case ReasoningControl::ThinkingLevel:
        return "thinkingConfig.thinkingLevel: " + THINKING_LEVEL;
    case ReasoningControl::None:
        break;
    }
    return "none";
}

// Sliding context window: how many previous translations to include.
constexpr int CONTEXT_WINDOW = 50;

// Lookahead context window: how many upcoming script lines to include.
constexpr int CONTEXT_AFTER = 20;

// Per-request output ceiling Gemini accepts (65,536 tokens).
constexpr std::int64_t MAX_OUTPUT_TOKENS = vn::llm::kMaxOutputTokensCeiling;

// Floor for the same computation: a small batch still needs room for a whole
// answer and the thinking that precedes it.
constexpr std::int64_t MIN_OUTPUT_TOKENS = vn::llm::kMinOutputTokensFloor;

// Output tokens allowed per line of a batch.  The margin absorbs long lines
// and the retry after a rate limit.
constexpr std::int64_t OUTPUT_TOKENS_PER_LINE = vn::llm::kDefaultTokensPerBatchLine;

}  // namespace snt::translate
