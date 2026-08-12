// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Runtime translation-table builder.
//
// Collapses translated_text.json into a deduplicated UTF-8 TSV
//     <jp>\t<en>\n        (\\ \t \r \n escaped on both sides)
// which the proxy DLL (xinput1_3.dll) loads at game launch and uses to
// substitute text on every GDI render call.  Runtime substitution means no
// archive rewriting and no byte-budget ceiling on the English.
#pragma once

#include <string>

namespace crc::build_tsv {

// ORDER IS LOAD-BEARING: backslash first, or every later escape doubles.
std::string esc(const std::string& s);

// out_tsv is written with bare LF row separators -- the one text artefact in
// the pipeline that is NOT CRLF, because a real CR inside a field is carried
// as the two-character escape "\r" instead.  It is then copied to game_tsv
// when that directory exists.
int run_build(const std::string& translated_file, const std::string& out_tsv,
              const std::string& game_tsv);

}  // namespace crc::build_tsv
