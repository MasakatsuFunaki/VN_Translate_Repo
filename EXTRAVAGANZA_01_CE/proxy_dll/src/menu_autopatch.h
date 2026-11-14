// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace menu_autopatch {

// Computes the 32-bit MurmurHash2 digest (seed = 0, m = 0x5BD1E995, r = 24)
// used to verify original and patched file/entry payloads.
std::uint32_t ComputeMurmurHash2(const std::uint8_t* data, std::size_t len) noexcept;

// Applies or removes the keyless position-dependent XOR mask in place.
// Because the mask depends only on `orig_slice` and `orig_data_hash`, calling
// this function a second time with the same arguments restores the original buffer.
void XorMaskPayload(std::uint8_t* buf,
                    std::size_t buf_len,
                    const std::uint8_t* orig_slice,
                    std::size_t orig_len,
                    std::uint32_t orig_data_hash) noexcept;

// Ensures that all 29 target game files in `game_dir` (`16` `spt\*.spt`,
// `8` `chart\chart1..8.fxf`, `spt\charaname.xtx`, `init2\mainControl.fxf`,
// `init2\backlog.fxf`, `dwq\sys0.gpk`, and `dwq\sys0.gtb`) have been backed up
// to `<file>.bak` and patched using `<game_dir>\extravaganza_patch.bin`,
// recording `<game_dir>\extravaganza_patch.bin.applied` for fast-path launches.
//
// Returns true if the game files were patched on this call or were already
// patched on a previous launch. Returns false without modifying any game file
// if `extravaganza_patch.bin` is absent or fails validation.
bool EnsureGameFilesPatchedInDir(const std::wstring& game_dir);

// Resolves the directory containing the running `winmm.dll` (or host executable)
// and invokes `EnsureGameFilesPatchedInDir`.
bool EnsureGameFilesPatched();

}  // namespace menu_autopatch
