// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Keyless XOR-delta menu patcher and first-launch auto-patcher for pac\cgsys.ypf.
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
    std::uint64_t patched_archive_bytes = 0u;
};

// Synchronized, idempotent FRATMENU v1 auto-patcher for pac\cgsys.ypf.
// Creates atomic .bak backups, tracks applied patch identity via
// pac\cgsys.ypf.applied so v2 updates re-apply cleanly from .bak, and
// recovers a missing live pac\cgsys.ypf from .bak.
class MenuAutoPatcher {
public:
    static constexpr const wchar_t* kDefaultMutexName =
        L"Local\\FraterniteHdMenuPatchMutex";
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

// Applies a FRATMENU v1 XOR-delta patch blob to an unpatched YPF v490 archive in memory.
// Returns true and populates out_patched_ypf on success; returns false on any header,
// entry count, MurmurHash2, or PNG IHDR mismatch.
bool ApplyXorMenuPatchInMemory(
    const std::vector<std::uint8_t>& orig_ypf,
    const std::vector<std::uint8_t>& patch_blob,
    std::vector<std::uint8_t>& out_patched_ypf);

// Synchronizes via Local\FraterniteHdMenuPatchMutex, backs up <game_dir>\pac\cgsys.ypf
// to <game_dir>\pac\cgsys.ypf.bak if needed, and atomically patches <game_dir>\pac\cgsys.ypf
// from <game_dir>\cgsys_patch.bin when not yet up to date with pac\cgsys.ypf.applied.
bool EnsureMenuArchivePatchedInDir(const std::wstring& game_dir);

// Startup entrypoint called from TranslatorInit() before the engine opens pac\cgsys.ypf.
void EnsureMenuArchivePatched();

}  // namespace menu_autopatch
