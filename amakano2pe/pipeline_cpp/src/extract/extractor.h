// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step 1: extract all Japanese text from the game's scene.int archive.
// CatSystem2 engine -- KIF archive format with CatScene scripts.
#pragma once

#include <string>

namespace ama::extract {

// Walks every CatScene script in scene_int and writes output_file
// (extracted_text.json).  Returns the process exit code.
int run_extract(const std::string& scene_int, const std::string& output_file);

}  // namespace ama::extract
