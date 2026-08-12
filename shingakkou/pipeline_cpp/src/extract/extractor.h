// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step 1: walk the decrypted script bytecode for text records, classify them,
// and emit extracted_text.json.
#pragma once

#include <string>
#include <vector>

#include "common/util.h"

namespace shin::extract {

// Hiragana | Katakana | CJK unified | FULLWIDTH forms.
//
// Deliberately WIDER than translate::has_real_japanese: it accepts fullwidth
// (so the speaker '？？？' survives extraction) and rejects CJK Ext-A (which
// the translate-side predicate accepts).  The two are not interchangeable --
// swapping one for the other changes both the extracted set and the
// translatable set.
bool has_japanese(const std::string& utf8);

struct Str {
    std::size_t offset;   // byte position of the FF 01 80 marker
    std::string type;     // "dialogue" | "narration" | "choice"
    std::string speaker;  // raw JP name, "" for narration/choice
    std::string text;
};

// Every record is  <pre_byte> FF 01 80 <UTF-16LE units> 00 00.  The byte
// BEFORE the marker classifies it: 0x01 choice, 0x36/0x07 dialogue-or-
// narration, anything else ignored.  Dialogue is "Speaker\n Text" where the
// \n is a LITERAL backslash + n, not a newline.
std::vector<Str> extract_strings_from_script(const Bytes& dec);

// <game>\Data\sin_text.dat.bak when it exists (the unpatched original), else
// <game>\Data\sin_text.dat.
std::string resolve_source_file(const std::string& game_dir);

// Read the archive, decompress + decrypt + walk every script, write
// extracted_text.json.  Returns the process exit code.
int run_extract(const std::string& game_dir, const std::string& output_file);

}  // namespace shin::extract
