// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The .xtx config-file translation behind 03_translate_xtx.
//
// XTX files are comma-separated CP932 text.  Only the character-name table is
// applied: it drives the name plate above dialogue and uses FULL-WIDTH romaji,
// because the BLACKCyc engine crashes on single-byte ASCII in XTX display
// fields.  The BGM/SE lists are cosmetic (gallery/settings only) and stay
// disabled for the same reason -- to enable them, convert their tables to
// full-width first.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace exm::xtx {

// Insertion-ordered (JP, EN) pairs; substitutions are applied in this order.
const std::vector<std::pair<std::string, std::string>>& charaname_translations();

// Rewrite one XTX file in place, backing the original up first.
// `name_field` is the 0-indexed comma field holding the display name:
//   spt/*.xtx             -> 'key,name'          -> 1
//   nya/listenbgmlist.xtx -> 'name,key,f3,...'   -> 0
// Returns the number of fields changed (0 when the file is missing).
int translate_xtx_file(const std::string& filepath,
                       const std::vector<std::pair<std::string, std::string>>& translations,
                       const std::string& backup_dir, int name_field = 1);

int run_translate_xtx(const std::string& game_dir);

}  // namespace exm::xtx
