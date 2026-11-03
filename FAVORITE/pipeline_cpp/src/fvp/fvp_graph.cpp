// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "fvp/fvp_graph.h"

#include "common/util.h"

#include <zlib.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace fav::fvp {

namespace {

constexpr uint32_t kByteMask = 0xFFu;
constexpr unsigned int kOneByteShift = 8u;
constexpr unsigned int kTwoBytesShift = 16u;
constexpr unsigned int kThreeBytesShift = 24u;

constexpr size_t kMagicByteCount = 4;
constexpr char kHzc1Magic[kMagicByteCount] = {'h', 'z', 'c', '1'};
constexpr char kNvsgMagic[kMagicByteCount] = {'N', 'V', 'S', 'G'};

constexpr size_t kArchiveFileCountOffset = 0;
constexpr size_t kArchiveNamePoolSizeOffset = 4;

constexpr size_t kTocNameOffsetField = 0;
constexpr size_t kTocDataOffsetField = 4;
constexpr size_t kTocDataSizeField = 8;

constexpr size_t kHzc1UncompressedSizeOffset = 4;
constexpr size_t kHzc1InnerHeaderSizeOffset = 8;
constexpr size_t kNvsgHeaderOffset = 12;
constexpr size_t kNvsgVersionOffset = 16;
constexpr size_t kNvsgFormatOffset = 18;
constexpr size_t kNvsgWidthOffset = 20;
constexpr size_t kNvsgHeightOffset = 22;
constexpr size_t kNvsgOffsetXOffset = 24;
constexpr size_t kNvsgOffsetYOffset = 26;
constexpr size_t kNvsgUnknown1Offset = 28;
constexpr size_t kNvsgFrameCountOffset = 32;
constexpr size_t kNvsgUnknown2Offset = 36;
constexpr size_t kNvsgUnknown3Offset = 40;

constexpr uint16_t kDefaultNvsgVersion = 0x0100u;
constexpr uint64_t kMaxArchiveOffset = 0xFFFFFFFFull;

uint16_t read_u16_le(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << kOneByteShift);
}

uint32_t read_u32_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << kOneByteShift) |
           (static_cast<uint32_t>(p[2]) << kTwoBytesShift) |
           (static_cast<uint32_t>(p[3]) << kThreeBytesShift);
}

void write_u16_le(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & kByteMask);
    p[1] = static_cast<uint8_t>((v >> kOneByteShift) & kByteMask);
}

void write_u32_le(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & kByteMask);
    p[1] = static_cast<uint8_t>((v >> kOneByteShift) & kByteMask);
    p[2] = static_cast<uint8_t>((v >> kTwoBytesShift) & kByteMask);
    p[3] = static_cast<uint8_t>((v >> kThreeBytesShift) & kByteMask);
}

}  // namespace

struct GraphArchive::Impl {
    std::string d_bin_path;
    ArchiveHeader d_header;
    std::unordered_map<std::string, size_t> d_index_by_name;

    explicit Impl(std::string path)
        : d_bin_path(std::move(path)),
          d_header(read_archive_toc(d_bin_path)) {
        d_index_by_name.reserve(d_header.entries.size());
        for (size_t i = 0; i < d_header.entries.size(); ++i) {
            d_index_by_name.emplace(d_header.entries[i].name, i);
        }
    }
};

GraphArchive::GraphArchive(std::string bin_path)
    : d_impl(std::make_unique<Impl>(std::move(bin_path))) {}

GraphArchive::GraphArchive(GraphArchive&&) noexcept = default;
GraphArchive& GraphArchive::operator=(GraphArchive&&) noexcept = default;
GraphArchive::~GraphArchive() = default;

const std::string& GraphArchive::path() const noexcept {
    return d_impl->d_bin_path;
}

const ArchiveHeader& GraphArchive::header() const noexcept {
    return d_impl->d_header;
}

bool GraphArchive::contains(const std::string& name) const noexcept {
    return find_entry(name) != nullptr;
}

const ArchiveEntry* GraphArchive::find_entry(const std::string& name) const noexcept {
    const auto it = d_impl->d_index_by_name.find(name);
    if (it == d_impl->d_index_by_name.end()) {
        return nullptr;
    }
    return &d_impl->d_header.entries[it->second];
}

std::vector<uint8_t> GraphArchive::extract_raw(const std::string& name) const {
    const ArchiveEntry* entry = find_entry(name);
    if (!entry) {
        throw std::runtime_error("Missing entry in FVP archive (" + d_impl->d_bin_path +
                                 "): " + name);
    }
    return extract_entry(d_impl->d_bin_path, *entry);
}

NvsgImage GraphArchive::load_image(const std::string& name) const {
    return decode_nvsg(extract_raw(name));
}

void GraphArchive::repack_to(
    const std::string& dst_bin_path,
    const std::unordered_map<std::string, std::vector<uint8_t>>& replacements) const {
    const ArchiveHeader& hdr = d_impl->d_header;

    std::ifstream in(fs::u8path(d_impl->d_bin_path), std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot open source archive: " + d_impl->d_bin_path);
    }

    const fs::path dst_path = fs::u8path(dst_bin_path);
    const fs::path tmp_path = fs::u8path(dst_bin_path + ".tmp");

    const uint64_t data_start =
        static_cast<uint64_t>(kArchiveHeaderByteCount) +
        static_cast<uint64_t>(hdr.file_count) * static_cast<uint64_t>(kTocEntryByteCount) +
        static_cast<uint64_t>(hdr.name_pool_size);
    uint64_t running_offset = data_start;

    std::vector<ArchiveEntry> new_entries = hdr.entries;
    for (auto& e : new_entries) {
        if (running_offset > kMaxArchiveOffset) {
            throw std::runtime_error("Archive exceeds 4 GiB offset limit");
        }
        e.data_offset = static_cast<uint32_t>(running_offset);
        const auto it = replacements.find(e.name);
        if (it != replacements.end()) {
            e.data_size = static_cast<uint32_t>(it->second.size());
        }
        running_offset += e.data_size;
    }

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("Cannot open output archive: " + tmp_path.u8string());
        }

        uint8_t hdr_buf[kArchiveHeaderByteCount]{};
        write_u32_le(hdr_buf + kArchiveFileCountOffset, hdr.file_count);
        write_u32_le(hdr_buf + kArchiveNamePoolSizeOffset, hdr.name_pool_size);
        out.write(reinterpret_cast<const char*>(hdr_buf), sizeof(hdr_buf));

        std::vector<uint8_t> toc_buf(
            static_cast<size_t>(hdr.file_count) * kTocEntryByteCount);
        for (uint32_t i = 0; i < hdr.file_count; ++i) {
            uint8_t* p = toc_buf.data() + static_cast<size_t>(i) * kTocEntryByteCount;
            write_u32_le(p + kTocNameOffsetField, new_entries[i].name_offset);
            write_u32_le(p + kTocDataOffsetField, new_entries[i].data_offset);
            write_u32_le(p + kTocDataSizeField, new_entries[i].data_size);
        }
        if (!toc_buf.empty()) {
            out.write(reinterpret_cast<const char*>(toc_buf.data()),
                      static_cast<std::streamsize>(toc_buf.size()));
        }
        if (!hdr.raw_name_pool.empty()) {
            out.write(reinterpret_cast<const char*>(hdr.raw_name_pool.data()),
                      static_cast<std::streamsize>(hdr.raw_name_pool.size()));
        }

        std::vector<uint8_t> copy_buf;
        for (uint32_t i = 0; i < hdr.file_count; ++i) {
            const auto& orig = hdr.entries[i];
            const auto it = replacements.find(orig.name);
            if (it != replacements.end()) {
                if (!it->second.empty()) {
                    out.write(reinterpret_cast<const char*>(it->second.data()),
                              static_cast<std::streamsize>(it->second.size()));
                }
            } else if (orig.data_size > 0) {
                copy_buf.resize(orig.data_size);
                in.seekg(static_cast<std::streamoff>(orig.data_offset), std::ios::beg);
                in.read(reinterpret_cast<char*>(copy_buf.data()),
                        static_cast<std::streamsize>(orig.data_size));
                if (in.gcount() != static_cast<std::streamsize>(orig.data_size)) {
                    throw std::runtime_error("Failed reading original entry " + orig.name);
                }
                out.write(reinterpret_cast<const char*>(copy_buf.data()),
                          static_cast<std::streamsize>(copy_buf.size()));
            }
            if (!out) {
                throw std::runtime_error("Failed writing entry " + orig.name);
            }
        }
        out.flush();
    }
    in.close();

    std::error_code ec;
    fs::rename(tmp_path, dst_path, ec);
    if (!ec) {
        return;
    }
    fs::copy_file(tmp_path, dst_path, fs::copy_options::overwrite_existing, ec);
    fs::remove(tmp_path);
    if (ec) {
        throw std::runtime_error("Failed replacing archive " + dst_bin_path + ": " +
                                 ec.message());
    }
}

ArchiveHeader read_archive_toc(const std::string& bin_path) {
    std::ifstream in(fs::u8path(bin_path), std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot open FVP archive: " + bin_path);
    }

    uint8_t hdr_buf[kArchiveHeaderByteCount]{};
    in.read(reinterpret_cast<char*>(hdr_buf), sizeof(hdr_buf));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(hdr_buf))) {
        throw std::runtime_error("Truncated FVP archive header: " + bin_path);
    }

    ArchiveHeader hdr;
    hdr.file_count = read_u32_le(hdr_buf + kArchiveFileCountOffset);
    hdr.name_pool_size = read_u32_le(hdr_buf + kArchiveNamePoolSizeOffset);

    std::vector<uint8_t> toc_buf(
        static_cast<size_t>(hdr.file_count) * kTocEntryByteCount);
    if (!toc_buf.empty()) {
        in.read(reinterpret_cast<char*>(toc_buf.data()),
                static_cast<std::streamsize>(toc_buf.size()));
        if (in.gcount() != static_cast<std::streamsize>(toc_buf.size())) {
            throw std::runtime_error("Truncated FVP archive TOC: " + bin_path);
        }
    }

    hdr.raw_name_pool.resize(hdr.name_pool_size);
    if (hdr.name_pool_size > 0) {
        in.read(reinterpret_cast<char*>(hdr.raw_name_pool.data()),
                static_cast<std::streamsize>(hdr.name_pool_size));
        if (in.gcount() != static_cast<std::streamsize>(hdr.name_pool_size)) {
            throw std::runtime_error("Truncated FVP archive name pool: " + bin_path);
        }
    }

    hdr.entries.resize(hdr.file_count);
    for (uint32_t i = 0; i < hdr.file_count; ++i) {
        const uint8_t* p = toc_buf.data() + static_cast<size_t>(i) * kTocEntryByteCount;
        ArchiveEntry& e = hdr.entries[i];
        e.name_offset = read_u32_le(p + kTocNameOffsetField);
        e.data_offset = read_u32_le(p + kTocDataOffsetField);
        e.data_size = read_u32_le(p + kTocDataSizeField);

        if (e.name_offset >= hdr.raw_name_pool.size()) {
            throw std::runtime_error("Invalid name_offset in FVP archive: " + bin_path);
        }
        const char* raw_str =
            reinterpret_cast<const char*>(hdr.raw_name_pool.data() + e.name_offset);
        const size_t max_len = hdr.raw_name_pool.size() - e.name_offset;
        const size_t len = strnlen(raw_str, max_len);
        e.name = cp932_to_utf8_replace(
            reinterpret_cast<const uint8_t*>(raw_str), len);
    }

    return hdr;
}

std::vector<uint8_t> extract_entry(const std::string& bin_path, const ArchiveEntry& entry) {
    std::ifstream in(fs::u8path(bin_path), std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot open FVP archive: " + bin_path);
    }
    in.seekg(static_cast<std::streamoff>(entry.data_offset), std::ios::beg);
    if (!in) {
        throw std::runtime_error("Seek failed for entry " + entry.name);
    }
    std::vector<uint8_t> buf(entry.data_size);
    if (entry.data_size > 0) {
        in.read(reinterpret_cast<char*>(buf.data()),
                static_cast<std::streamsize>(entry.data_size));
        if (in.gcount() != static_cast<std::streamsize>(entry.data_size)) {
            throw std::runtime_error("Read failed for entry " + entry.name);
        }
    }
    return buf;
}

void repack_archive(
    const std::string& src_bin_path,
    const std::string& dst_bin_path,
    const std::unordered_map<std::string, std::vector<uint8_t>>& replacements) {
    const GraphArchive archive(src_bin_path);
    archive.repack_to(dst_bin_path, replacements);
}

NvsgImage decode_nvsg(const std::vector<uint8_t>& raw_entry) {
    if (raw_entry.size() < kCompressedStreamHeaderByteCount) {
        throw std::runtime_error("hzc1/NVSG buffer too small (< 44 bytes)");
    }
    if (std::memcmp(raw_entry.data(), kHzc1Magic, kMagicByteCount) != 0) {
        throw std::runtime_error("Invalid magic: expected 'hzc1'");
    }
    const uint32_t uncompressed_size =
        read_u32_le(raw_entry.data() + kHzc1UncompressedSizeOffset);
    const uint32_t nvsg_hdr_len =
        read_u32_le(raw_entry.data() + kHzc1InnerHeaderSizeOffset);
    if (nvsg_hdr_len != kNvsgHeaderByteCount ||
        std::memcmp(raw_entry.data() + kNvsgHeaderOffset, kNvsgMagic, kMagicByteCount) != 0) {
        throw std::runtime_error("Invalid inner magic: expected 32-byte 'NVSG' header");
    }

    NvsgImage img;
    std::memcpy(img.raw_header.data(),
                raw_entry.data() + kNvsgHeaderOffset,
                kNvsgHeaderByteCount);
    img.has_raw_header = true;

    img.format = read_u16_le(raw_entry.data() + kNvsgFormatOffset);
    img.width = read_u16_le(raw_entry.data() + kNvsgWidthOffset);
    img.height = read_u16_le(raw_entry.data() + kNvsgHeightOffset);
    img.offset_x = read_u16_le(raw_entry.data() + kNvsgOffsetXOffset);
    img.offset_y = read_u16_le(raw_entry.data() + kNvsgOffsetYOffset);
    img.unknown1 = read_u16_le(raw_entry.data() + kNvsgUnknown1Offset);
    img.frame_count = read_u32_le(raw_entry.data() + kNvsgFrameCountOffset);
    img.unknown2 = read_u32_le(raw_entry.data() + kNvsgUnknown2Offset);
    img.unknown3 = read_u32_le(raw_entry.data() + kNvsgUnknown3Offset);

    if (img.bytes_per_pixel() == 0) {
        throw std::runtime_error("Unsupported NVSG pixel format: " +
                                 std::to_string(img.format));
    }
    const size_t expected_size = img.expected_byte_size();
    if (uncompressed_size != expected_size) {
        throw std::runtime_error("NVSG uncompressed size mismatch");
    }

    img.pixels.resize(uncompressed_size);
    uLongf dest_len = static_cast<uLongf>(uncompressed_size);
    const int zrc = uncompress(
        img.pixels.data(), &dest_len,
        raw_entry.data() + kCompressedStreamHeaderByteCount,
        static_cast<uLong>(raw_entry.size() - kCompressedStreamHeaderByteCount));
    if (zrc != Z_OK || dest_len != static_cast<uLongf>(uncompressed_size)) {
        throw std::runtime_error("zlib uncompress failed on NVSG stream");
    }

    return img;
}

std::vector<uint8_t> encode_nvsg(const NvsgImage& img) {
    if (img.bytes_per_pixel() == 0) {
        throw std::runtime_error("Unsupported NVSG pixel format in encode_nvsg");
    }
    const size_t expected_size = img.expected_byte_size();
    if (img.pixels.size() != expected_size) {
        throw std::runtime_error("NvsgImage pixel buffer size does not match dimensions");
    }

    const uLongf comp_bound = compressBound(static_cast<uLong>(img.pixels.size()));
    std::vector<uint8_t> out(kCompressedStreamHeaderByteCount + comp_bound);

    std::memcpy(out.data(), kHzc1Magic, kMagicByteCount);
    write_u32_le(out.data() + kHzc1UncompressedSizeOffset,
                 static_cast<uint32_t>(img.pixels.size()));
    write_u32_le(out.data() + kHzc1InnerHeaderSizeOffset,
                 static_cast<uint32_t>(kNvsgHeaderByteCount));

    if (img.has_raw_header) {
        std::memcpy(out.data() + kNvsgHeaderOffset,
                    img.raw_header.data(),
                    kNvsgHeaderByteCount);
    } else {
        std::memset(out.data() + kNvsgHeaderOffset, 0, kNvsgHeaderByteCount);
        std::memcpy(out.data() + kNvsgHeaderOffset, kNvsgMagic, kMagicByteCount);
        write_u16_le(out.data() + kNvsgVersionOffset, kDefaultNvsgVersion);
        write_u16_le(out.data() + kNvsgUnknown1Offset, img.unknown1);
        write_u32_le(out.data() + kNvsgFrameCountOffset, img.frame_count);
        write_u32_le(out.data() + kNvsgUnknown2Offset, img.unknown2);
        write_u32_le(out.data() + kNvsgUnknown3Offset, img.unknown3);
    }
    write_u16_le(out.data() + kNvsgFormatOffset, img.format);
    write_u16_le(out.data() + kNvsgWidthOffset, img.width);
    write_u16_le(out.data() + kNvsgHeightOffset, img.height);
    write_u16_le(out.data() + kNvsgOffsetXOffset, img.offset_x);
    write_u16_le(out.data() + kNvsgOffsetYOffset, img.offset_y);

    uLongf actual_comp = comp_bound;
    const int zrc = compress2(
        out.data() + kCompressedStreamHeaderByteCount, &actual_comp,
        img.pixels.data(), static_cast<uLong>(img.pixels.size()),
        Z_BEST_COMPRESSION);
    if (zrc != Z_OK) {
        throw std::runtime_error("zlib compress2 failed in encode_nvsg");
    }
    out.resize(kCompressedStreamHeaderByteCount + actual_comp);
    return out;
}

}  // namespace fav::fvp
