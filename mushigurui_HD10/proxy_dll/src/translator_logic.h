// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// translator_logic.h
//
// Pure (OS-independent) logic that powers the mushigurui_HD10 runtime
// translator. Everything here is free of Windows / file-IO / thread
// dependencies so the same object file can be linked into both the
// proxy DLL and the gtest binary.
//
// The counterpart translator.cpp wires these primitives up to the
// real engine: it opens translation_table.tsv with _wfopen, installs
// the thiscall trampoline around FUN_00523850, and keeps the
// TranslateText hook memory pool. The parts that can run without the
// game (BOM stripping, TSV parsing, backslash-escape expansion) live
// here where they can be exercised without a running executable.
//
// Unlike the FRATERNITE_HD translator, this one does an exact-string
// lookup on the whole buf passed to the splitter -- there is no
// boundary-gated segmenter because mushigurui's text arrives already
// pre-split per SPT entry. That means the logic here is noticeably
// smaller; all the "greedy match" subtlety lives in the FRATERNITE
// project.

#include <cstddef>
#include <string>
#include <unordered_map>

namespace translator_logic {

using TranslationMap = std::unordered_map<std::string, std::string>;

// --- String helpers ------------------------------------------------------

// Undo the TSV escape sequences: \r \n \t \\. In-place. Unknown
// escapes (e.g. \q) are preserved verbatim so the backslash survives.
void UnescapeInPlace(std::string& s);

// --- BOM / TSV parsing ---------------------------------------------------

// If the buffer at `data` starts with the UTF-8 BOM (EF BB BF) and is
// at least 3 bytes long, returns 3; otherwise returns 0. Caller adds
// this to their read cursor to skip the BOM.
//
// We keep this pure (instead of baking it into ParseTsvBuffer) so the
// tests can pin it down in isolation -- getting BOM handling wrong
// causes subtle bugs where the FIRST TSV entry silently has three
// junk bytes prepended to its key.
std::size_t Utf8BomLen(const char* data, std::size_t len);

// Parse a TSV buffer in memory into a translation map. Each line is
// "jp\ten\n" (or \r\n). Unescapes both sides, drops empty entries.
// Returns the number of entries inserted; `out` is appended to, not
// cleared. Duplicate keys: last one wins (matches the existing
// LoadTranslations semantics so behavior is unchanged).
//
// Takes a view as (ptr, len). Accepts both \n and \r\n line endings.
// Does NOT strip the UTF-8 BOM -- call Utf8BomLen() first and advance
// the pointer/length to skip it.
int ParseTsvBuffer(const char* data, std::size_t len, TranslationMap& out);

}  // namespace translator_logic
