// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Deterministic DDP2 menu archive patcher, SHINMENU v1 keyless XOR-delta codec,
// and TGA 24/32-bit RAW/RLE codec for Data/sin_sysd.dat and Bootup.dat.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "common/util.h"

namespace shin::menu {

inline constexpr std::size_t kTgaHeaderByteCount = 18;
inline constexpr std::uint8_t kTgaImageTypeUncompressedTrueColor = 2;
inline constexpr std::uint8_t kTgaImageTypeRleTrueColor = 10;
inline constexpr std::uint8_t kTgaBitDepth24 = 24;
inline constexpr std::uint8_t kTgaBitDepth32 = 32;
inline constexpr std::uint8_t kTgaDescriptorOriginMask = 0x20U;
inline constexpr std::size_t kBitsPerByte = 8;

inline constexpr std::size_t kXorMenuPatchMagicSize = 8;
inline constexpr std::uint32_t kXorMenuPatchFormatVersion = 1;
inline constexpr std::size_t kXorMenuPatchHeaderSize = 36;
inline constexpr std::size_t kXorMenuPatchRecordHeaderSize = 20;
inline constexpr std::uint32_t kXorMenuArchiveIdSinSysd = 0;
inline constexpr std::uint32_t kXorMenuArchiveIdBootup = 1;

struct TgaHeaderInfo {
    std::uint8_t id_length = 0;
    std::uint8_t color_map_type = 0;
    std::uint8_t image_type = 0;
    std::uint16_t color_map_first_entry = 0;
    std::uint16_t color_map_length = 0;
    std::uint8_t color_map_entry_size = 0;
    std::uint16_t x_origin = 0;
    std::uint16_t y_origin = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t bpp = 0;
    std::uint8_t descriptor = 0;
};

struct MenuPatchOptions {
    std::string game_dir;
    std::string assets_dir;
    std::string patch_file;
    std::string build_patch_out;
    bool verify = false;
};

struct MenuPatchStats {
    std::size_t sin_sysd_total_entries = 0;
    std::size_t sin_sysd_patched_tga_count = 0;
    std::size_t sin_sysd_patched_csv_count = 0;
    std::size_t sin_sysd_patched_raw_count = 0;
    std::size_t bootup_total_entries = 0;
    std::size_t bootup_patched_count = 0;
    bool verified = false;
};

// REQUIRES: data contains a valid 24-bit or 32-bit uncompressed (type 2) or
// RLE (type 10) TrueColor TGA header.
TgaHeaderInfo ReadTgaHeader(const Bytes& data);
TgaHeaderInfo ReadTgaInfo(const Bytes& data);

// REQUIRES: data contains a complete 24-bit or 32-bit type-2 or type-10 TGA.
// Returns the decoded BGR(A) pixel buffer of size width * height * (bpp / 8)
// in the scanline order stored in the TGA file.
Bytes DecodeTgaPixels(const Bytes& data);

// REQUIRES: pixels.size() == header.width * header.height * (header.bpp / 8).
// Encodes pixels into a complete TGA file matching header's 18-byte layout and
// image_type (type 2 RAW or type 10 RLE).
Bytes EncodeTga(const TgaHeaderInfo& header, const Bytes& pixels);

// Computes 32-bit MurmurHash2 (m = 0x5BD1E995, r = 24, seed = len).
std::uint32_t ComputeMenuMurmurHash2(const std::uint8_t* data, std::size_t len);

// Builds a SHINMENU v1 keyless XOR-delta patch blob from unpatched DDP2
// archives (sin_sysd.dat.bak and Bootup.dat.bak) and uncompressed replacement
// payloads keyed by entry index.
Bytes BuildXorMenuPatchBlob(const Bytes& orig_sin_sysd,
                            const std::map<std::size_t, Bytes>& sin_sysd_replacements,
                            const Bytes& orig_bootup,
                            const std::map<std::size_t, Bytes>& bootup_replacements);

// Validates a SHINMENU v1 XOR-delta patch blob against orig_sin_sysd and
// orig_bootup, unmasks each modified entry, wraps it with shs_wrap, and
// reconstructs out_patched_sin_sysd and out_patched_bootup matching
// shin::ddp::repack_ddp2 byte-for-byte.
void ApplyXorMenuPatchBlobToDdp2(const Bytes& orig_sin_sysd,
                                 const Bytes& orig_bootup,
                                 const Bytes& patch_blob,
                                 Bytes& out_patched_sin_sysd,
                                 Bytes& out_patched_bootup);

// Thread-compatible: concurrent const access is safe, but mutating operations
// require external synchronization.
class MenuArchivePatcher {
public:
    explicit MenuArchivePatcher(MenuPatchOptions options);
    MenuArchivePatcher(const MenuArchivePatcher&) = delete;
    MenuArchivePatcher& operator=(const MenuArchivePatcher&) = delete;
    MenuArchivePatcher(MenuArchivePatcher&& other) noexcept;
    MenuArchivePatcher& operator=(MenuArchivePatcher&& other) noexcept;
    ~MenuArchivePatcher();

    MenuPatchStats PatchArchives();

    const MenuPatchOptions& options() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace shin::menu
