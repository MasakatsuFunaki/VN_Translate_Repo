// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// BLACKCyc GTB/GPK/DWQ menu graphics archive parser/repacker and init2/*.fxf
// system UI configuration patcher.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/util.h"

namespace exc::menus {

inline constexpr std::size_t kDwqHeaderSize = 64;
inline constexpr std::size_t kBmpFileHeaderSize = 14;
inline constexpr std::size_t kBmpInfoHeaderSize = 40;
inline constexpr std::size_t kBmpHeaderSize = kBmpFileHeaderSize + kBmpInfoHeaderSize;
inline constexpr std::size_t kMaskPaletteSize = 1024;
inline constexpr std::size_t kMaskHeaderTotalSize = kBmpHeaderSize + kMaskPaletteSize;

inline constexpr std::size_t kDwqCropTopOffset = 0x10;
inline constexpr std::size_t kDwqCropLeftOffset = 0x14;
inline constexpr std::size_t kDwqCropRightOffset = 0x18;
inline constexpr std::size_t kDwqCropBottomOffset = 0x1C;
inline constexpr std::size_t kDwqDataSizeOffset = 0x20;
inline constexpr std::size_t kDwqWidthOffset = 0x24;
inline constexpr std::size_t kDwqHeightOffset = 0x28;
inline constexpr std::size_t kDwqPackTypeOffset = 0x39;
inline constexpr std::size_t kDwqCropFlagOffset = 0x3A;

inline constexpr char kPackTypeUncompressedBmp = '0';
inline constexpr char kPackTypeBmpWithMask = '2';
inline constexpr char kCropFlagActive = 'A';

inline constexpr int kRgbaChannels = 4;
inline constexpr int kRedChannel = 0;
inline constexpr int kGreenChannel = 1;
inline constexpr int kBlueChannel = 2;
inline constexpr int kAlphaChannel = 3;

inline constexpr int kBgrChannels = 3;
inline constexpr int kBgrBlueIndex = 0;
inline constexpr int kBgrGreenIndex = 1;
inline constexpr int kBgrRedIndex = 2;

inline constexpr int kBmpRowAlignment = 4;
inline constexpr int kMaxRleZeroRun = 255;

inline constexpr std::size_t kNumberedPrefixLength = 4;
inline constexpr std::size_t kNumberedPrefixSeparatorIndex = 3;
inline constexpr char kNumberedPrefixSeparator = '_';

struct RgbaImage {
    int width = 0;
    int height = 0;
    Bytes pixels;  // Top-down row-major RGBA8888 (width * height * 4 bytes)
};

struct CropMargins {
    std::int32_t top = 0;
    std::int32_t left = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

struct GtbEntryInfo {
    std::uint32_t index = 0;
    std::string name;
    std::uint32_t data_offset = 0;
    std::uint32_t data_size = 0;
    char pack_type = 0;
    bool has_crop_margins = false;
    int width = 0;
    int height = 0;
};

struct MenuBuildOptions {
    std::string game_dir;
    std::string rendered_png_dir;
    std::string patch_bin_path;
    std::string write_patch_bin_path;
    bool test_mode = false;
};

struct MenuBuildSummary {
    std::size_t total_archive_entries = 0;
    std::size_t patched_sprite_entries = 0;
    std::size_t untouched_sprite_entries = 0;
    std::size_t patched_standalone_files = 0;
    bool patched_main_control_fxf = false;
    bool patched_backlog_fxf = false;
};

inline constexpr std::size_t kPatchMagicSize = 8;
inline constexpr char kPatchMagic[kPatchMagicSize] = {
    'E', 'X', 'C', 'E', 'P', 'A', 'T', '1'
};
inline constexpr std::uint32_t kPatchFormatVersion = 1;
inline constexpr std::size_t kPatchHeaderSize = 44;
inline constexpr std::size_t kStandaloneRecordMetaSize = 16;
inline constexpr std::size_t kGpkEntryRecordHeaderSize = 24;
inline constexpr std::size_t kExpectedStandaloneFileCount = 27;
inline constexpr std::size_t kExpectedModifiedGpkEntryCount = 64;

struct StandalonePatchRecord {
    std::string rel_path;
    std::uint32_t orig_file_size = 0;
    std::uint32_t orig_data_hash = 0;
    std::uint32_t new_file_size = 0;
    std::uint32_t new_data_hash = 0;
    Bytes masked_payload;
};

struct GpkEntryPatchRecord {
    std::uint32_t entry_index = 0;
    std::uint32_t orig_offset = 0;
    std::uint32_t orig_size = 0;
    std::uint32_t orig_data_hash = 0;
    std::uint32_t new_size = 0;
    std::uint32_t new_data_hash = 0;
    Bytes masked_entry;
};

struct ParsedPatchBin {
    std::uint32_t version = 0;
    std::uint32_t standalone_file_count = 0;
    std::uint32_t orig_gpk_size = 0;
    std::uint32_t new_gpk_size = 0;
    std::uint32_t orig_gtb_size = 0;
    std::uint32_t orig_gtb_hash = 0;
    std::uint32_t new_gtb_size = 0;
    std::uint32_t new_gtb_hash = 0;
    std::uint32_t gpk_entry_count = 0;
    std::vector<StandalonePatchRecord> standalone_records;
    Bytes masked_gtb;
    std::vector<GpkEntryPatchRecord> gpk_records;
};

// Compute 32-bit MurmurHash2 (m = 0x5BD1E995, r = 24, seed = 0).
std::uint32_t ComputeMurmurHash2(const std::uint8_t* data, std::size_t len) noexcept;

// Self-inverse keyless XOR mask using the original asset bytes and MurmurHash2.
void XorMaskPayload(std::uint8_t* buf,
                    std::size_t buf_len,
                    const std::uint8_t* orig_slice,
                    std::size_t orig_len,
                    std::uint32_t orig_data_hash) noexcept;

// Parse and validate an EXCEPAT1 v1 keyless XOR-delta container in memory.
ParsedPatchBin parse_patch_bin(const Bytes& patch_bin);

// Decode a BLACKCyc RLE + vertical XOR compressed 8-bit mask stream into
// top-down row-major alpha bytes (width * height).
Bytes decode_rle_xor_mask(const std::uint8_t* stream, std::size_t stream_size,
                          int width, int height);

// Encode top-down row-major alpha bytes (width * height) into a BLACKCyc
// RLE + vertical XOR compressed 8-bit mask stream.
Bytes encode_rle_xor_mask(const std::uint8_t* alpha, int width, int height);

// Compute active non-transparent (alpha > 0) bounding margins for a sprite.
CropMargins compute_crop_margins(const RgbaImage& img);

// Decode a 24-bit PACKTYPE=0 or PACKTYPE=2A DWQ entry into a top-down RgbaImage.
RgbaImage decode_dwq_to_rgba(const std::uint8_t* dwq_data, std::size_t dwq_size);

// Re-encode a 24-bit PACKTYPE=0 or PACKTYPE=2A DWQ entry from a top-down
// RgbaImage while preserving the original DWQ/BMP headers and hidden RGB values
// on pixels that remain fully transparent (alpha == 0).
Bytes encode_dwq_from_rgba(const std::uint8_t* orig_dwq, std::size_t orig_size,
                           const RgbaImage& img, bool update_crop_margins = true);

// Read an 8-bit RGB or RGBA PNG file into a top-down RgbaImage.
RgbaImage read_png_rgba(const std::string& path);

// Patch init2/mainControl.fxf (encrypted with 0xFF XOR) with English window
// titles and startup error messages.
Bytes patch_main_control_fxf(const Bytes& encrypted_fxf);

// Patch init2/backlog.fxf (encrypted with 0xFF XOR) by translating
// firstMessage1 to "#青　　　　　　　　Ｂａｃｋｌｏｇ" while preserving "#青"
// and 2-byte CP932 character alignment.
Bytes patch_backlog_fxf(const Bytes& encrypted_fxf);

// PIMPL-insulated GTB + GPK archive container.  Untouched entries keep their
// exact original byte slices verbatim; modified entries re-encode their DWQ
// payload and update the GTB cumulative offset table.
class MenuArchivePatcher {
public:
    MenuArchivePatcher();
    ~MenuArchivePatcher();
    MenuArchivePatcher(MenuArchivePatcher&&) noexcept;
    MenuArchivePatcher& operator=(MenuArchivePatcher&&) noexcept;
    MenuArchivePatcher(const MenuArchivePatcher&) = delete;
    MenuArchivePatcher& operator=(const MenuArchivePatcher&) = delete;

    static MenuArchivePatcher load(const std::string& gtb_path, const std::string& gpk_path);
    static MenuArchivePatcher load_from_memory(const Bytes& gtb_bytes, const Bytes& gpk_bytes);

    std::size_t entry_count() const noexcept;
    std::vector<GtbEntryInfo> entries() const;
    std::optional<std::size_t> find_entry_index(const std::string& name) const;
    Bytes raw_entry_bytes(std::size_t index) const;

    void replace_entry_rgba(std::size_t index, const RgbaImage& img);
    void replace_entry_raw(std::size_t index, Bytes raw_dwq);

    // Serialize into {gtb_bytes, gpk_bytes}.
    std::pair<Bytes, Bytes> serialize() const;
    void save(const std::string& out_gtb_path, const std::string& out_gpk_path) const;

    // Build an EXCEPAT1 v1 keyless XOR-delta container from the backup and
    // patched files in a game directory.
    static Bytes build_patch_bin_from_game_dir(const std::string& game_dir);
    static void write_patch_bin_from_game_dir(const std::string& game_dir,
                                              const std::string& out_patch_bin_path);

    // Reconstruct {new_gtb_bytes, new_gpk_bytes} in memory from an EXCEPAT1 v1
    // patch container and the original unpatched sys0.gtb / sys0.gpk bytes.
    static std::pair<Bytes, Bytes> reconstruct_archive_from_patch_bin(
        const Bytes& patch_bin,
        const Bytes& orig_gtb_bytes,
        const Bytes& orig_gpk_bytes);

    // Apply an EXCEPAT1 v1 keyless XOR-delta container to a game directory.
    static MenuBuildSummary apply_patch_bin_to_game_dir(
        const std::string& game_dir,
        const std::string& patch_bin_path,
        bool test_mode = false);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

using GpkArchive = MenuArchivePatcher;

// Locate the default directory containing the 64 rendered menu PNG files
// (<project_dir>/build/menu_sprites).
std::string ResolveDefaultSpritesDir(const std::string& project_dir);
std::string default_rendered_png_dir(const std::string& project_dir);

// Repack dwq/sys0.gpk + dwq/sys0.gtb from dwq_backup/ and patch
// init2/mainControl.fxf + init2/backlog.fxf from init2_backup/.
MenuBuildSummary run_build_menus(const MenuBuildOptions& opt);

}  // namespace exc::menus
