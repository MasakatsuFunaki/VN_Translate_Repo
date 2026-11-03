// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "disasm.h"

#include <stdexcept>

#include <capstone/capstone.h>

namespace ksh::pe {

struct Disassembler::Impl {
    csh handle = 0;

    Impl() {
        if (cs_open(CS_ARCH_X86, CS_MODE_32, &handle) != CS_ERR_OK)
            throw std::runtime_error("capstone: cannot open an x86-32 handle");
    }
    ~Impl() {
        if (handle) cs_close(&handle);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

std::string format_instruction(const Instruction& ins) {
    return "  0x" + hexu(ins.address, 8) + "  " + ljust(hex_bytes(ins.bytes), 24) + "  " +
           ins.mnemonic + " " + ins.op_str;
}

Disassembler::Disassembler() : impl_(std::make_unique<Impl>()) {}
Disassembler::~Disassembler() = default;
Disassembler::Disassembler(Disassembler&&) noexcept = default;
Disassembler& Disassembler::operator=(Disassembler&&) noexcept = default;

std::vector<Instruction> Disassembler::disasm(const std::uint8_t* code, std::size_t size,
                                              std::uint64_t address) const {
    std::vector<Instruction> out;
    cs_insn* insn = nullptr;
    const std::size_t count = cs_disasm(impl_->handle, code, size, address, 0, &insn);
    if (!insn) return out;

    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Instruction ins;
        ins.address = insn[i].address;
        ins.bytes.assign(insn[i].bytes, insn[i].bytes + insn[i].size);
        ins.mnemonic = insn[i].mnemonic;
        ins.op_str = insn[i].op_str;
        out.push_back(std::move(ins));
    }
    cs_free(insn, count);
    return out;
}

}  // namespace ksh::pe
