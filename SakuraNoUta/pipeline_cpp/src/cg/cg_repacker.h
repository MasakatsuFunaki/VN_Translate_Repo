// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Universal BGI-native CG archive encoder and idempotent `.backup`-based
// BURIKO ARC20 repacker.
//
// Emits `DSC FORMAT 1.00` wrapping BGI's 16-byte raw BG header (`encode_sysgrp`)
// for both 24-bpp BGR and 32-bpp straight-BGRA images, which is accepted by all
// BGI graphic opcodes (`$9010`, `$9214`, `$8130`, `$90C5`) and header probes,
// avoiding Windows `"BM"` BMP payloads that `$9214` rejects.
//
// Thread-compatible.
#pragma once

#include "bgi/bgi_sysgrp.h"
#include "cg/image.h"
#include "common/util.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace snu::cg {

struct ArchiveBgSpec {
    int width = 0;
    int height = 0;
    int bpp = 0;  // 24 or 32
    std::array<std::uint8_t, bgi::kSysGrpReservedBytes> reserved{};
};

struct RepackResult {
    std::size_t applied_count = 0;
    std::size_t skipped_count = 0;
    std::string backup_path;
};

// Default excluded archive entries shared by scan, translate, and repack.
const std::set<std::string>& default_skip_entries();

// True when `entry_name` is in `default_skip_entries()`.
bool is_skipped_cg_entry(std::string_view entry_name) noexcept;

// Probes the original dimensions, bit depth (24 or 32), and 10-byte BGI header
// reserved fields of a raw archive entry (`CompressedBG___`, `DSC FORMAT 1.00`
// wrapping a 16-byte BGI BG header, uncompressed 16-byte BGI BG, or Windows
// BMP).  Returns std::nullopt when the format is unrecognized, truncated, has
// non-positive dimensions, or uses an unsupported bit depth (e.g. 8-bpp).
std::optional<ArchiveBgSpec> probe_archive_bg_spec(const Bytes& raw_entry);

// Encodes `patched` as `DSC FORMAT 1.00` wrapping a 16-byte native BGI BG
// header (`target_bpp` must be 24 or 32) and top-down unpadded BGR/BGRA pixels.
//
// When `target_bpp == 32`:
//   - uses `patched.alpha` if present;
//   - otherwise copies `alpha_donor->alpha` if `alpha_donor` is provided and
//     carries an alpha channel of matching dimensions;
//   - otherwise fills opaque alpha (0xFF).
//
// Throws std::invalid_argument on empty image, unsupported `target_bpp`,
// malformed `patched.rgb` or `patched.alpha`, or mismatched/malformed
// `alpha_donor`.
Bytes encode_bgi_archive_bg(
    const Image& patched, int target_bpp, const Image* alpha_donor = nullptr,
    const std::array<std::uint8_t, bgi::kSysGrpReservedBytes>& reserved = {});

// Idempotently repacks BURIKO ARC20 archives from their pristine `.backup`
// copy while enforcing a skip list on replacement keys.
//
// Thread-compatible.
class ArchiveRepacker {
public:
    ArchiveRepacker();
    explicit ArchiveRepacker(std::set<std::string> skip_entries);
    ArchiveRepacker(const ArchiveRepacker&) = delete;
    ArchiveRepacker& operator=(const ArchiveRepacker&) = delete;
    ArchiveRepacker(ArchiveRepacker&& other) noexcept;
    ArchiveRepacker& operator=(ArchiveRepacker&& other) noexcept;
    ~ArchiveRepacker();

    bool is_skipped(std::string_view entry_name) const noexcept;

    // Repacks `arc_path` using `<arc_path>.backup` as the immutable base
    // archive so removed or skipped entries revert to original bytes on every
    // re-run:
    //   - Excludes any key in `replacements` matching `is_skipped()`.
    //   - When no `.backup` exists and no replacements survive filtering,
    //     leaves `arc_path` untouched and creates no `.backup`.
    //   - Otherwise builds `<arc_path>.tmp` from `<arc_path>.backup` (or
    //     `arc_path` on first run), creates `<arc_path>.backup` if absent, and
    //     replaces `arc_path` with `<arc_path>.tmp`.
    //   - On failure, removes `<arc_path>.tmp` and leaves existing files intact.
    RepackResult repack_from_backup(
        const std::string& arc_path,
        const std::map<std::string, Bytes>& replacements) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snu::cg
