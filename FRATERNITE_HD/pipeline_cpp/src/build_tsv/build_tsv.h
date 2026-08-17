// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Runtime translation-table builder (translated_text.json -> translation_table.tsv).
// Keys are CP932 buf38 byte strings; each line also emits variant forms.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/util.h"

namespace frat::build_tsv {

// Short quotes get no ？？？ variant — they are said by everyone.
constexpr std::size_t MIN_MYSTERY_QUOTE_BYTES = 14;

// Newlines -> ' ', then full-Unicode trim (U+3000 must go too).
std::string flatten(const std::string& text);

std::string escape_for_tsv(const std::string& text);

// Substitute glyphs CP932 lacks; '?' for anything unencodable.
std::string cp932_safe(const std::string& s);

std::optional<Bytes> to_cp932(const std::string& s);

// Romanise raw JP names the LLM left in the English, longest-first.
std::string clean_en(const std::string& en);

// ≪WORD／READING≫ -> WORD (what the engine draws).
std::string strip_ruby(const std::string& s);

std::string ensure_close_bracket(const std::string& s);

// Splits `Name「quote」`: "／「あ」" matches with name == "／".
bool speaker_match(const std::string& s, std::string& name, std::string& quote);

// All (jp_variant, en_variant) pairs for one line; insertion-ordered dedup.
std::vector<std::pair<std::string, std::string>> variants(const std::string& jp,
                                                          const std::string& en);

int run_build(const std::string& translated_file, const std::string& out_tsv);

}  // namespace frat::build_tsv
