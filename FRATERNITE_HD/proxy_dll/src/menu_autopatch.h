// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Keyless XOR-delta menu patcher and first-launch auto-patcher for pac\cgsys.ypf.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace menu_autopatch {

// Applies a FRATMENU v1 XOR-delta patch blob to an unpatched YPF v490 archive in memory.
// Returns true and populates out_patched_ypf on success; returns false on any header,
// entry count, MurmurHash2, or PNG IHDR mismatch.
bool ApplyXorMenuPatchInMemory(
    const std::vector<std::uint8_t>& orig_ypf,
    const std::vector<std::uint8_t>& patch_blob,
    std::vector<std::uint8_t>& out_patched_ypf);

// Synchronizes via Local\FraterniteHdMenuPatchMutex, backs up <game_dir>\pac\cgsys.ypf
// to <game_dir>\pac\cgsys.ypf.bak if needed, and atomically patches <game_dir>\pac\cgsys.ypf
// from <game_dir>\cgsys_patch.bin when pac\cgsys.ypf is still unpatched.
bool EnsureMenuArchivePatchedInDir(const std::wstring& game_dir);

// Startup entrypoint called from TranslatorInit() before the engine opens pac\cgsys.ypf.
void EnsureMenuArchivePatched();

}  // namespace menu_autopatch
