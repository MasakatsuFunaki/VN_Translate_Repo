// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace exm::menu {

constexpr size_t kCropMarginCount = 4;

inline constexpr size_t kPatchMagicSize = 8;
inline constexpr char kPatchMagic[kPatchMagicSize] = {
    'A', 'G', 'H', 'M', 'E', 'N', 'U', '1'
};
inline constexpr uint32_t kPatchFormatVersion = 1;
inline constexpr size_t kPatchHeaderSize = 44;
inline constexpr size_t kStandaloneRecordMetaSize = 16;
inline constexpr size_t kGpkEntryRecordHeaderSize = 24;
inline constexpr size_t kExpectedStandaloneFileCount = 3;
inline constexpr size_t kExpectedModifiedGpkEntryCount = 21;

struct RgbaImage {
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t bpp = 0;
    bool has_alpha = false;
    std::string pack_type;
    std::array<int32_t, kCropMarginCount> crop_margins{0, 0, 0, 0};
    std::vector<uint8_t> pixels;  // Top-down RGBA (width * height * 4)
};

struct StandalonePatchRecord {
    std::string rel_path;
    uint32_t orig_file_size = 0;
    uint32_t orig_data_hash = 0;
    uint32_t new_file_size = 0;
    uint32_t new_data_hash = 0;
    std::vector<uint8_t> masked_payload;
};

struct GpkEntryPatchRecord {
    uint32_t entry_index = 0;
    uint32_t orig_offset = 0;
    uint32_t orig_size = 0;
    uint32_t orig_data_hash = 0;
    uint32_t new_size = 0;
    uint32_t new_data_hash = 0;
    std::vector<uint8_t> masked_entry;
};

struct ParsedPatchBin {
    uint32_t version = 0;
    uint32_t standalone_file_count = 0;
    uint32_t orig_gpk_size = 0;
    uint32_t new_gpk_size = 0;
    uint32_t orig_gtb_size = 0;
    uint32_t orig_gtb_hash = 0;
    uint32_t new_gtb_size = 0;
    uint32_t new_gtb_hash = 0;
    uint32_t gpk_entry_count = 0;
    std::vector<StandalonePatchRecord> standalone_records;
    std::vector<uint8_t> masked_gtb;
    std::vector<GpkEntryPatchRecord> gpk_records;
};

struct MenuBuildSummary {
    size_t total_archive_entries = 0;
    size_t patched_sprite_entries = 0;
    size_t untouched_sprite_entries = 0;
    size_t patched_standalone_files = 0;
    bool patched_main_control_fxf = false;
    bool patched_backlog_fxf = false;
};

// Compute 32-bit MurmurHash2 (m = 0x5BD1E995, r = 24, default seed = 0).
uint32_t ComputeMurmurHash2(const uint8_t* data, size_t len, uint32_t seed = 0) noexcept;

// 4-byte repeating key XOR helper.
void XorMaskPayload(uint8_t* data, size_t len, uint32_t key) noexcept;

// Self-inverse keyless XOR mask using original asset bytes and MurmurHash2.
void XorMaskPayload(uint8_t* buf,
                    size_t buf_len,
                    const uint8_t* orig_slice,
                    size_t orig_len,
                    uint32_t orig_data_hash) noexcept;

// Parse and validate an AGHMENU1 v1 keyless XOR-delta container in memory.
ParsedPatchBin parse_patch_bin(const std::vector<uint8_t>& patch_bin);

// Bitwise-NOT (.fxf) configuration script encryption/decryption.
std::string decrypt_fxf(const std::vector<uint8_t>& raw);
std::vector<uint8_t> encrypt_fxf(const std::string& cp932_text);

// Compute [top, left, right, bottom] transparent bounding-box margins for 32bpp RGBA sprites.
std::array<int32_t, kCropMarginCount> compute_crop_margins(const RgbaImage& img);

// Decode a DWQ entry (PACKTYPE=8A, PACKTYPE=0, or PACKTYPE=2A) to top-down RGBA.
RgbaImage decode_dwq_to_rgba(const uint8_t* dwq, size_t size);

// Encode a top-down RGBA image back into a DWQ entry matching orig_dwq's header and pack type.
std::vector<uint8_t> encode_dwq_from_rgba(const uint8_t* orig_dwq, size_t orig_size,
                                          const RgbaImage& img,
                                          bool update_crop_margins = true);

// Encode a PACKTYPE=8A DWQ entry directly from pre-rendered PNG file bytes.
std::vector<uint8_t> encode_dwq_from_png_bytes(const uint8_t* orig_dwq, size_t orig_size,
                                               const std::vector<uint8_t>& png_bytes,
                                               bool update_crop_margins = true);

// Read/write PNG files via Windows Imaging Component (WIC).
RgbaImage read_png_rgba(const std::filesystem::path& path);
void write_png_rgba(const std::filesystem::path& path, const RgbaImage& img);

// Decode/encode raw PNG byte buffers in memory via WIC.
RgbaImage decode_png_bytes_to_rgba(const uint8_t* png_data, size_t png_size);
std::vector<uint8_t> encode_rgba_to_png_bytes(const RgbaImage& img, bool write_alpha);

// Patch init2\mainControl.fxf and init2\backlog.fxf decoded CP932 text.
std::string patch_main_control_fxf(const std::string& cp932_text);
std::string patch_backlog_fxf(const std::string& cp932_text);

// PIMPL-insulated GPK/GTB archive patcher.
class MenuArchivePatcher {
public:
    MenuArchivePatcher();
    ~MenuArchivePatcher();

    MenuArchivePatcher(MenuArchivePatcher&&) noexcept;
    MenuArchivePatcher& operator=(MenuArchivePatcher&&) noexcept;

    MenuArchivePatcher(const MenuArchivePatcher&) = delete;
    MenuArchivePatcher& operator=(const MenuArchivePatcher&) = delete;

    void load(const std::filesystem::path& gpk_path,
              const std::filesystem::path& gtb_path);
    void load_from_memory(const std::vector<uint8_t>& gpk_bytes,
                          const std::vector<uint8_t>& gtb_bytes);

    size_t entry_count() const;
    std::string entry_name(size_t index) const;
    uint32_t entry_offset(size_t index) const;
    uint32_t entry_size(size_t index) const;
    const std::vector<uint8_t>& entry_raw_bytes(size_t index) const;

    RgbaImage decode_entry_rgba(size_t index) const;
    void replace_entry_rgba(size_t index, const RgbaImage& img,
                            bool update_crop_margins = true);
    void replace_entry_png_bytes(size_t index, const std::vector<uint8_t>& png_bytes,
                                 bool update_crop_margins = true);
    void replace_entry_raw(size_t index, std::vector<uint8_t> raw_dwq);

    std::pair<std::vector<uint8_t>, std::vector<uint8_t>> serialize() const;
    void save(const std::filesystem::path& out_gpk_path,
              const std::filesystem::path& out_gtb_path) const;

    static std::vector<uint8_t> build_patch_bin_from_game_dir(
        const std::filesystem::path& game_dir);
    static void write_patch_bin_from_game_dir(
        const std::filesystem::path& game_dir,
        const std::filesystem::path& out_patch_bin_path);

    static std::pair<std::vector<uint8_t>, std::vector<uint8_t>>
    reconstruct_archive_from_patch_bin(
        const std::vector<uint8_t>& patch_bin,
        const std::vector<uint8_t>& orig_gtb_bytes,
        const std::vector<uint8_t>& orig_gpk_bytes);

    static MenuBuildSummary apply_patch_bin_to_game_dir(
        const std::filesystem::path& game_dir,
        const std::filesystem::path& patch_bin_path,
        bool test_mode = false);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

using MenuPatcher = MenuArchivePatcher;

struct PatchMenusOptions {
    std::filesystem::path game_dir;
    std::filesystem::path rendered_dir;
    std::filesystem::path manifest_path;
    std::filesystem::path patch_bin_path;
    std::filesystem::path write_patch_bin_path;
    bool extract_only = false;
    std::filesystem::path extract_dir;
    bool test_mode = false;
};

int run_patch_menus(const PatchMenusOptions& opt);

}  // namespace exm::menu
