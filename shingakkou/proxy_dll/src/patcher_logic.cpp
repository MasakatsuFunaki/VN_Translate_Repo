// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "patcher_logic.h"

#include <cstring>

namespace patcher_logic {

bool BytesMatch(const unsigned char* addr,
                const unsigned char* expected,
                std::size_t len)
{
    return std::memcmp(addr, expected, len) == 0;
}

void FillNops(unsigned char* dst, std::size_t len)
{
    std::memset(dst, 0x90, len);
}

std::int32_t ComputeRel32(std::uintptr_t instr_start,
                          std::uintptr_t target)
{
    const std::uintptr_t post = instr_start + 5;
    return static_cast<std::int32_t>(
        static_cast<std::intptr_t>(target) - static_cast<std::intptr_t>(post));
}

unsigned char* EmitRelOp32(unsigned char* out,
                           unsigned char opcode,
                           std::uintptr_t target)
{
    out[0] = opcode;
    const std::int32_t rel = ComputeRel32(
        reinterpret_cast<std::uintptr_t>(out), target);
    std::memcpy(out + 1, &rel, sizeof(rel));
    return out + 5;
}

unsigned char* EmitBytes(unsigned char* out,
                         const unsigned char* src,
                         std::size_t len)
{
    std::memcpy(out, src, len);
    return out + len;
}

bool IsValidSpec(const PatchSpec& s)
{
    if (s.expected == nullptr) return false;
    if (s.len == 0) return false;
    if (s.replacement != nullptr && s.replacement == s.expected) return false;
    return true;
}

}  // namespace patcher_logic
