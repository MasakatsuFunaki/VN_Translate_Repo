// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Runtime first-launch auto-patcher ("MSGMENU1" v1) for Mushigurui (HD10,
// HD10_Win7, and Complete Edition).  Streams multi-archive GPK/GTB updates
// (`dwq\sys0`, `dwq\ta0`, `dwq\ev0`) and edition-variant standalone files
// (`spt\charaname.xtx`, `init2\backlog.fxf`, `init2\mainControl.fxf`) using
// `.bak` backups and two-phase `.tmp` -> atomic replace commits.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace menu_autopatch {

inline constexpr std::uint32_t kMurmurDefaultSeed = 0u;

// Computes the 32-bit MurmurHash2 digest (m = 0x5BD1E995, r = 24, seed = 0 by
// default) used to verify original and patched file/entry payloads.
std::uint32_t ComputeMurmurHash2(const std::uint8_t* data,
                                 std::size_t len,
                                 std::uint32_t seed = kMurmurDefaultSeed) noexcept;

// Applies or removes the keyless position-dependent XOR mask in place.
// Calling this function a second time with the same `orig_slice` and
// `orig_data_hash` restores the unmasked payload bytes.
void XorMaskPayload(std::uint8_t* buf,
                    std::size_t buf_len,
                    const std::uint8_t* orig_slice,
                    std::size_t orig_len,
                    std::uint32_t orig_data_hash) noexcept;

// Ensures that all target game files and GPK/GTB archives referenced by
// `<game_dir>\menu_patch.bin` have been backed up to `<file>.bak` and patched.
//
// Thread-safe across processes via `Local\MushiguruiMenuPatchMutex`.
// Returns true if the game files were patched on this call or were already
// up-to-date.  Returns false without modifying any game file or leaving `.tmp`
// or newly created `.bak` artifacts if `menu_patch.bin` is absent or invalid.
bool EnsureGameFilesPatchedInDir(const std::wstring& game_dir);

// Resolves the directory containing the running `winmm.dll` (or host
// executable) and invokes `EnsureGameFilesPatchedInDir`.
bool EnsureGameFilesPatched();

}  // namespace menu_autopatch
