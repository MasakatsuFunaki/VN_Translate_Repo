// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step 3: rebuild the RAW script string form for both key and value and write
// the TSV the proxy DLL loads.
//
// The proxy DLL hooks the decrypt function and patches script data in memory,
// so the keys must match the bytes the engine emits, byte for byte:
//   Dialogue:  SpeakerName\nDialogueText   (literal backslash-n)
//   Narration: \nText
//   Choice:    Text                        (no prefix)
#pragma once

#include <string>

namespace shin::build_tsv {

// Only a real tab / CR / LF is escaped, and only to an unambiguous token.  The
// literal backslash-n that separates speaker from text is two ORDINARY
// characters (0x5C 0x6E) and is deliberately left alone.
std::string escape_for_tsv(const std::string& text);

// Reads translated_text.json, writes <out_tsv> as UTF-8-**SIG** with CRLF row
// terminators.  Returns the process exit code.
int run_build(const std::string& translated_file, const std::string& out_tsv);

}  // namespace shin::build_tsv
