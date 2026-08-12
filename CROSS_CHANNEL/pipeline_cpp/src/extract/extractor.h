// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step 1: pull every translatable Japanese string out of the decompressed
// sn.bin bytecode.
//
// Three engine-specific traps are baked into this file; all three were real
// in-game bugs and are documented at length in the repo CLAUDE.md:
//   §6  strip_control_prefix must KEEP a pure-printable prefix.
//   §7  the JP-count threshold relaxes to 2 behind the choice-option preamble.
//   §9  a CP932 decode failure slides ONE byte, it does not skip to the next NUL.
#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/util.h"

namespace crc::extract {

// sn.bin stores scene blocks in authoring order, not player-visible order.
// The game opening (「今、何時だ？」) lives at this offset in the decompressed
// data; the entry list is rotated so it becomes index 0.
constexpr std::size_t GAME_START_OFFSET = 0x1CDBD3;  // 1,891,283

// Hiragana / katakana / CJK unified + ext-A / CJK symbols.  Half-width katakana
// (FF66-FF9D) are deliberately EXCLUDED -- they are almost always bytecode
// artefacts.  NOTE this is NOT the same predicate as
// translate::has_real_japanese, which omits the 3000-303F block; the two gates
// select different strings and must stay separate.
bool is_real_japanese(char32_t cp);

bool has_japanese(const std::string& utf8);
int count_japanese(const std::string& utf8);

// Fraction of CODEPOINTS that are Japanese, counting a small set of
// decorative characters as Japanese too.  U+3000 is in both the
// is_real_japanese range and the bonus set and is therefore counted TWICE;
// the 0.3 gate below is calibrated against that double count, so removing it
// would silently retune the filter.
double japanese_density(const std::string& utf8);

// Offsets of the name bytes following each `47 0D 00` speaker opcode.
std::unordered_set<std::size_t> find_speaker_offsets(const Bytes& data);

// CLAUDE.md §6.  Strip a leading run only when it actually contains a control
// byte; a pure-printable prefix (`※`, `Ａ`, `（`, `FLOWER'S`) is legitimate
// content and dropping it produced TSV keys the engine never emits.
std::string strip_control_prefix(const std::string& utf8);

// CLAUDE.md §7.  Choice-menu option text is always preceded by
// `FF FF <jump label LE16> 00 00`.
bool is_choice_option_at(std::size_t offset, const Bytes& data);

struct RawString {
    std::size_t offset;
    std::string text;
    bool is_speaker;
};

std::vector<RawString> extract_strings(const Bytes& data,
                                       const std::unordered_set<std::size_t>& speaker_offsets);

struct Entry {
    std::size_t offset;
    std::string type;  // "dialogue" | "narration"
    std::string speaker;
    std::string text;
};

std::vector<Entry> classify_and_pair(const std::vector<RawString>& strings, const Bytes& data);

// Rotate so the first entry at or after GAME_START_OFFSET leads.  Returns the
// index the list was rotated by (0 = untouched).
std::size_t rotate_to_game_start(std::vector<Entry>& entries);

int run_extract(const std::string& sn_bin_path, const std::string& output_file);

}  // namespace crc::extract
