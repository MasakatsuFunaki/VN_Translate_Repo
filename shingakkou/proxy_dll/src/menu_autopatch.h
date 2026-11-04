// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Keyless SHINMENU v1 XOR-delta menu patcher and first-launch auto-patcher
// for Data\sin_sysd.dat and Bootup.dat.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace menu_autopatch {

// Applies a SHINMENU v1 XOR-delta patch blob to unpatched DDP2 archives
// (Data\sin_sysd.dat and Bootup.dat) in memory.
// Returns true and populates out_patched_sin_sysd and out_patched_bootup on
// success; returns false on any header, entry count, or MurmurHash2 mismatch.
bool ApplyXorMenuPatchInMemory(
    const std::vector<std::uint8_t>& orig_sin_sysd,
    const std::vector<std::uint8_t>& orig_bootup,
    const std::vector<std::uint8_t>& patch_blob,
    std::vector<std::uint8_t>& out_patched_sin_sysd,
    std::vector<std::uint8_t>& out_patched_bootup);

// Synchronizes via Local\ShingakkouMenuPatchMutex, backs up Data\sin_sysd.dat
// and Bootup.dat to .bak if needed, and atomically patches them from
// <game_dir>\menu_patch.bin when either archive is still unpatched.
bool EnsureMenuArchivePatchedInDir(const std::wstring& game_dir);

// Startup entrypoint called from TranslatorInit() before the engine opens
// Bootup.dat or Data\sin_sysd.dat.
void EnsureMenuArchivePatched();

}  // namespace menu_autopatch
