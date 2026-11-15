// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// menu_autopatch.h
//
// First-launch atomic auto-patcher for CROSS_CHANNEL's data\sys.cpk UI archive.
// Invoked from TranslatorInit() before cc.exe opens data\sys.cpk.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace menu_autopatch {

inline constexpr const wchar_t* kDefaultMutexName =
    L"Local\\CrossChannelMenuPatchMutex";
inline constexpr std::uint32_t kDefaultMutexTimeoutMs = 10000u;

enum class PatchStatus {
    kApplied,             // Patched data\sys.cpk from data\sys.cpk.bak + menu_patch.bin
    kAlreadyUpToDate,     // Fast-path hit via data\sys.cpk.applied + size checks
    kSkippedMissingInput, // menu_patch.bin or data\sys.cpk(.bak) absent; benign no-op
    kFailed               // Corrupt patch/archive, hash mismatch, mutex or I/O error
};

struct PatchResult {
    PatchStatus status = PatchStatus::kFailed;
    std::uint64_t patched_size = 0;
};

// Stateful auto-patcher with configurable synchronization settings (PIMPL).
class MenuAutoPatcher {
public:
    explicit MenuAutoPatcher(
        std::wstring mutex_name = kDefaultMutexName,
        std::uint32_t mutex_timeout_ms = kDefaultMutexTimeoutMs);
    ~MenuAutoPatcher();

    MenuAutoPatcher(MenuAutoPatcher&&) noexcept;
    MenuAutoPatcher& operator=(MenuAutoPatcher&&) noexcept;

    MenuAutoPatcher(const MenuAutoPatcher&) = delete;
    MenuAutoPatcher& operator=(const MenuAutoPatcher&) = delete;

    PatchResult Apply(const std::filesystem::path& game_dir) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Applies <game_dir>\menu_patch.bin to <game_dir>\data\sys.cpk:
//   - Synchronizes via Local\CrossChannelMenuPatchMutex.
//   - Fast-paths Launch 2+ in <1 ms via data\sys.cpk.applied + size checks.
//   - Atomically backs up unpatched data\sys.cpk to data\sys.cpk.bak on first
//     launch (never overwriting an existing valid .bak).
//   - Always patches from data\sys.cpk.bak into data\sys.cpk.tmp and commits
//     via MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) so
//     updated menu_patch.bin releases cleanly re-apply.
// Returns true on kApplied or kAlreadyUpToDate; false otherwise.
bool ApplyMenuPatch(const std::filesystem::path& game_dir);

// Detailed variant exposing the exact PatchStatus for diagnostics and testing.
PatchResult ApplyMenuPatchDetailed(const std::filesystem::path& game_dir);

// Zero-argument startup helper resolving the game directory from the host
// executable, proxy DLL module path, or current working directory.
bool EnsureMenuArchivePatched();

}  // namespace menu_autopatch
