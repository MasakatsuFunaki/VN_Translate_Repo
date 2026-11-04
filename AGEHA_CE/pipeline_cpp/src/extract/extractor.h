// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Extract Japanese text from .spt script files (XOR-0xFF encrypted CP932).
#pragma once

#include <string>

namespace exm::extract {

int run_extract(const std::string& spt_dir, const std::string& output_file);
std::string resolve_spt_dir(const std::string& game_dir);

}  // namespace exm::extract
