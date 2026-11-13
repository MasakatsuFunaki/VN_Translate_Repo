// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// @PURPOSE: Provide OS-independent TSV parsing, CP932 lookup key normalization,
//           and 58-column word-wrap utilities for the FAVORITE proxy DLL.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace translator_logic {

using TranslationMap = std::unordered_map<std::string, std::string>;

inline constexpr std::size_t kDefaultFvpColumnGridWidth = 58;

// Undo the TSV escape sequences: \r \n \t \\. In-place.
void UnescapeInPlace(std::string& s);

// Truncate the NUL-terminated UTF-8 string `s` to at most `max_bytes`
// bytes, snapping to a UTF-8 character boundary. Returns the safe
// byte length (always <= max_bytes, and never splits a multi-byte
// UTF-8 codepoint).
std::size_t Utf8SafeTruncate(const char* s, std::size_t max_bytes);

// If `s` begins with the 2-byte sequence `\n` (backslash + 'n'), replace it
// with a single space and return true.
bool StripLeadingControlNewline(std::string& s);

// Pre-wrap English text at word boundaries by inserting 2-byte `\n`
// (backslash + 'n') line-break control codes before any word that
// would push the current line past `line_chars` characters.
std::size_t WordWrapForEngine(std::string& s, std::size_t line_chars,
                              std::size_t start_col = 0);

// Parse a TSV buffer in memory into a translation map.
// Format per line: "<jp>\t<en>\n" (or \r\n). Skips a leading UTF-8
// BOM (EF BB BF) if present. Unescapes both sides. Drops empty
// entries. Returns the number of entries inserted; `out` is
// appended to, not cleared.
int ParseTsvBuffer(const char* data, std::size_t len, TranslationMap& out);

// Open a file by path, read the entire contents, and call ParseTsvBuffer.
// Returns true iff the file was opened and at least one entry was parsed.
bool LoadTsvFile(const std::string& path, TranslationMap& out);

// Find the translation for `src` in `map`. Returns nullptr if no hit.
const std::string* FindTranslation(const TranslationMap& map,
                                   const std::string& src);

// Normalize a runtime CP932 lookup key by tokenizing forward along CP932
// character boundaries, stripping leading and trailing ASCII and fullwidth
// whitespace (0x81 0x40), unwrapping one outer pair of corner brackets
// (「...」 0x81 0x75..0x81 0x76 or 『...』 0x81 0x77..0x81 0x78), and
// re-stripping inner leading/trailing whitespace.
std::string NormalizeCp932LookupKey(const char* src, std::size_t len);

// Pad each fixed-column row with ASCII spaces before any word that would cross
// `max_cols` so FVP's fixed-column glyph layout wraps on word boundaries.
std::string WrapAsciiToColumnGrid(
    const std::string& text,
    std::size_t max_cols = kDefaultFvpColumnGridWidth);

}  // namespace translator_logic
