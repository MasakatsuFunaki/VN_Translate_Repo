// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Runtime translation-table builder.
//
// Reads translated_text.json and writes translation_table.tsv next to
// fraternite_hd.exe.  The proxy DLL keys on the engine's live buf38 contents
// (raw CP932 bytes), so keys must be CP932-encodable, single-line, and
// tab/backslash escaped:  <jp_cp932> TAB <en_cp932> LF
//
// Every JSON entry also produces the runtime-byte VARIANTS the engine may
// actually render: ruby markers stripped, a leading ／ directive dropped, a
// missing 」 closed, a missing 。 appended, and the ？？？ mystery-speaker form.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/util.h"

namespace frat::build_tsv {

// Only quotes at least this many CP932 bytes get a ？？？ variant -- short
// utterances are said by everyone, so a mystery form would mislabel them.
constexpr std::size_t MIN_MYSTERY_QUOTE_BYTES = 14;

// Every newline form -> ' ', then a BARE strip -- U+3000 must go too, or the
// key carries an ideographic space the engine's buffer does not.
std::string flatten(const std::string& text);

// Backslash doubled FIRST, then TAB.  No newline escape: flatten already
// removed them.
std::string escape_for_tsv(const std::string& text);

// Substitute the handful of glyphs CP932 lacks, '?' for anything else that
// will not encode.
std::string cp932_safe(const std::string& s);

std::optional<Bytes> to_cp932(const std::string& s);

// Romanise raw JP names the LLM left inside the English.  Every table entry is
// applied (no break), longest-first.
std::string clean_en(const std::string& en);

// ≪WORD／READING≫ -> WORD, which is what the engine draws.
std::string strip_ruby(const std::string& s);

// Append 」 when the string has more 「 than 」.
std::string ensure_close_bracket(const std::string& s);

// Splits `Name「quote」` on an already-stripped string: an optional leading ／,
// then a non-greedy run with no 「 and no whitespace, then the quote (which may
// span newlines).  Consequence worth knowing: "／「あ」" matches with
// name == "／", because the optional ／ backtracks to empty.
bool speaker_match(const std::string& s, std::string& name, std::string& quote);

// All (jp_variant, en_variant) pairs for one translated line, deduped on the
// jp side by an INSERTION-ORDERED set so the TSV's line order is stable.
std::vector<std::pair<std::string, std::string>> variants(const std::string& jp,
                                                          const std::string& en);

int run_build(const std::string& translated_file, const std::string& out_tsv);

}  // namespace frat::build_tsv
