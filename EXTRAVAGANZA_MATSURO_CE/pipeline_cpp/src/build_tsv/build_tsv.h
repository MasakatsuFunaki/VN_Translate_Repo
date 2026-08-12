// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Runtime translation-table builder.
//
// Reads translated_text.json and writes translation_table.tsv in the game
// directory.  Unlike the other games' tables this one is **CP932-encoded**,
// because the BLACKCyc engine reads it directly:
//     JP_CP932<TAB>EN_CP932\n     (\\ \r \n \t escaped)
//
// The English side is speaker-merged and word-wrapped first so it fits the
// textbox at the engine's font size.
#pragma once

#include <string>

namespace exm::build_tsv {

// Chars per line at fontSize=14 (~44 fit in the textbox, 60 is the ceiling).
constexpr int MAX_LINE_CHARS = 60;

// Merge the speaker plate onto the dialogue line to save vertical space.
std::string merge_speaker_line(const std::string& text);

// Normalise bare \n to \r\n for BLACKCyc, dropping a leading newline.
std::string sanitize_newlines(const std::string& text);

// Greedy word wrap, preserving existing \r\n segment breaks.
std::string word_wrap(const std::string& text, int max_chars = MAX_LINE_CHARS);

std::string escape_for_tsv(const std::string& text);

int run_build(const std::string& translated_file, const std::string& out_tsv);

}  // namespace exm::build_tsv
