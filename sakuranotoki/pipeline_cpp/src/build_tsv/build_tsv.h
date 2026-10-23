// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Runtime translation-table builder.
//
// Reads translated_text.json and emits a deduplicated UTF-8 TSV:
//     <jp_utf8>\t<en_utf8>\n
// with \t \r \n \\ escaped in both fields.  The proxy DLL loads this at
// startup and looks each JP string up in O(1).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace snt::build_tsv {

// Escape for the TSV: backslash, tab, CR, LF.
std::string esc(const std::string& s);

// Rewrite a superseded English spelling, but only where the Japanese of the
// same row carries the term the pin belongs to.  Whole terms only, so a longer
// word that merely starts the same survives.  Running it twice gives what
// running it once gives.
std::string apply_pinned_terms(const std::string& jp, const std::string& en);

// Give the English the paragraph indent the Japanese key carries, and no other:
// leading U+3000 and any ASCII blank the answer put in front are replaced by
// exactly the key's indent.
std::string normalize_leading_space(const std::string& jp, const std::string& en);

// Turn the breaks inside a value back into spaces so the engine's own wrap
// decides where the lines end.  A break at the very end is left alone.
std::string run_together(const std::string& value);

// "<jp> / <jp> -> <en>" for every English that stands for more than one name
// plate.  Two plates the player cannot tell apart are one character to them.
std::vector<std::string> name_plates_sharing_english(
    const std::vector<std::pair<std::string, std::string>>& name_rows);

// `cache_file` is the resume cache: translated_text.json can be stale if step
// 2 ran again without rebuilding it, so any entry with no real translation
// falls back to the cache.  `flow_breaks` runs the breaks inside a value
// together; the default keeps them, because the table has to stay free of any
// assumption about how the engine lays a box out.
int run_build(const std::string& translated_file, const std::string& cache_file,
              const std::string& out_tsv, bool include_untranslated,
              bool flow_breaks = false);

}  // namespace snt::build_tsv
