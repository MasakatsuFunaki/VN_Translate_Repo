// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Runtime translation-table builder.
//
// Reads translated_text.json and writes translation_table.tsv in CP932:
//     JP_CP932<TAB>EN_CP932\n     (\\ \r \n \t escaped)
#pragma once

#include <string>

namespace exm::build_tsv {

constexpr int MAX_LINE_CHARS = 60;

std::string merge_speaker_line(const std::string& text);
std::string sanitize_newlines(const std::string& text);
std::string reflow_lines(const std::string& text);
std::string word_wrap(const std::string& text, int max_chars = MAX_LINE_CHARS);
std::string escape_for_tsv(const std::string& text);
int run_build(const std::string& translated_file, const std::string& out_tsv);

}  // namespace exm::build_tsv
