// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Runtime translation-table builder.
//
// Reads translated_text.json and writes a UTF-8 TSV next to Sakura.exe:
//     <jp>\t<en>\n            (escapes: \\ and \t)
//
// The proxy DLL (FAVORITE/proxy_dll/) loads this at game startup and replaces
// Japanese strings with English on the fly via a hook on Sakura.exe's text
// rendering path.
#pragma once

#include <string>

namespace fav::build_tsv {

// Strip newline forms -- the renderer can crash on embedded newlines.
std::string flatten(const std::string& text);

// Escape for the TSV: backslash and tab only.  Newlines are already gone via
// flatten(), and the prompt forbids them in the English output.
std::string escape_for_tsv(const std::string& text);

int run_build(const std::string& translated_file, const std::string& cache_file,
              const std::string& out_tsv);

}  // namespace fav::build_tsv
