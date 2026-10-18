// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Runtime translation-table builder: reads translated_text.json and emits a
// deduplicated UTF-8 TSV the proxy DLL loads for JP -> EN swap at draw time.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace snu::build_tsv {

// Name rows that share the same English -- always a defect (two speakers the
// player cannot tell apart).
std::vector<std::string> name_plates_sharing_english(
    const std::vector<std::pair<std::string, std::string>>& name_rows);

// Escape for the TSV: backslash, tab, CR, LF.
std::string esc(const std::string& s);

// Give the English the same leading U+3000 indent as its Japanese key.
// Done here (not in the cache) so rebuilds reproduce the table from inputs.
std::string normalize_leading_space(const std::string& jp, const std::string& en);

// Rewrite superseded terminology and name spellings in one row's English.
// Only applied where the JP carries the term, and only at word boundaries.
std::string apply_pinned_terms(const std::string& jp, const std::string& en);

int run_build(const std::string& translated_file, const std::string& out_tsv,
              bool include_untranslated);

}  // namespace snu::build_tsv
