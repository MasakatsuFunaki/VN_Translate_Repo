// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Runtime translation-table builder.
//
// Reads translated_text.json and writes a UTF-8 TSV next to cs2.exe:
//     <jp>\t<en>\n            (escapes: \\ and \t)
//
// The proxy DLL (amakano2pe/proxy_dll/) loads this at game startup and
// replaces Japanese strings with English on the fly via a hook on
// FUN_0063a9b0.
//
// The engine renders one char at a time and crashes on any embedded newline
// (both "\r\n" and bare "\n" are fatal): it has NO concept of "line break
// inside a string" -- multi-line dialogue comes from consecutive MESSAGE
// invocations.  So every value is flattened to a single line.
#pragma once

#include <string>

namespace ama::build_tsv {

// Strip every form of newline -- embedding any will crash the renderer.
std::string flatten(const std::string& text);

// Escape for the TSV: backslash and tab only.
std::string escape_for_tsv(const std::string& text);

int run_build(const std::string& translated_file, const std::string& out_tsv);

}  // namespace ama::build_tsv
