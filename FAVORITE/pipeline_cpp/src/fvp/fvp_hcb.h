// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// FAVORITE / FVP `.hcb` script string extractor.
//
// Format (verified against Sakura.hcb, FAVORITE 2019):
//
//     [0..3]      uint32 LE  script_len
//     [4..script_len]        bytecode
//     [script_len..]         <main_script_start uint32 LE> + extra binary
//
// Bytecode is a stream of single-byte opcodes plus typed args.  We only care
// about opcode 0x0E (string push):
//
//     0x0E <LEN_BYTE> <BYTES> 0x00     # LEN_BYTE = len(BYTES) + 1
//
// All other opcodes have fixed-size args; we walk the stream so a stray 0x0E
// inside another opcode's payload is never misread as a string.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/util.h"

namespace fav::fvp {

// One string-push instruction.
struct StringRef {
    std::uint32_t offset = 0;  // offset of the 0x0E opcode byte itself
    Bytes raw;                 // CP932 payload without the trailing NUL
    std::string decoded;       // UTF-8 (CP932 strict, else U+FFFD replacement)
};

// Arg kinds an opcode's operand list can hold.
enum class Arg : char { B = 'b', W = 'w', D = 'd', S = 's' };

// Opcode -> operand list.  Returns nullptr for an unknown opcode.
const std::vector<Arg>* opcode_args(std::uint8_t op);

// True when `op` is a known opcode.
bool is_known_opcode(std::uint8_t op);

// Walk an `.hcb` bytecode stream and return every 0x0E (string-push) opcode
// in stream order.  Throws std::runtime_error on an unknown opcode (the
// format drifted) or a file too small to be an .hcb.
std::vector<StringRef> extract_strings(const Bytes& data);

// Convenience: read the file and extract.
std::vector<StringRef> load_all(const std::string& path);

// One decoded instruction: the opcode plus, for a 0x0E string push, its
// payload (empty optional when the opcode has no string arg or the CP932
// decode failed).
struct Event {
    std::uint8_t op = 0;
    std::optional<std::string> payload;
};

// Walk the whole bytecode stream recording every instruction.  Unlike
// extract_strings this STOPS at the first unknown opcode instead of throwing
// -- the cast-roster gate only needs the 0x10 -> 0x0E pairs that precede any
// such drift.
std::vector<Event> walk_events(const Bytes& data);

}  // namespace fav::fvp
