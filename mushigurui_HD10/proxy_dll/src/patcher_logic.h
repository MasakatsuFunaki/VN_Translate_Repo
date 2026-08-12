// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// patcher_logic.h
//
// Pure (OS-independent) byte-level helpers used by the mushigurui_HD10
// runtime patcher AND translator. Everything here is free of Windows
// API calls so the same object file can be linked into both the proxy
// DLL and the gtest binary.
//
// Companion patcher.cpp / translator.cpp wire these primitives up to
// the real engine: they allocate executable memory, walk the IAT,
// install hooks. The tricky byte-level arithmetic (rel32 encoding for
// JMP/CALL, NOP fills, trampoline emission, byte-prefix verification)
// lives here where it can be exercised without having to load the DLL
// into the game process.
//
// On MSVC the whole project is compiled with /utf-8 so the Japanese
// glyphs that appear in comments here (e.g. 虫愚類 -- the game's
// title) survive regardless of the system codepage -- important on
// this Japanese-locale box where the default is still CP932.

#include <cstddef>
#include <cstdint>

namespace patcher_logic {

// True iff the `len` bytes at `addr` match `expected` exactly. Thin
// wrapper around memcmp that keeps callers free of <cstring> and
// makes patch-site verification a one-liner.
bool BytesMatch(const unsigned char* addr,
                const unsigned char* expected,
                std::size_t len);

// Fill `len` bytes at `dst` with 0x90 (x86 NOP). Used to blank out
// patch ranges we want the CPU to skip over at runtime.
void FillNops(unsigned char* dst, std::size_t len);

// Compute the 32-bit signed displacement that the CPU needs to reach
// `target` from a rel32 instruction (E8 CALL / E9 JMP) whose opcode
// byte sits at `instr_start`. The instruction is assumed to be
// 5 bytes total: 1 opcode + 4-byte rel.
//
//   rel = (int32)(target - (instr_start + 5))
//
// Returned as int32 because that is what the encoding holds; callers
// typically memcpy it into the buffer.
std::int32_t ComputeRel32(std::uintptr_t instr_start,
                          std::uintptr_t target);

// Emit a single-byte-opcode + rel32 instruction at `out`, which when
// executed from address `out` will transfer control to `target`.
//
//   out[0]      = opcode
//   out[1..4]   = (int32) target - (out + 5)
//
// Advances the caller's cursor and returns it (out + 5). The typical
// opcodes used here are 0xE8 (CALL rel32) and 0xE9 (JMP rel32); the
// helper is opcode-agnostic so future one-byte rel32 forms slot in
// without a new function.
//
// NOTE: `out` must already point into executable memory the caller
// controls. This helper writes bytes only; it does NOT call
// VirtualProtect or flush instruction caches -- that's the caller's
// job.
unsigned char* EmitRelOp32(unsigned char* out,
                           unsigned char opcode,
                           std::uintptr_t target);

inline unsigned char* EmitRelCall32(unsigned char* out,
                                    std::uintptr_t target) {
    return EmitRelOp32(out, 0xE8, target);
}
inline unsigned char* EmitRelJmp32(unsigned char* out,
                                   std::uintptr_t target) {
    return EmitRelOp32(out, 0xE9, target);
}

// Copy `len` raw bytes from `src` to `out` and return the advanced
// cursor. Used to splice the original instruction bytes into a
// trampoline verbatim before the JMP-back.
unsigned char* EmitBytes(unsigned char* out,
                         const unsigned char* src,
                         std::size_t len);

// Heuristic: does `val` (a 32-bit value read from memory) look like
// four ASCII bytes? Used by the crash handler to decide whether a
// dereferenced pointer is a string. "Looks like ASCII" here means at
// least 3 of the 4 bytes are printable (0x20..0x7E).
//
// Purely a predicate over the integer value -- no memory access --
// which is why it lives in the logic module.
bool LooksLikeAscii(std::uint32_t val);

// A PatchSpec describes one byte-level patch: RVA (relative to the
// EXE's ImageBase), the bytes we expect to find there, and the bytes
// to write. The replacement pointer may be null -- in that case the
// patcher fills the range with NOPs (0x90) at `len` length.
struct PatchSpec {
    std::uint32_t        rva;
    const unsigned char* expected;
    const unsigned char* replacement;   // may be null => nop-fill
    std::size_t          len;
};

// Validate structural invariants of a PatchSpec without touching
// memory. Returns false if:
//   - expected is null
//   - len == 0
//   - replacement is non-null but points at the same buffer as
//     expected (would have been a no-op patch -- almost always a bug)
bool IsValidSpec(const PatchSpec& s);

}  // namespace patcher_logic
