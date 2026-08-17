// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// GENERATED name table for clean_en() — NOT the glossary in translate/.
// Separate because collapsing the two would change the TSV output.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace frat::build_tsv {

const std::vector<std::pair<std::string, std::string>>& NameFixups();

// Descending codepoint length so 園田 cannot eat into 園田Ｈ.
const std::vector<std::pair<std::string, std::string>>& NameFixupsByLenDesc();

}  // namespace frat::build_tsv
