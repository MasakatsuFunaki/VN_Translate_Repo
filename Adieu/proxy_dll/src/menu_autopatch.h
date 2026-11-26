// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

// Loader-lock-safe (pure kernel32.dll) two-phase first-launch auto-patcher for
// Adieu's Gameexe.dat and g00\*.g00 UI assets.
//
// Guarded across processes by named mutex Local\AdieuMenuPatchMutex:
//   - Returns kNoPatchFile when <game_dir>\menu_patch.bin does not exist.
//   - Fast-paths Launch 2+ (< 1 ms) via <game_dir>\menu_patch.applied
//     ("ADIEUAPL" v1) without reading asset payloads when patch size,
//     manifest_hash, and target byte sizes on disk all match.
//   - Stages all immutable backups (<target>.bak.tmp), unmasked payloads
//     (<target>.stage.tmp), and dropped-target restorations from <target>.bak
//     (<target>.restore.tmp) with FlushFileBuffers in Phase 1 before swapping
//     any live file via MoveFileExW in Phase 2, guaranteeing zero partial live
//     mutations if any record fails validation.

namespace menu_autopatch {

inline constexpr wchar_t kMenuPatchFileName[] = L"menu_patch.bin";
inline constexpr wchar_t kMenuPatchSentinelFileName[] = L"menu_patch.applied";
inline constexpr wchar_t kMenuPatchMutexName[] = L"Local\\AdieuMenuPatchMutex";

enum class MenuPatchOutcome {
    kNoPatchFile,
    kAlreadyApplied,
    kApplied,
    kFailed,
};

const char* MenuPatchOutcomeName(MenuPatchOutcome outcome) noexcept;

// Validates and atomically applies <game_dir>\menu_patch.bin. Never throws.
MenuPatchOutcome EnsureMenuAssetsPatched(const wchar_t* game_dir) noexcept;

}  // namespace menu_autopatch
