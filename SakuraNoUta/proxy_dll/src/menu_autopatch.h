// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

// Loader-lock-safe (pure kernel32.dll) two-phase first-launch auto-patcher for
// Sakura no Uta's sysgrp.arc UI sprites, att01/att02 splash BMPs, and
// select01-1..select17-2 choice BMPs.
//
// Guarded across processes by named mutex Local\SakuraNoUtaMenuPatchMutex:
//   - Returns kNoPatchFile when <game_dir>\menu_patch.bin does not exist.
//   - Fast-paths Launch 2+ (< 1 ms) via <game_dir>\menu_patch.applied
//     ("SNUMAPL1" v1) without reading asset payloads when patch size,
//     manifest_hash, and every unique target byte size on disk match.
//   - Caches loaded source archives (.bak, .backup, live) in memory by
//     lowercase source_rel_path and falls back from orig_offset to
//     FindArc20EntrySlice(entry_name) when an archive was repacked by CG tools.
//   - Stages all immutable backups (<target>.bak.tmp) and unmasked/repacked
//     targets (<target>.stage.tmp) with FlushFileBuffers in Phase 1 before
//     promoting any live file via MoveFileExW in Phase 2, leaving zero .tmp
//     files or partial live mutations on failure.

namespace menu_autopatch {

inline constexpr wchar_t kMenuPatchFileName[] = L"menu_patch.bin";
inline constexpr wchar_t kMenuPatchSentinelFileName[] = L"menu_patch.applied";
inline constexpr wchar_t kMenuPatchMutexName[] =
    L"Local\\SakuraNoUtaMenuPatchMutex";

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
