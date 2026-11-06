// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// The .xtx config-file translation behind 03_build_xtx.
//
// XTX files are comma-separated CP932 text. Only the character-name table is
// applied: it drives the name plate above dialogue and uses full-width romaji,
// because the BLACKCyc engine crashes on single-byte ASCII in XTX display
// fields.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace exm::xtx {

constexpr int kDefaultNameFieldIndex = 1;

const std::vector<std::pair<std::string, std::string>>& charaname_translations();

int translate_xtx_file(const std::string& filepath,
                       const std::vector<std::pair<std::string, std::string>>& translations,
                       const std::string& backup_dir,
                       int name_field = kDefaultNameFieldIndex);

int run_translate_xtx(const std::string& game_dir);

}  // namespace exm::xtx
