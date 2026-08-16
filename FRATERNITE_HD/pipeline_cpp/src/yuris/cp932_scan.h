// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CP932 Japanese-run scanner over decrypted YBN bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common/util.h"

namespace frat::yuris {

inline bool is_cp932_lead(std::uint8_t b) {
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}
inline bool is_cp932_trail(std::uint8_t b) {
    return (b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFC);
}
inline bool is_ascii_printable(std::uint8_t b) {
    return (b >= 0x20 && b < 0x7F) || b == 0x09 || b == 0x0A || b == 0x0D;
}

// Any kana / CJK / fullwidth / CJK-symbol codepoint.  Wider than
// extract::has_japanese: this one also accepts U+3000-303F and U+FF00-FFEF.
bool has_japanese_run(const std::string& utf8);

// Drop the ASCII-only prefix/suffix around the Japanese core so opcode bytes
// scanned out of the bytecode do not become part of the key.
//
// The "Japanese" test here is the single interval 0x3040-0x9FFF plus
// 0xFF00-0xFFEF, which deliberately EXCLUDES U+3000-303F: a leading 「 or a
// trailing 」/。 on a run is trimmed away.  Only pieces produced by
// split_into_messages can therefore start with 「 and classify as "dialogue".
std::string trim_ascii_edges(const std::string& utf8);

// Removes ONLY the listed characters, never the wider Unicode whitespace set
// trim() uses.  Called with " \t\r\n\0", so U+3000 survives -- it is part
// of the text the engine renders, not padding.
std::string strip_chars(const std::string& s, const std::string& chars);

// Maximal CP932 runs containing at least one double-byte character.
//
// On a strict-decode failure the WHOLE run is skipped and scanning resumes at
// the already-advanced cursor -- it does NOT slide one byte and retry.  That
// is the deliberate inverse of CLAUDE.md section 9: here the run boundaries
// are what define a string, so sliding would admit opcode bytes as text.
// Changing it changes the extracted-string set and every downstream key.
std::vector<std::pair<std::size_t, std::string>> scan_cp932_jp(
    const Bytes& data, std::size_t min_run = 4, std::size_t max_run = 16384);

}  // namespace frat::yuris
