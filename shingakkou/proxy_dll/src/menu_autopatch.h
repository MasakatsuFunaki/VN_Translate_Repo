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
#include <memory>
#include <string>
#include <vector>

namespace menu_autopatch {

enum class PatchStatus {
    kApplied,
    kAlreadyUpToDate,
    kSkippedMissingInput,
    kFailed,
};

struct PatchResult {
    PatchStatus status = PatchStatus::kFailed;
    std::uint64_t patched_sin_sysd_bytes = 0u;
    std::uint64_t patched_bootup_bytes = 0u;
};

// Synchronized, idempotent SHINMENU v1 auto-patcher for Data\sin_sysd.dat
// and Bootup.dat. Creates atomic .bak backups, tracks applied patch identity
// via <game_dir>\menu_patch.applied so v2 updates re-apply cleanly from .bak,
// and recovers missing live archives from .bak.
class MenuAutoPatcher {
public:
    static constexpr const wchar_t* kDefaultMutexName =
        L"Local\\ShingakkouMenuPatchMutex";
    static constexpr std::uint32_t kDefaultMutexTimeoutMs = 10000u;

    explicit MenuAutoPatcher(
        std::wstring mutex_name = kDefaultMutexName,
        std::uint32_t mutex_timeout_ms = kDefaultMutexTimeoutMs);
    ~MenuAutoPatcher();

    MenuAutoPatcher(const MenuAutoPatcher&) = delete;
    MenuAutoPatcher& operator=(const MenuAutoPatcher&) = delete;
    MenuAutoPatcher(MenuAutoPatcher&&) noexcept;
    MenuAutoPatcher& operator=(MenuAutoPatcher&&) noexcept;

    PatchResult Apply(const std::wstring& game_dir) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

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
// <game_dir>\menu_patch.bin when not yet up to date with menu_patch.applied.
bool EnsureMenuArchivePatchedInDir(const std::wstring& game_dir);

// Startup entrypoint called from TranslatorInit() before the engine opens
// Bootup.dat or Data\sin_sysd.dat.
void EnsureMenuArchivePatched();

}  // namespace menu_autopatch
