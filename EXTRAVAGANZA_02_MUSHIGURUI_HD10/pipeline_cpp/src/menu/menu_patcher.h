// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Multi-archive and multi-edition menu patch builder ("MSGMENU1" v1) for
// EXTRAVAGANZA ~Mushigurui~ (HD10 and Complete Edition).
//
// Wire layout of `menu_patch.bin` (all multi-byte integers are little-endian):
//
//   1. Fixed 32-byte file header:
//        [0x00..0x08)  char     magic[8]                = "MSGMENU1"
//        [0x08..0x0C)  uint32_t version                 = 1
//        [0x0C..0x10)  uint32_t standalone_group_count  = 3
//        [0x10..0x14)  uint32_t archive_count           = 3
//        [0x14..0x18)  uint32_t flags                   = 0
//        [0x18..0x20)  uint64_t reserved                = 0
//
//   2. Standalone file groups (`standalone_group_count` records):
//        uint16_t path_len
//        char     rel_path[path_len]   // ASCII, '\\'-separated relative to game dir
//                                      // ("spt\\charaname.xtx",
//                                      //  "init2\\backlog.fxf",
//                                      //  "init2\\mainControl.fxf")
//        uint16_t variant_count        // 1 for shared files, 2 for mainControl.fxf
//        Repeated `variant_count` times:
//          uint32_t orig_file_size
//          uint32_t orig_data_hash     // ComputeMurmurHash2(orig, orig_file_size, 0)
//          uint32_t new_file_size
//          uint32_t new_data_hash      // ComputeMurmurHash2(patched, new_file_size, 0)
//          uint8_t  masked_payload[new_file_size]
//                   // `patched` masked in-place via:
//                   // XorMaskPayload(buf, new_file_size, orig, orig_file_size, orig_data_hash)
//
//      At runtime, a standalone group matches an installed file when the
//      pristine file (or backup copy) matches `(orig_file_size, orig_data_hash)`
//      of one of the group's variants, and is considered already up-to-date when
//      the live file matches `(new_file_size, new_data_hash)` of that variant.
//
//   3. GPK/GTB archive sections (`archive_count` records: "dwq\\sys0", "dwq\\ta0", "dwq\\ev0"):
//        uint16_t stem_len
//        char     archive_stem[stem_len]   // "<subdir>\\<stem>", append ".gpk" / ".gtb"
//        uint64_t orig_gpk_size
//        uint64_t new_gpk_size
//        uint32_t orig_gtb_size
//        uint32_t orig_gtb_hash            // ComputeMurmurHash2(orig_gtb, orig_gtb_size, 0)
//        uint32_t new_gtb_size             // == orig_gtb_size (dual 32/64-bit offsets updated)
//        uint32_t new_gtb_hash             // ComputeMurmurHash2(new_gtb, new_gtb_size, 0)
//        uint32_t patched_entry_count
//        uint8_t  masked_gtb[new_gtb_size]
//                 // `new_gtb` masked in-place via:
//                 // XorMaskPayload(buf, new_gtb_size, orig_gtb, orig_gtb_size, orig_gtb_hash)
//        Repeated `patched_entry_count` times (strictly ascending `entry_index`):
//          uint32_t entry_index
//          uint64_t orig_offset
//          uint32_t orig_size
//          uint32_t orig_data_hash         // ComputeMurmurHash2(orig_entry, orig_size, 0)
//          uint32_t new_size
//          uint32_t new_data_hash          // ComputeMurmurHash2(new_entry, new_size, 0)
//          uint8_t  masked_entry[new_size]
//                   // `new_entry` masked in-place via:
//                   // XorMaskPayload(buf, new_size, orig_entry, orig_size, orig_data_hash)
//
//   4. Keyless self-inverse payload masking (`XorMaskPayload`) and `ComputeMurmurHash2`:
//        - `ComputeMurmurHash2(data, len, seed = 0)` uses MurmurHash2 (x86_32)
//          with `m = 0x5BD1E995u`, `r = 24`, `seed = 0u`, `h = seed ^ uint32_t(len)`.
//        - `XorMaskPayload(buf, len, orig_slice, orig_len, orig_data_hash)` XORs
//          each byte `buf[i]` (`0 <= i < len`) with:
//            pos_mix   = uint8_t(((orig_data_hash >> ((i & 3u) * 8u)) & 0xFFu) ^
//                                (uint32_t(i) * 0x9Eu + 0x37u))
//            orig_byte = (orig_slice != nullptr && orig_len > 0)
//                            ? orig_slice[i % orig_len] : 0u
//            buf[i]   ^= uint8_t(orig_byte ^ pos_mix)
//          Applying `XorMaskPayload` a second time with the same pristine slice
//          and `orig_data_hash` restores the exact unmasked bytes.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mgi::menu {

inline constexpr std::size_t kMenuPatchMagicSize = 8;
inline constexpr char kMenuPatchMagic[kMenuPatchMagicSize] = {
    'M', 'S', 'G', 'M', 'E', 'N', 'U', '1'};
inline constexpr std::uint32_t kMenuPatchFormatVersion = 1u;
inline constexpr std::size_t kMenuPatchHeaderSize = 32;

inline constexpr std::uint32_t kExpectedStandaloneGroupCount = 3u;
inline constexpr std::uint32_t kExpectedArchiveCount = 3u;
inline constexpr std::uint32_t kExpectedSys0PatchedCount = 59u;
inline constexpr std::uint32_t kExpectedTa0PatchedCount = 12u;
inline constexpr std::uint32_t kExpectedEv0PatchedCount = 3u;
inline constexpr std::uint32_t kExpectedTotalPatchedEntryCount =
    kExpectedSys0PatchedCount + kExpectedTa0PatchedCount + kExpectedEv0PatchedCount;

inline constexpr std::uint32_t kMurmurHash2Multiplier = 0x5BD1E995u;
inline constexpr int kMurmurHash2Shift = 24;
inline constexpr std::uint32_t kMurmurHash2DefaultSeed = 0u;

inline constexpr std::uint32_t kXorHashByteIndexMask = 3u;
inline constexpr std::uint32_t kXorBitsPerByte = 8u;
inline constexpr std::uint32_t kXorByteMask = 0xFFu;
inline constexpr std::uint32_t kXorPositionStride = 0x9Eu;
inline constexpr std::uint32_t kXorPositionBias = 0x37u;

inline constexpr std::size_t kDwqHeaderSize = 64;
inline constexpr std::size_t kDwqTypeFieldSize = 16;
inline constexpr std::size_t kDwqCropMarginsOffset = 0x10;
inline constexpr std::size_t kDwqCropMarginCount = 4;
inline constexpr std::size_t kDwqDataSizeOffset = 0x20;
inline constexpr std::size_t kDwqWidthOffset = 0x24;
inline constexpr std::size_t kDwqHeightOffset = 0x28;
inline constexpr std::size_t kDwqBppOffset = 0x2A;
inline constexpr std::size_t kDwqPackTypeOffset = 0x2C;
inline constexpr std::size_t kDwqPackTypeFieldSize = 20;

inline constexpr std::size_t kGtbCountFieldSize = 4;
inline constexpr std::size_t kGtbU32EntryStride = 4;
inline constexpr std::size_t kGtbU64EntryStride = 8;
inline constexpr std::size_t kGtbOver2GTrailerSize = 8;
inline constexpr std::size_t kGtbOver2GFooterPaddingBytes = 16;
inline constexpr char kGtbOver2GTrailer[kGtbOver2GTrailerSize] = {
    'o', 'v', 'e', 'r', '2', 'G', '!', '\0'};

struct RgbaImage {
    int width = 0;
    int height = 0;
    int channels = 4;
    std::vector<std::uint8_t> pixels;
};

struct ParsedGtbTable {
    std::uint32_t entry_count = 0;
    bool has_over2g_u64_table = false;
    std::vector<std::uint64_t> offsets;  // size == entry_count + 1; offsets.back() == gpk_size
};

struct StandaloneVariantRecord {
    std::uint32_t orig_file_size = 0;
    std::uint32_t orig_data_hash = 0;
    std::uint32_t new_file_size = 0;
    std::uint32_t new_data_hash = 0;
    std::vector<std::uint8_t> masked_payload;
};

struct StandaloneGroupRecord {
    std::string rel_path;
    std::vector<StandaloneVariantRecord> variants;
};

struct ArchiveEntryRecord {
    std::uint32_t entry_index = 0;
    std::uint64_t orig_offset = 0;
    std::uint32_t orig_size = 0;
    std::uint32_t orig_data_hash = 0;
    std::uint32_t new_size = 0;
    std::uint32_t new_data_hash = 0;
    std::vector<std::uint8_t> masked_entry;
};

struct ArchiveSectionRecord {
    std::string archive_stem;
    std::uint64_t orig_gpk_size = 0;
    std::uint64_t new_gpk_size = 0;
    std::uint32_t orig_gtb_size = 0;
    std::uint32_t orig_gtb_hash = 0;
    std::uint32_t new_gtb_size = 0;
    std::uint32_t new_gtb_hash = 0;
    std::vector<std::uint8_t> masked_gtb;
    std::vector<ArchiveEntryRecord> entries;
};

struct ParsedMenuPatchBin {
    std::uint32_t version = kMenuPatchFormatVersion;
    std::uint32_t flags = 0;
    std::uint64_t reserved = 0;
    std::vector<StandaloneGroupRecord> standalone_groups;
    std::vector<ArchiveSectionRecord> archives;
};

struct MenuPatchOptions {
    std::filesystem::path hd10_game_dir;
    std::filesystem::path ce_game_dir;
    std::filesystem::path output_patch_bin;
};

struct MenuPatchSummary {
    std::filesystem::path output_patch_bin;
    std::size_t patch_bin_bytes = 0;
    std::uint32_t standalone_group_count = 0;
    std::uint32_t standalone_variant_count = 0;
    std::uint32_t archive_count = 0;
    std::uint32_t patched_entry_count = 0;
};

// MurmurHash2 (x86_32) with seed = 0 by default.
std::uint32_t ComputeMurmurHash2(const std::uint8_t* data,
                                 std::size_t len,
                                 std::uint32_t seed = kMurmurHash2DefaultSeed);

// Self-inverse delta XOR mask tying every stored payload to the user's pristine
// installation bytes plus a position-dependent MurmurHash2 mix.
void XorMaskPayload(std::uint8_t* buf,
                    std::size_t len,
                    const std::uint8_t* orig_slice,
                    std::size_t orig_len,
                    std::uint32_t orig_data_hash);

// Bitwise-NOT (`~byte`) codec for `init2/*.fxf` configuration files.
std::vector<std::uint8_t> encrypt_fxf(const std::string& plain_cp932);
std::string decrypt_fxf(const std::vector<std::uint8_t>& raw);

// Pure CP932 text transformers for the three standalone menu/config files.
std::string patch_charaname_xtx_cp932(const std::string& orig_cp932);
std::string patch_backlog_fxf_cp932(const std::string& orig_cp932);
std::string patch_main_control_fxf_cp932(const std::string& orig_cp932);

// GTB offset-table reader and dual-table ("over2G!\0" 32-bit + 64-bit) updater.
ParsedGtbTable parse_gtb_offsets(const std::vector<std::uint8_t>& gtb_bytes,
                                 std::uint64_t gpk_size);
std::vector<std::uint8_t> update_gtb_offsets(
    const std::vector<std::uint8_t>& orig_gtb,
    const std::vector<std::uint64_t>& new_offsets);

// DWQ 64-byte header preservation and PNG image helpers.
std::array<std::int32_t, kDwqCropMarginCount> compute_rgba_crop_margins(
    const RgbaImage& img);
std::vector<std::uint8_t> wrap_dwq_png_preserving_header(
    const std::vector<std::uint8_t>& orig_dwq_raw,
    const std::vector<std::uint8_t>& encoded_png,
    bool recompute_crop_margins,
    const RgbaImage* decoded_for_crop = nullptr);
RgbaImage decode_png_rgba(const std::uint8_t* png_data, std::size_t png_size);
std::vector<std::uint8_t> encode_png(const RgbaImage& img,
                                     bool write_24bpp_rgb = false);

// Binary serialization, strict validation, and in-memory reconstruction of
// `menu_patch.bin` ("MSGMENU1" v1).
std::vector<std::uint8_t> serialize_menu_patch_bin(const ParsedMenuPatchBin& patch);
ParsedMenuPatchBin parse_menu_patch_bin(const std::vector<std::uint8_t>& patch_bytes);

std::vector<std::uint8_t> reconstruct_standalone_file(
    const StandaloneGroupRecord& group,
    const std::vector<std::uint8_t>& orig_bytes);

std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>
reconstruct_archive_pair(const ArchiveSectionRecord& section,
                         const std::vector<std::uint8_t>& orig_gtb,
                         const std::vector<std::uint8_t>& orig_gpk);

struct VerifiedArchiveEntry {
    std::uint32_t entry_index = 0;
    std::uint64_t new_offset = 0;
    std::uint32_t new_size = 0;
    std::vector<std::uint8_t> unmasked_dwq;
};

struct VerifiedArchiveStreamResult {
    std::vector<std::uint8_t> new_gtb;
    std::uint64_t computed_new_gpk_size = 0;
    std::vector<VerifiedArchiveEntry> patched_entries;
};

// Streaming archive verifier that validates GTB/GPK sizes, MurmurHash2 values,
// dual 32/64-bit offset tables, and unmasked DWQ payloads by seeking only to
// the patched entry slices inside large on-disk `.gpk` archives.
VerifiedArchiveStreamResult verify_and_unmask_archive_from_disk(
    const ArchiveSectionRecord& section,
    const std::filesystem::path& orig_gtb_path,
    const std::filesystem::path& orig_gpk_path);

// PIMPL-insulated builder that renders all 74 English UI/omake/commentary
// sprites and standalone config variants from pristine HD10 + CE installations
// and writes `menu_patch.bin`.
class MenuPatchBuilder {
public:
    explicit MenuPatchBuilder(MenuPatchOptions options);
    ~MenuPatchBuilder();

    MenuPatchBuilder(MenuPatchBuilder&&) noexcept;
    MenuPatchBuilder& operator=(MenuPatchBuilder&&) noexcept;

    MenuPatchBuilder(const MenuPatchBuilder&) = delete;
    MenuPatchBuilder& operator=(const MenuPatchBuilder&) = delete;

    MenuPatchSummary build();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mgi::menu
