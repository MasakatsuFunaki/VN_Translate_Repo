// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// translator_logic.h  (amakano2pe / CatSystem2)
//
// Pure (OS-independent) logic that powers the amakano2pe runtime
// translator. Everything here is free of Windows / hook /
// trampoline dependencies so the same object file can be linked
// into both the proxy DLL and the gtest binary.
//
// Unlike the FRATERNITE_HD sibling, amakano2pe's engine (CatSystem2)
// hands us a complete UTF-8 NUL-terminated string per call, so the
// logic here is deliberately small: parse the TSV, do a direct
// lookup, UTF-8-safely truncate the English replacement if it's
// about to overflow the engine's downstream buffer.

#include <cstddef>
#include <string>
#include <unordered_map>

namespace translator_logic {

using TranslationMap = std::unordered_map<std::string, std::string>;

// --- String helpers ------------------------------------------------------

// Undo the TSV escape sequences: \r \n \t \\. In-place.
void UnescapeInPlace(std::string& s);

// Truncate the NUL-terminated UTF-8 string `s` to at most `max_bytes`
// bytes, snapping to a UTF-8 character boundary. Returns the safe
// byte length (always <= max_bytes, and never splits a multi-byte
// UTF-8 codepoint). For ASCII-only input this is a plain min().
size_t Utf8SafeTruncate(const char* s, size_t max_bytes);

// If `s` begins with the 2-byte sequence `\n` (backslash + 'n', the
// CatSystem2 textbox line-break control code -- NOT an ASCII 0x0A),
// replace it with a single space and return true. Returning true
// signals to the runtime that this entry is a CONTINUATION of the
// previous MESSAGE in the same textbox -- the JP author split one
// sentence across consecutive MESSAGEs. The runtime uses that signal
// to keep its running column counter (instead of resetting) so the
// wrap accounts for the text already drawn on the textbox line.
//
// Called at *runtime*, NOT at TSV parse time, because the parsed
// EN value must retain the leading `\n` marker so the runtime can
// distinguish continuations from fresh utterances.
bool StripLeadingControlNewline(std::string& s);

// Pre-wrap English text at word boundaries by inserting 2-byte `\n`
// (backslash + 'n') line-break control codes before any word that
// would push the current line past `line_chars` characters.
//
// Why: CatSystem2 concatenates consecutive MESSAGEs into a single
// textbox line and then wraps that line by pixel count without
// English word-boundary awareness, producing mid-word breaks
// ("ro\nugh."). By inserting explicit breaks at word boundaries
// slightly inside the engine's pixel limit, we guarantee every
// engine-rendered line starts and ends on a complete word.
//
// `start_col` is the column position at which `s` will be APPENDED
// to the engine's current textbox line. When the JP script splits a
// sentence across MESSAGEs, the engine concatenates them into one
// rendered line; per-MESSAGE wrap that always assumes start_col=0
// fails to wrap a short MESSAGE that overflows once concatenated to
// a longer prior MESSAGE. Pass the previous call's return value as
// `start_col` to wrap correctly across MESSAGE boundaries.
//
// Returns the final column at the end of `s` (i.e., the col where
// the NEXT MESSAGE would start being appended). A `\n` control code
// emitted by the wrap (or already present in `s`) resets the
// running column to 0.
//
// Words longer than `line_chars` are left intact (can't split
// mid-word). When `start_col >= line_chars`, the first inter-word
// break is taken immediately so the next MESSAGE doesn't pile onto
// an already-overflowing line.
size_t WordWrapForEngine(std::string& s, size_t line_chars,
                         size_t start_col = 0);

// --- TSV loading ---------------------------------------------------------

// Parse a TSV buffer in memory into a translation map.
//
// Format per line: "<jp>\t<en>\n" (or \r\n). Skips a leading UTF-8
// BOM (EF BB BF) if present. Unescapes both sides. Drops empty
// entries. Returns the number of entries inserted; `out` is
// appended to, not cleared.
int ParseTsvBuffer(const char* data, size_t len, TranslationMap& out);

// Convenience: open a file by path, read the entire contents, and
// call ParseTsvBuffer. Returns true iff the file was opened and at
// least one entry was parsed.
bool LoadTsvFile(const std::string& path, TranslationMap& out);

// --- Lookup --------------------------------------------------------------

// Find the translation for `src` in `map`. Returns nullptr if no
// hit. Thin wrapper so tests assert lookup semantics without
// exposing the map's unordered_map type to the hook path.
const std::string* FindTranslation(const TranslationMap& map,
                                   const std::string& src);

}  // namespace translator_logic
