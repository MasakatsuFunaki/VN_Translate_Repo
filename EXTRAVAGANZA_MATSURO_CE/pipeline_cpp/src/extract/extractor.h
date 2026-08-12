// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step 1: extract all Japanese text from .spt script files.
// SPT files are XOR-0xFF encrypted CP932.
#pragma once

#include <string>

namespace exm::extract {

// Walks every .spt in spt_dir and writes output_file (extracted_text.json).
// Returns the process exit code.
int run_extract(const std::string& spt_dir, const std::string& output_file);

// Picks <game>\spt_backup when present, else <game>\spt -- the backup holds
// the pristine scripts once the repack step has run.
std::string resolve_spt_dir(const std::string& game_dir);

}  // namespace exm::extract
