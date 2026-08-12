// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step 3: repack translated text back into the .spt script files.
//
// Unlike the other BLACKCyc games (which ship a runtime TSV the proxy DLL
// consults), EXTRAVAGANZA CE rewrites the SPT binaries in place.
//
// SPT section layout (each section):
//   [pre_value: u32][0x66660001][0x5555xxxx][0x4444xxxx][...data...]
//   pre_value * 4 = total section size in bytes (header + data).
//
// The text section (type 0x44440002) contains:
//   - An offset table of h_10 entries, 4 bytes each, where each entry is a
//     DWORD-indexed pointer: entry[i] = base + string_byte_offset[i] / 4,
//     with base = h_14 + h_10 (header fields at 0x10 and 0x14).
//   - Text data: sequential NUL-terminated, 4-byte-aligned CP932 strings.
//
// Rebuilding the block with different-length translations therefore requires:
//   1. rebuilding the offset table with the new string positions,
//   2. updating the section pre_value,
//   3. updating header field h_1C = h_14 + new_pre_value,
//   4. relocating the DWORD-aligned VAs that point past the text block.
//
// Reads are ALWAYS from the backup (pristine) copy, so repacking is idempotent.
#pragma once

#include <string>
#include <vector>

#include "common/util.h"

namespace exc::repack {

// Chars per line at fontSize=14, space=-4 -- fills the textbox width fully.
constexpr int MAX_LINE_CHARS = 60;
// Max lines per text block; the proxy DLL patches windowMessageNumber 4 -> 7.
constexpr int MAX_LINES = 7;

// Merge the speaker plate onto the dialogue line to save vertical space.
std::string merge_speaker_line(const std::string& text);
// Normalise bare \n to \r\n for BLACKCyc, dropping leading newlines (which are
// extraction artifacts from the control bytes between 4-aligned SPT strings).
std::string sanitize_newlines(const std::string& text);
// Greedy word wrap, preserving existing \r\n breaks, truncated to MAX_LINES.
std::string word_wrap(const std::string& text, int max_chars = MAX_LINE_CHARS);

// XOR 0xFF, same both ways.
Bytes encrypt_spt(const Bytes& data);
Bytes decrypt_spt(const Bytes& data);

// One string as laid out in the original text block.
struct BlockString {
    std::size_t offset = 0;  // byte offset in the decrypted file
    std::size_t byte_len = 0;
    Bytes raw;
};

// Offset of the next 0x66660001 section marker at or after `start`, or the
// buffer size when there is none.
std::size_t find_next_section_marker(const Bytes& dec, std::size_t start);

// Start of the text block: after the 12-byte section marker and the h_10-entry
// offset table.  Falls back to the lowest translated offset.
// Returns SIZE_MAX when there is nothing to do.
std::size_t find_text_block_start(const Bytes& dec,
                                  const std::vector<std::size_t>& translated_offsets);

// Sequential NUL-terminated 4-aligned strings, stopping at the next section.
std::vector<BlockString> parse_text_block(const Bytes& dec, std::size_t start,
                                          std::size_t* block_end);

int run_repack(const std::string& translated_file, const std::string& spt_dir,
               const std::string& backup_dir);

// Copy every .spt back out of the backup directory.
int run_restore(const std::string& spt_dir, const std::string& backup_dir);

}  // namespace exc::repack
