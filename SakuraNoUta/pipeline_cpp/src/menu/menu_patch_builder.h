// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// End-to-end builder for Sakura no Uta's keyless XOR-delta menu container
// (`menu_patch.bin`, `"SNUMENU1"` v1), raw-asset-signature verifier, visual QA
// contact-sheet generator, and 1280x720 preview exporter.
//
// Insulation: Win32 child-process handles for optional external CBG v2 fallback
// decoding stay strictly inside `menu_patch_builder.cpp`.
//
// Thread-compatible.
#pragma once

#include "menu_patch_format.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace snu::menu {

inline constexpr std::size_t kExpectedSplashRecordCount = 2u;
inline constexpr std::size_t kExpectedSysGrpRecordCount = 82u;
inline constexpr std::size_t kExpectedChoiceRecordCount = 34u;
inline constexpr std::size_t kExpectedRetailRecordCount =
    kExpectedSplashRecordCount + kExpectedSysGrpRecordCount +
    kExpectedChoiceRecordCount;
inline constexpr std::size_t kForbiddenContainerZeroRunBytes = 64u;

struct MenuPatchBuildOptions {
    std::filesystem::path project_dir;
    std::filesystem::path game_dir;
    std::filesystem::path output_patch_path;
    std::filesystem::path contact_sheets_dir;
    std::filesystem::path previews_dir;

    // When true (default for retail builds), requires all 2 splash + 82 sysgrp
    // + 34 choice records to be present. Component tests over smaller synthetic
    // archives may set `strict_retail_inventory = false`.
    bool strict_retail_inventory = true;
};

struct MenuPatchBuildSummary {
    std::size_t splash_records = 0u;
    std::size_t sysgrp_records = 0u;
    std::size_t choice_records = 0u;
    std::size_t total_records = 0u;
    std::size_t patch_bytes = 0u;
    std::uint32_t manifest_hash = 0u;
    std::vector<std::filesystem::path> contact_sheet_files;
    std::vector<std::filesystem::path> preview_files;
};

// Scans `blob[0x20 .. EOF)` and returns false (populating `*violation_out` when
// non-null) if any record payload begins with `"BM"`, or if PNG magic
// (`89 50 4E 47 0D 0A 1A 0A`), `"BURIKO ARC20"`, `"DSC FORMAT 1.00"`,
// `"CompressedBG___"`, or `>= 64` consecutive `0x00` bytes appear in the body.
bool VerifyNoRawAssetSignatures(menu_patch_format::ByteSpan blob,
                                std::string* violation_out = nullptr);

// Throws `std::runtime_error` if `VerifyNoRawAssetSignatures(blob)` fails.
void AssertNoRawAssetSignatures(menu_patch_format::ByteSpan blob);

// Atomically writes `blob` to `output_path` via `<output_path>.tmp` + rename.
void WriteBinaryFileAtomic(const std::filesystem::path& output_path,
                           menu_patch_format::ByteSpan blob);

class MenuPatchBuilder {
public:
    MenuPatchBuilder();
    MenuPatchBuilder(const MenuPatchBuilder&) = delete;
    MenuPatchBuilder& operator=(const MenuPatchBuilder&) = delete;
    MenuPatchBuilder(MenuPatchBuilder&& other) noexcept;
    MenuPatchBuilder& operator=(MenuPatchBuilder&& other) noexcept;
    ~MenuPatchBuilder();

    // Renders all localized UI, splash, music, dialog, tooltip, and choice
    // assets from `options.game_dir`, serializes `"SNUMENU1"` v1 bytes,
    // enforces `AssertNoRawAssetSignatures`, writes `options.output_patch_path`
    // atomically (when non-empty), and emits optional contact sheets/previews.
    MenuPatchBuildSummary Build(const MenuPatchBuildOptions& options,
                                std::vector<std::uint8_t>* patch_bytes_out = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

MenuPatchBuildSummary BuildMenuPatch(
    const MenuPatchBuildOptions& options,
    std::vector<std::uint8_t>* patch_bytes_out = nullptr);

}  // namespace snu::menu
