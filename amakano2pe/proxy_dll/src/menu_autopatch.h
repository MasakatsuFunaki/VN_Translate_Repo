// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// menu_autopatch.h  (amakano2pe / CatSystem2)
//
// First-launch auto-patcher. CatSystem2 runs with <direct>1</direct>, so it
// prefers loose files under <GameDir>\{image,fes,config,kcs,export}\ over the
// entries inside the retail .int archives. This component reconstructs those
// loose English override files from <GameDir>\menu_patch.bin ("AMK2MENU" v1)
// by reading the original byte slices from the untouched retail archives,
// verifying their digests, and unmasking the keyless XOR deltas. Standalone
// root files patched in place are first preserved as <file>.bak.
//
// Every file is written as <file>.tmp and swapped in with
// MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), and the
// whole run is serialized across processes by the named mutex
// Local\Amakano2PeMenuPatchMutex. A successful run leaves
// <GameDir>\menu_patch.applied so later launches skip all archive reads.
//
// Thread-safe.

#include <string>

namespace menu_autopatch {

enum class PatchOutcome {
    kNoPatchBundle,   // menu_patch.bin absent: dialogue-only install
    kAlreadyApplied,  // sentinel matches the bundle and every override exists
    kApplied,         // every record reconstructed; sentinel written
    kPartiallyApplied,  // some records failed; sentinel withheld for a retry
    kFailed,          // bundle unreadable/invalid or lock not acquired
};

const char* PatchOutcomeName(PatchOutcome outcome) noexcept;

// Applies <game_dir>\menu_patch.bin. Never throws.
PatchOutcome EnsureMenuAssetsPatched(const std::wstring& game_dir) noexcept;

// Applies the bundle next to the host executable. Never throws.
PatchOutcome EnsureMenuAssetsPatchedForHostProcess() noexcept;

}  // namespace menu_autopatch
