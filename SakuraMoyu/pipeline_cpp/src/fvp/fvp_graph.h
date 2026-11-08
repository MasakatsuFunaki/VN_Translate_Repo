// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// @PURPOSE: Decode, inspect, and atomically repack FVP (.bin / hzc1 / NVSG)
//           graphics archives.
//
// @CLASSES:
//   fav::fvp::ArchiveEntry:  Table-of-contents metadata for one archive entry.
//   fav::fvp::ArchiveHeader: Archive table of contents and raw CP932 name pool.
//   fav::fvp::NvsgImage:     Decoded 24-bit BGR or 32-bit BGRA NVSG texture.
//   fav::fvp::GraphArchive:  Insulated reader and atomic repacker for FVP .bin.
//
// @DESCRIPTION:
//   FVP graphics archives (such as graph.bin) begin with an 8-byte header
//   (uint32_t file_count, uint32_t name_pool_size) followed by file_count
//   12-byte table-of-contents records (name_offset, data_offset, data_size)
//   and a contiguous NUL-terminated CP932 filename pool.
//
//   Each compressed texture entry uses a 12-byte "hzc1" outer envelope
//   (4-byte magic "hzc1", uint32_t uncompressed_size, uint32_t header_length)
//   wrapping a 32-byte "NVSG" metadata header and a zlib-compressed pixel
//   payload in either 24-bit BGR (format 0) or 32-bit BGRA (format 1).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fav::fvp {

inline constexpr size_t kArchiveHeaderByteCount = 8;
inline constexpr size_t kTocEntryByteCount = 12;
inline constexpr size_t kHzc1EnvelopeHeaderByteCount = 12;
inline constexpr size_t kNvsgHeaderByteCount = 32;
inline constexpr size_t kCompressedStreamHeaderByteCount =
    kHzc1EnvelopeHeaderByteCount + kNvsgHeaderByteCount;

inline constexpr uint16_t kNvsgFormatBgr24 = 0;
inline constexpr uint16_t kNvsgFormatBgra32 = 1;
inline constexpr size_t kBgr24BytesPerPixel = 3;
inline constexpr size_t kBgra32BytesPerPixel = 4;

struct ArchiveEntry {
    uint32_t name_offset = 0;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    std::string name;  // UTF-8 decoded from CP932 name pool
};

struct ArchiveHeader {
    uint32_t file_count = 0;
    uint32_t name_pool_size = 0;
    std::vector<ArchiveEntry> entries;
    std::vector<uint8_t> raw_name_pool;
};

struct NvsgImage {
    uint16_t format = kNvsgFormatBgr24;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t offset_x = 0;
    uint16_t offset_y = 0;
    uint16_t unknown1 = 0;
    uint32_t frame_count = 1;
    uint32_t unknown2 = 0;
    uint32_t unknown3 = 0;
    std::array<uint8_t, kNvsgHeaderByteCount> raw_header{};
    bool has_raw_header = false;
    std::vector<uint8_t> pixels;

    size_t bytes_per_pixel() const noexcept {
        if (format == kNvsgFormatBgr24) {
            return kBgr24BytesPerPixel;
        }
        if (format == kNvsgFormatBgra32) {
            return kBgra32BytesPerPixel;
        }
        return 0;
    }

    size_t expected_byte_size() const noexcept {
        const size_t frames = (frame_count > 0) ? static_cast<size_t>(frame_count) : size_t{1};
        return static_cast<size_t>(width) * static_cast<size_t>(height) *
               bytes_per_pixel() * frames;
    }

    bool in_bounds(int x, int y) const noexcept {
        return x >= 0 && y >= 0 &&
               x < static_cast<int>(width) &&
               y < static_cast<int>(height);
    }

    uint8_t* pixel_ptr(int x, int y) noexcept {
        const size_t bpp = bytes_per_pixel();
        const size_t index =
            (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * bpp;
        return pixels.data() + index;
    }

    const uint8_t* pixel_ptr(int x, int y) const noexcept {
        const size_t bpp = bytes_per_pixel();
        const size_t index =
            (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * bpp;
        return pixels.data() + index;
    }
};

// Thread-compatible: concurrent const access is safe; mutating operations
// require external synchronization.
class GraphArchive {
public:
    // CREATORS
    explicit GraphArchive(std::string bin_path);
    GraphArchive(const GraphArchive&) = delete;
    GraphArchive& operator=(const GraphArchive&) = delete;
    GraphArchive(GraphArchive&&) noexcept;
    GraphArchive& operator=(GraphArchive&&) noexcept;
    ~GraphArchive();

    // ACCESSORS
    const std::string& path() const noexcept;
    const ArchiveHeader& header() const noexcept;
    bool contains(const std::string& name) const noexcept;
    const ArchiveEntry* find_entry(const std::string& name) const noexcept;
    std::vector<uint8_t> extract_raw(const std::string& name) const;
    NvsgImage load_image(const std::string& name) const;
    void repack_to(
        const std::string& dst_bin_path,
        const std::unordered_map<std::string, std::vector<uint8_t>>& replacements) const;

private:
    // DATA
    struct Impl;
    std::unique_ptr<Impl> d_impl;
};

ArchiveHeader read_archive_toc(const std::string& bin_path);
std::vector<uint8_t> extract_entry(const std::string& bin_path, const ArchiveEntry& entry);

void repack_archive(
    const std::string& src_bin_path,
    const std::string& dst_bin_path,
    const std::unordered_map<std::string, std::vector<uint8_t>>& replacements);

NvsgImage decode_nvsg(const std::vector<uint8_t>& raw_entry);
std::vector<uint8_t> encode_nvsg(const NvsgImage& img);

}  // namespace fav::fvp
