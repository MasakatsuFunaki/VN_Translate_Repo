// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Lossless YPF v490 menu sprite archive patcher with PNG IHDR validation.
#pragma once

#include "common/util.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace frat::menu {

struct PngHeaderInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bit_depth = 0;
    std::uint8_t color_type = 0;
};

struct PatchConfig {
    std::string game_dir;
    std::string assets_dir;
    std::string archive_rel_path = "pac\\cgsys.ypf";
};

struct PatchSummary {
    std::size_t total_entries = 0;
    std::size_t replaced_entries = 0;
    std::size_t discovered_assets = 0;
    bool backup_created = false;
    std::string archive_path;
    std::string backup_path;
};

// REQUIRES: png contains a valid PNG signature followed by a 13-byte IHDR chunk.
// Throws std::runtime_error if the buffer is not a valid PNG header.
PngHeaderInfo parse_png_ihdr(const Bytes& png, const std::string& label = "png");

// Thread-compatible: concurrent const access is safe.
class MenuArchivePatcher {
public:
    MenuArchivePatcher();
    MenuArchivePatcher(const MenuArchivePatcher&) = delete;
    MenuArchivePatcher& operator=(const MenuArchivePatcher&) = delete;
    MenuArchivePatcher(MenuArchivePatcher&&) noexcept;
    MenuArchivePatcher& operator=(MenuArchivePatcher&&) noexcept;
    ~MenuArchivePatcher();

    Bytes repack_archive_bytes(
        const Bytes& original_archive,
        const std::unordered_map<std::string, Bytes>& replacements,
        PatchSummary* summary = nullptr) const;

    PatchSummary patch_archive(const PatchConfig& config) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace frat::menu
