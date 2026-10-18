// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Sakura no Uta character / location glossary + Anthropic prompts.
// Names are confirmed against the official SCA-DI cast list, so they are
// pinned here rather than left to the model.
#pragma once

#include <cstdint>
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

// Person-name rows from name_translations_ordered(), for the prompt.
// Role/mob labels (少女, 客Ａ, …) are excluded so they stay lowercase in prose.
// Derived from the table, so prompt and runtime cannot drift.
const std::vector<std::pair<std::string, std::string>>& prompt_name_translations();

// Cast names absent from the scripts; can't be speakers, but the name box still needs a row.
const std::vector<std::pair<std::string, std::string>>& extra_name_translations();

// Recurring non-name vocabulary (titles, surnames, places, motifs).
// Prompt-side only -- NOT seeded into cache or runtime table.
// Keys are disjoint from both name tables (enforced by test).
const std::vector<std::pair<std::string, std::string>>& terminology_translations();

// (JP term, old English rendering) -- rewritten in the derived table at build time.
// Replacement is the term's current entry in terminology_translations(); every key must exist there (test-enforced).
const std::vector<std::pair<std::string, std::string>>& superseded_renderings();

// (JP name, old English rendering) -- same repair as superseded_renderings(), for character names.
// Replacement is the name's current entry in the name tables; every key must exist there (test-enforced).
const std::vector<std::pair<std::string, std::string>>& superseded_name_renderings();

// The system prompt, with the terminology table rendered into it.
const std::string& system_prompt();

// ── Available models (edit MODEL below to switch) ──
// Opus 4.6/4.7 share the rate card, but 4.7's tokenizer emits ~35% more tokens.
inline const std::string MODEL_HAIKU = "claude-haiku-4-5-20251001";
inline const std::string MODEL_SONNET = "claude-sonnet-4-6";
inline const std::string MODEL_OPUS_46 = "claude-opus-4-6";
inline const std::string MODEL_OPUS_47 = "claude-opus-4-7";
inline const std::string MODEL = MODEL_OPUS_47;  // ← swap as needed
inline const std::string EFFORT = "high";

// Per-model reasoning knob: Opus 4.7 = adaptive thinking, others = output_config.effort.
enum class ReasoningControl { None, AdaptiveThinking, Effort };

inline ReasoningControl reasoning_control(const std::string& m) {
    if (m == MODEL_OPUS_47) return ReasoningControl::AdaptiveThinking;
    if (m == MODEL_SONNET || m == MODEL_OPUS_46) return ReasoningControl::Effort;
    return ReasoningControl::None;
}

// What the banner prints -- the wording names the field that goes on the wire.
inline std::string reasoning_control_label(const std::string& m) {
    switch (reasoning_control(m)) {
    case ReasoningControl::AdaptiveThinking: return "thinking: adaptive";
    case ReasoningControl::Effort:           return "output_config.effort: " + EFFORT;
    case ReasoningControl::None:             break;
    }
    return "none";
}

constexpr int CONTEXT_WINDOW = 50;  // previous EN lines for continuity

// Per-request output ceiling accepted by the models above.  max_tokens is a
// cap, not an allocation, but a request asking for more than the model can
// emit is rejected outright, so the computed value is clamped to it.
constexpr std::int64_t MAX_OUTPUT_TOKENS = 128000;

// Floor for the same computation: a small batch still needs room for a whole
// answer plus the thinking that precedes it.
constexpr std::int64_t MIN_OUTPUT_TOKENS = 16384;

// Output tokens to allow per line of a batch.  Measured output is ~26 tokens
// per line including thinking; the margin absorbs long lines and retries.
constexpr std::int64_t OUTPUT_TOKENS_PER_LINE = 300;

}  // namespace snu::translate
