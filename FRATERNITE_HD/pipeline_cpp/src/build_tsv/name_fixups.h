// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The SECOND, smaller name table -- 26 entries, used only by step 3.  It is
// NOT the 78-entry glossary in translate/glossary.h, despite the overlap:
// this one exists only for clean_en(), to romanise raw kanji the LLM left
// inside the English.  Collapsing the two would rewrite far more of the EN
// side and change the TSV.
//
// name_fixups.cpp is GENERATED; regenerate rather than hand-edit:
//          frat::build_tsv build_tsv/name_fixups.h NameFixups=NAME_TRANSLATIONS
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace frat::build_tsv {

// Declaration order of the table, preserved: NameFixupsByLenDesc() sorts it
// stably, so this order breaks ties between equal-length JP keys.
const std::vector<std::pair<std::string, std::string>>& NameFixups();

// Stable sort of NameFixups() by DESCENDING codepoint length of the JP key,
// so 園田 cannot eat into 園田Ｈ.
const std::vector<std::pair<std::string, std::string>>& NameFixupsByLenDesc();

}  // namespace frat::build_tsv
