// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step 1: extract Japanese strings from Sakura.hcb.
//
// Every string-push opcode (0x0E) in the bytecode becomes one entry, in
// stream order.  Every string is kept (even non-JP ones) so the runtime DLL
// hook can match strings exactly as they appear in the script -- the
// translator step is the one that decides what's worth sending to the API.
#pragma once

#include <string>

namespace fav::extract {

// Walks hcb_path and writes output_file (extracted_text.json).
// Returns the process exit code.
int run_extract(const std::string& hcb_path, const std::string& output_file);

}  // namespace fav::extract
