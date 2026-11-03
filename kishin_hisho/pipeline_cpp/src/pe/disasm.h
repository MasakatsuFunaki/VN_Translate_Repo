// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Thin RAII wrapper over Capstone's x86-32 decoder.
//
// The tools print `address / raw bytes / mnemonic / op_str`, so that is all
// this exposes -- deliberately not a general disassembly abstraction.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/util.h"

namespace ksh::pe {

struct Instruction {
    std::uint64_t address = 0;
    Bytes bytes;
    std::string mnemonic;
    std::string op_str;
};

// The line every analysis tool prints an instruction with:
//   "  0xAAAAAAAA  AA BB CC<padded to 24>  mnemonic op_str"
// The separator before op_str is emitted unconditionally, so an operand-less
// instruction keeps a TRAILING space.  The committed logs contain it, so this
// must not be trimmed.
std::string format_instruction(const Instruction& ins);

class Disassembler {
public:
    // Throws std::runtime_error if Capstone cannot open an x86-32 handle.
    Disassembler();
    ~Disassembler();
    Disassembler(Disassembler&&) noexcept;
    Disassembler& operator=(Disassembler&&) noexcept;
    Disassembler(const Disassembler&) = delete;
    Disassembler& operator=(const Disassembler&) = delete;

    // Decode from `code` as though it were loaded at `address`.  Stops at the
    // first byte that does not decode, exactly as capstone's disasm() does.
    std::vector<Instruction> disasm(const std::uint8_t* code, std::size_t size,
                                    std::uint64_t address) const;
    std::vector<Instruction> disasm(const Bytes& code, std::uint64_t address) const {
        return disasm(code.data(), code.size(), address);
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ksh::pe
