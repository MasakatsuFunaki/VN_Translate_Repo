// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "hg3/hg3_codec.h"
#include "menu/core/menu_patch_container.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ama::menu {

using MenuPatchEntrySpec = MenuPatchRecordInput;

struct ExtractedAssetEntry {
    std::string archive_rel_path;
    std::string entry_name;
    std::uint64_t orig_offset = 0;
    std::vector<std::uint8_t> orig_raw_slice;
    std::vector<std::uint8_t> plain_bytes;
};

struct DomainRenderOptions {
    std::filesystem::path contact_sheet_dir;
    bool export_contact_sheets = false;
};

class GameAssetSource {
public:
    explicit GameAssetSource(std::filesystem::path game_dir);
    GameAssetSource(std::filesystem::path game_dir, const std::string& vcode2);
    ~GameAssetSource();

    GameAssetSource(GameAssetSource&&) noexcept;
    GameAssetSource& operator=(GameAssetSource&&) noexcept;

    GameAssetSource(const GameAssetSource&) = delete;
    GameAssetSource& operator=(const GameAssetSource&) = delete;

    const std::filesystem::path& game_dir() const noexcept;

    bool HasArchiveEntry(const std::string& archive_rel_path,
                         const std::string& entry_name) const;
    std::vector<std::string> ListArchiveEntries(const std::string& archive_rel_path) const;

    ExtractedAssetEntry ReadArchiveEntry(const std::string& archive_rel_path,
                                         const std::string& entry_name) const;
    ExtractedAssetEntry ReadStandaloneFile(const std::string& rel_path) const;
    ExtractedAssetEntry ReadExecutableSlice(std::uint64_t offset = 0,
                                            std::uint32_t length = 4096) const;

    ExtractedAssetEntry FindAndReadHg3(const std::string& hg3_filename) const;
    ama::hg3::Hg3Container LoadHg3(const std::string& hg3_filename,
                                   ExtractedAssetEntry* out_source = nullptr) const;

    static MenuPatchEntrySpec BuildHg3PatchEntry(const ExtractedAssetEntry& source_entry,
                                                 const ama::hg3::Hg3Container& patched_hg3);
    static MenuPatchEntrySpec BuildRawPatchEntry(const ExtractedAssetEntry& source_entry,
                                                 std::string target_rel_path,
                                                 std::vector<std::uint8_t> new_raw_bytes,
                                                 std::uint8_t flags = kFlagLooseOverride);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void ExportHg3ContactSheetPages(const std::string& hg3_filename,
                                const ama::hg3::Hg3Container& container,
                                const std::filesystem::path& output_dir);

class IMenuDomainRenderer {
public:
    virtual ~IMenuDomainRenderer();

    virtual std::string DomainName() const = 0;
    virtual std::vector<MenuPatchEntrySpec> RenderDomain(
        const GameAssetSource& source, const DomainRenderOptions& options) = 0;
};

}  // namespace ama::menu
