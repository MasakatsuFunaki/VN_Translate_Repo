// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "gpk_archive.h"

#include "cg/image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace mgi::gpk {

namespace {

constexpr std::size_t kTypeFieldBytes = 16u;
constexpr std::size_t kMetaFieldOffset = 0x10u;
constexpr std::size_t kMetaFieldBytes = 16u;
constexpr std::size_t kCropTopOffset = 0x10u;
constexpr std::size_t kCropLeftOffset = 0x14u;
constexpr std::size_t kCropRightOffset = 0x18u;
constexpr std::size_t kCropBottomOffset = 0x1Cu;
constexpr std::size_t kPackedSizeOffset = 0x20u;
constexpr std::size_t kWidthOffset = 0x24u;
constexpr std::size_t kHeightOffset = 0x28u;
constexpr std::size_t kBppOffset = 0x2Au;
constexpr std::size_t kPadOffset = 0x2Cu;
constexpr std::size_t kPadBytes = 4u;
constexpr std::size_t kPacktypeOffset = 0x30u;
constexpr std::size_t kPacktypeBytes = 16u;
constexpr std::size_t kPacktypeCharIndex = 9u;
constexpr std::size_t kPacktypeAlphaIndex = 10u;

constexpr char kPackTypeBmpWithMask = '2';
constexpr char kPackTypeIndexedRle = '3';
constexpr char kAlphaFlagChar = 'A';

constexpr std::size_t kBmpFileHeaderSize = 14u;
constexpr std::size_t kBmpInfoHeaderSize = 40u;
constexpr std::size_t kBmpHeaderSize = kBmpFileHeaderSize + kBmpInfoHeaderSize;
constexpr std::size_t kMaskPaletteColorCount = 256u;
constexpr std::size_t kMaskPaletteEntrySize = 4u;
constexpr std::size_t kMaskPaletteSize = kMaskPaletteColorCount * kMaskPaletteEntrySize;
constexpr std::size_t kMaskHeaderTotalSize = kBmpHeaderSize + kMaskPaletteSize;

constexpr std::size_t kBmpFileSizeOffset = 2u;
constexpr std::size_t kBmpOffBitsOffset = 10u;
constexpr std::size_t kBmpWidthOffset = 18u;
constexpr std::size_t kBmpHeightOffset = 22u;
constexpr std::size_t kBmpBitCountOffset = 28u;
constexpr std::size_t kBmpClrUsedOffset = 46u;
constexpr std::size_t kBmpClrImportantOffset = 50u;
constexpr std::uint16_t kBmp24BitCount = 24u;
constexpr std::uint16_t kBmp8BitCount = 8u;
constexpr std::size_t kBmpRowAlignment = 4u;
constexpr std::size_t kMaxRleZeroRun = 255u;

constexpr int kRgbChannels = 3;
constexpr int kRgbaChannels = 4;
constexpr std::size_t kRedIndex = 0u;
constexpr std::size_t kGreenIndex = 1u;
constexpr std::size_t kBlueIndex = 2u;
constexpr std::size_t kAlphaIndex = 3u;
constexpr std::size_t kBgrBlueIndex = 0u;
constexpr std::size_t kBgrGreenIndex = 1u;
constexpr std::size_t kBgrRedIndex = 2u;
constexpr std::uint8_t kOpaqueAlpha = 255u;
constexpr std::uint8_t kTransparentAlpha = 0u;

constexpr std::size_t kU32Bytes = 4u;
constexpr std::size_t kU64Bytes = 8u;
constexpr std::size_t kGtbAlignBytes = 8u;
constexpr std::size_t kOver2GZeroPadBytes = 8u;
constexpr std::uint64_t kLowU32Mask = 0xFFFFFFFFull;
constexpr std::uint32_t kMaxU16Value = 0xFFFFu;
constexpr unsigned int kByteShift1 = 8u;
constexpr unsigned int kByteShift2 = 16u;
constexpr unsigned int kByteShift3 = 24u;

std::uint16_t rd_u16le(const std::uint8_t* b, std::size_t sz, std::size_t off) {
    if (off > sz || sz - off < 2u) {
        throw std::runtime_error("gtb/gpk: read past end of buffer");
    }
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(b[off]) |
        (static_cast<std::uint16_t>(b[off + 1u]) << kByteShift1));
}

std::uint32_t rd_u32le(const std::uint8_t* b, std::size_t sz, std::size_t off) {
    if (off > sz || sz - off < kU32Bytes) {
        throw std::runtime_error("gtb/gpk: read past end of buffer");
    }
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1u]) << kByteShift1) |
           (static_cast<std::uint32_t>(b[off + 2u]) << kByteShift2) |
           (static_cast<std::uint32_t>(b[off + 3u]) << kByteShift3);
}

std::uint32_t rd_u32le(const Bytes& b, std::size_t off) {
    return rd_u32le(b.data(), b.size(), off);
}

std::int32_t rd_i32le(const std::uint8_t* b, std::size_t sz, std::size_t off) {
    return static_cast<std::int32_t>(rd_u32le(b, sz, off));
}

std::uint64_t rd_u64le(const Bytes& b, std::size_t off) {
    if (off > b.size() || b.size() - off < kU64Bytes) {
        throw std::runtime_error("gtb/gpk: read past end of buffer");
    }
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < kU64Bytes; ++i) {
        v |= static_cast<std::uint64_t>(b[off + i]) << (kByteShift1 * i);
    }
    return v;
}

void wr_u16le(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> kByteShift1);
}

void wr_u32le(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> kByteShift1);
    p[2] = static_cast<std::uint8_t>(v >> kByteShift2);
    p[3] = static_cast<std::uint8_t>(v >> kByteShift3);
}

void wr_i32le(std::uint8_t* p, std::int32_t v) noexcept {
    wr_u32le(p, static_cast<std::uint32_t>(v));
}

void put_u32le(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> kByteShift1));
    out.push_back(static_cast<std::uint8_t>(v >> kByteShift2));
    out.push_back(static_cast<std::uint8_t>(v >> kByteShift3));
}

void put_u64le(Bytes& out, std::uint64_t v) {
    for (std::size_t i = 0; i < kU64Bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (kByteShift1 * i)));
    }
}

bool starts_with(const Bytes& d, const std::uint8_t* magic, std::size_t n) {
    return d.size() >= n && std::memcmp(d.data(), magic, n) == 0;
}

std::string hex_lower(std::size_t v) {
    static const char* const kDigits = "0123456789abcdef";
    if (v == 0) {
        return "0";
    }
    std::string s;
    while (v != 0) {
        s += kDigits[v & 0xFu];
        v >>= 4u;
    }
    return std::string(s.rbegin(), s.rend());
}

std::size_t bmp_row_stride_24(int width) noexcept {
    const std::size_t raw_row = static_cast<std::size_t>(width) * kRgbChannels;
    return (raw_row + (kBmpRowAlignment - 1u)) & ~(kBmpRowAlignment - 1u);
}

bool has_non_zero_type_raw(const std::array<std::uint8_t, kTypeFieldBytes>& raw) noexcept {
    for (std::uint8_t b : raw) {
        if (b != 0u) {
            return true;
        }
    }
    return false;
}

EntryHeader read_entry_header_ptr(const std::uint8_t* buf,
                                  std::size_t buf_size,
                                  std::size_t offset) {
    if (offset > buf_size || buf_size - offset < GPK_HEADER_SIZE) {
        throw std::runtime_error("truncated entry header at offset 0x" + hex_lower(offset));
    }
    const std::uint8_t* hdr = buf + offset;

    EntryHeader h;
    std::memcpy(h.type_raw.data(), hdr, kTypeFieldBytes);
    std::memcpy(h.meta.data(), hdr + kMetaFieldOffset, kMetaFieldBytes);
    h.packed_size = rd_u32le(buf, buf_size, offset + kPackedSizeOffset);
    h.width = rd_u32le(buf, buf_size, offset + kWidthOffset);
    h.height = rd_u16le(buf, buf_size, offset + kHeightOffset);
    h.bpp = rd_u16le(buf, buf_size, offset + kBppOffset);
    std::memcpy(h.pad.data(), hdr + kPadOffset, kPadBytes);
    std::memcpy(h.packtype.data(), hdr + kPacktypeOffset, kPacktypeBytes);

    // Strip trailing 0x20 AND 0x00 in any interleaving (shipped tags mix both),
    // then ASCII-decode with every byte >= 0x80 mapped to U+FFFD.
    std::size_t n = kTypeFieldBytes;
    while (n > 0 && (h.type_raw[n - 1u] == 0x20u || h.type_raw[n - 1u] == 0x00u)) {
        --n;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (h.type_raw[i] < 0x80u) {
            h.type_tag += static_cast<char>(h.type_raw[i]);
        } else {
            h.type_tag += "\xEF\xBF\xBD";  // U+FFFD
        }
    }
    return h;
}

Bytes decode_mask_section_alpha(const std::uint8_t* mask_hdr,
                                std::size_t avail_bytes,
                                int width,
                                int height,
                                std::uint32_t* out_mask_bf_off = nullptr) {
    if (avail_bytes < kBmpHeaderSize) {
        throw std::runtime_error("decode_dwq_masked_rgba: truncated MASK header");
    }
    const std::uint32_t mask_bf_off = rd_u32le(mask_hdr, avail_bytes, kBmpOffBitsOffset);
    if (mask_bf_off < kBmpHeaderSize || avail_bytes < mask_bf_off ||
        (mask_bf_off - kBmpHeaderSize) % kMaskPaletteEntrySize != 0u) {
        throw std::runtime_error("decode_dwq_masked_rgba: invalid MASK bfOffBits");
    }
    const std::size_t pal_entries = (mask_bf_off - kBmpHeaderSize) / kMaskPaletteEntrySize;
    if (pal_entries == 0u || pal_entries > kMaskPaletteColorCount) {
        throw std::runtime_error("decode_dwq_masked_rgba: invalid MASK palette size");
    }
    if (out_mask_bf_off != nullptr) {
        *out_mask_bf_off = mask_bf_off;
    }

    const std::uint8_t* mask_palette = mask_hdr + kBmpHeaderSize;
    const Bytes alpha_indices = decode_rle_xor_mask(
        mask_hdr + mask_bf_off, avail_bytes - mask_bf_off, width, height);

    const std::size_t total_px =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    Bytes alpha(total_px, 0u);
    for (std::size_t i = 0; i < total_px; ++i) {
        const std::size_t idx = std::min<std::size_t>(alpha_indices[i], pal_entries - 1u);
        alpha[i] = mask_palette[idx * kMaskPaletteEntrySize + kBgrRedIndex];
    }
    return alpha;
}

int read_bmp_abs_height(const std::uint8_t* bmp_hdr, std::size_t bmp_avail) {
    const std::int64_t raw_h = rd_i32le(bmp_hdr, bmp_avail, kBmpHeightOffset);
    const std::int64_t abs_h = (raw_h < 0) ? -raw_h : raw_h;
    if (abs_h <= 0 || abs_h > std::numeric_limits<int>::max()) {
        throw std::runtime_error("DWQ BMP height out of range");
    }
    return static_cast<int>(abs_h);
}

void append_identity_mask_bmp(Bytes& out,
                              const std::uint8_t* orig_mask_hdr,
                              std::size_t orig_mask_avail,
                              std::uint32_t orig_mask_bf_off,
                              const Bytes& new_alpha,
                              int width,
                              int height) {
    const std::size_t mask_hdr_pos = out.size();
    out.insert(out.end(), orig_mask_hdr, orig_mask_hdr + kBmpHeaderSize);

    const std::size_t total_px =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::uint32_t orig_bf_size = rd_u32le(orig_mask_hdr, orig_mask_avail, kBmpFileSizeOffset);
    const std::uint64_t expanded_bf_size =
        (orig_bf_size >= orig_mask_bf_off)
            ? (static_cast<std::uint64_t>(orig_bf_size) -
               static_cast<std::uint64_t>(orig_mask_bf_off) +
               static_cast<std::uint64_t>(kMaskHeaderTotalSize))
            : (static_cast<std::uint64_t>(kMaskHeaderTotalSize) +
               static_cast<std::uint64_t>(total_px));
    const std::uint32_t new_bf_size =
        (expanded_bf_size <= std::numeric_limits<std::uint32_t>::max())
            ? static_cast<std::uint32_t>(expanded_bf_size)
            : static_cast<std::uint32_t>(kMaskHeaderTotalSize + total_px);

    wr_u32le(out.data() + mask_hdr_pos + kBmpFileSizeOffset, new_bf_size);
    wr_u32le(out.data() + mask_hdr_pos + kBmpOffBitsOffset,
             static_cast<std::uint32_t>(kMaskHeaderTotalSize));
    wr_u32le(out.data() + mask_hdr_pos + kBmpClrUsedOffset,
             static_cast<std::uint32_t>(kMaskPaletteColorCount));
    wr_u32le(out.data() + mask_hdr_pos + kBmpClrImportantOffset, 0u);

    for (std::size_t i = 0; i < kMaskPaletteColorCount; ++i) {
        const auto gray = static_cast<std::uint8_t>(i);
        out.push_back(gray);
        out.push_back(gray);
        out.push_back(gray);
        out.push_back(0u);
    }

    const Bytes encoded_mask = encode_rle_xor_mask(new_alpha.data(), width, height);
    out.insert(out.end(), encoded_mask.begin(), encoded_mask.end());
}

void write_crop_margins(std::uint8_t* dwq_hdr, const DwqCropMargins& margins) noexcept {
    wr_i32le(dwq_hdr + kCropTopOffset, margins.top);
    wr_i32le(dwq_hdr + kCropLeftOffset, margins.left);
    wr_i32le(dwq_hdr + kCropRightOffset, margins.right);
    wr_i32le(dwq_hdr + kCropBottomOffset, margins.bottom);
}

std::map<std::uint64_t, std::uint64_t> build_next_offset_map(const Archive& arc) {
    std::vector<std::uint64_t> sorted_offs;
    sorted_offs.reserve(arc.entries.size());
    for (const auto& e : arc.entries) {
        sorted_offs.push_back(e.offset);
    }
    std::sort(sorted_offs.begin(), sorted_offs.end());

    // Pair each sorted offset with the next one.  Duplicate keys collapse to
    // the LAST pairing, so shared-payload entries size correctly.
    std::map<std::uint64_t, std::uint64_t> next_off;
    for (std::size_t i = 0; i + 1u < sorted_offs.size(); ++i) {
        next_off[sorted_offs[i]] = sorted_offs[i + 1u];
    }
    if (!sorted_offs.empty()) {
        next_off[sorted_offs.back()] = static_cast<std::uint64_t>(arc.gpk_size);
    }
    return next_off;
}

Bytes encode_masked_2a_entry(const std::uint8_t* orig_raw,
                             std::size_t orig_raw_size,
                             const EntryHeader& orig,
                             const cg::Image& img) {
    const std::uint8_t* bmp_hdr = orig_raw + GPK_HEADER_SIZE;
    const std::size_t bmp_avail = orig_raw_size - GPK_HEADER_SIZE;
    const int bm_w = static_cast<int>(rd_i32le(bmp_hdr, bmp_avail, kBmpWidthOffset));
    const int bm_h = read_bmp_abs_height(bmp_hdr, bmp_avail);
    const std::uint16_t bit_count = rd_u16le(bmp_hdr, bmp_avail, kBmpBitCountOffset);
    const std::uint32_t color_bf_off = rd_u32le(bmp_hdr, bmp_avail, kBmpOffBitsOffset);

    if (bit_count != kBmp24BitCount || bm_w <= 0 || bm_w != img.width || bm_h != img.height ||
        color_bf_off < kBmpHeaderSize) {
        throw std::runtime_error("build_entry_from_raw: PACKTYPE=2A header/image mismatch");
    }

    const std::size_t stride = bmp_row_stride_24(bm_w);
    const std::size_t bmp_section_size = orig.packed_size;
    if (orig_raw_size < GPK_HEADER_SIZE + kBmpHeaderSize ||
        bmp_section_size > orig_raw_size - GPK_HEADER_SIZE - kBmpHeaderSize ||
        bmp_section_size < static_cast<std::size_t>(color_bf_off) +
                               stride * static_cast<std::size_t>(bm_h)) {
        throw std::runtime_error("build_entry_from_raw: corrupt PACKTYPE=2A section sizes");
    }

    const std::size_t mask_offset = GPK_HEADER_SIZE + bmp_section_size;
    const std::size_t mask_avail = orig_raw_size - mask_offset;
    std::uint32_t orig_mask_bf_off = static_cast<std::uint32_t>(kMaskHeaderTotalSize);
    const Bytes orig_alpha = decode_mask_section_alpha(
        orig_raw + mask_offset, mask_avail, bm_w, bm_h, &orig_mask_bf_off);

    Bytes out(orig_raw, orig_raw + GPK_HEADER_SIZE + bmp_section_size);
    const std::size_t total_px =
        static_cast<std::size_t>(bm_w) * static_cast<std::size_t>(bm_h);
    const bool has_new_alpha = (img.channels == kRgbaChannels);

    std::uint8_t* dst_bmp_pixels = out.data() + GPK_HEADER_SIZE + color_bf_off;
    const std::uint8_t* orig_bmp_pixels = orig_raw + GPK_HEADER_SIZE + color_bf_off;
    for (int y = 0; y < bm_h; ++y) {
        std::uint8_t* dst_row = dst_bmp_pixels + static_cast<std::size_t>(y) * stride;
        const std::uint8_t* orig_row = orig_bmp_pixels + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < bm_w; ++x) {
            const std::uint8_t* src = img.at(x, y);
            const std::uint8_t r = src[kRedIndex];
            const std::uint8_t g = src[kGreenIndex];
            const std::uint8_t b = src[kBlueIndex];
            const std::size_t px_idx = static_cast<std::size_t>(y) * bm_w + x;
            const std::uint8_t a = has_new_alpha ? src[kAlphaIndex] : orig_alpha[px_idx];

            if (a == kTransparentAlpha && orig_alpha[px_idx] == kTransparentAlpha &&
                r == 0u && g == 0u && b == 0u) {
                dst_row[x * kRgbChannels + kBgrBlueIndex] =
                    orig_row[x * kRgbChannels + kBgrBlueIndex];
                dst_row[x * kRgbChannels + kBgrGreenIndex] =
                    orig_row[x * kRgbChannels + kBgrGreenIndex];
                dst_row[x * kRgbChannels + kBgrRedIndex] =
                    orig_row[x * kRgbChannels + kBgrRedIndex];
                continue;
            }
            dst_row[x * kRgbChannels + kBgrBlueIndex] = b;
            dst_row[x * kRgbChannels + kBgrGreenIndex] = g;
            dst_row[x * kRgbChannels + kBgrRedIndex] = r;
        }
    }

    if (has_new_alpha) {
        if (has_alpha_flag(orig)) {
            DwqRgbaImage rgba_view{img.width, img.height, img.px};
            write_crop_margins(out.data(), compute_dwq_crop_margins(rgba_view));
        }
        Bytes new_alpha(total_px, 0u);
        for (std::size_t i = 0; i < total_px; ++i) {
            new_alpha[i] = img.px[i * kRgbaChannels + kAlphaIndex];
        }
        append_identity_mask_bmp(
            out, orig_raw + mask_offset, mask_avail, orig_mask_bf_off, new_alpha, bm_w, bm_h);
    } else {
        // Replacement carried RGB only: preserve the original alpha mask verbatim.
        out.insert(out.end(), orig_raw + mask_offset, orig_raw + orig_raw_size);
    }
    return out;
}

Bytes encode_masked_3a_entry(const std::uint8_t* orig_raw,
                             std::size_t orig_raw_size,
                             const EntryHeader& orig,
                             const cg::Image& img) {
    const std::uint8_t* bmp_hdr = orig_raw + GPK_HEADER_SIZE;
    const std::size_t bmp_avail = orig_raw_size - GPK_HEADER_SIZE;
    const int bm_w = static_cast<int>(rd_i32le(bmp_hdr, bmp_avail, kBmpWidthOffset));
    const int bm_h = read_bmp_abs_height(bmp_hdr, bmp_avail);
    const std::uint16_t bit_count = rd_u16le(bmp_hdr, bmp_avail, kBmpBitCountOffset);
    const std::uint32_t color_bf_off = rd_u32le(bmp_hdr, bmp_avail, kBmpOffBitsOffset);

    if (bit_count != kBmp8BitCount || bm_w <= 0 || bm_w != img.width || bm_h != img.height ||
        color_bf_off < kBmpHeaderSize ||
        (color_bf_off - kBmpHeaderSize) % kMaskPaletteEntrySize != 0u) {
        throw std::runtime_error("build_entry_from_raw: PACKTYPE=3A header/image mismatch");
    }

    const std::size_t orig_data_size = orig.packed_size;
    if (orig_raw_size < GPK_HEADER_SIZE + kBmpHeaderSize ||
        orig_data_size <= color_bf_off ||
        orig_data_size > orig_raw_size - GPK_HEADER_SIZE - kBmpHeaderSize) {
        throw std::runtime_error("build_entry_from_raw: corrupt PACKTYPE=3A section sizes");
    }

    const std::size_t orig_pal_entries =
        (color_bf_off - kBmpHeaderSize) / kMaskPaletteEntrySize;
    if (orig_pal_entries == 0u || orig_pal_entries > kMaskPaletteColorCount) {
        throw std::runtime_error("build_entry_from_raw: invalid PACKTYPE=3A color palette");
    }
    const std::uint8_t* orig_color_pal = bmp_hdr + kBmpHeaderSize;
    const Bytes orig_color_indices = decode_rle_xor_mask(
        bmp_hdr + color_bf_off, orig_data_size - color_bf_off, bm_w, bm_h);

    const std::size_t mask_offset = GPK_HEADER_SIZE + orig_data_size;
    const std::size_t mask_avail = orig_raw_size - mask_offset;
    std::uint32_t orig_mask_bf_off = static_cast<std::uint32_t>(kMaskHeaderTotalSize);
    const Bytes orig_alpha = decode_mask_section_alpha(
        orig_raw + mask_offset, mask_avail, bm_w, bm_h, &orig_mask_bf_off);

    std::array<std::uint8_t, kMaskPaletteSize> new_pal{};
    std::memcpy(new_pal.data(), orig_color_pal, orig_pal_entries * kMaskPaletteEntrySize);
    std::size_t pal_count = orig_pal_entries;

    const auto find_exact = [&](std::uint8_t r, std::uint8_t g, std::uint8_t b) -> int {
        for (std::size_t p = 0; p < pal_count; ++p) {
            if (new_pal[p * kMaskPaletteEntrySize + kBgrBlueIndex] == b &&
                new_pal[p * kMaskPaletteEntrySize + kBgrGreenIndex] == g &&
                new_pal[p * kMaskPaletteEntrySize + kBgrRedIndex] == r) {
                return static_cast<int>(p);
            }
        }
        return -1;
    };

    const auto find_nearest = [&](std::uint8_t r, std::uint8_t g, std::uint8_t b) -> std::uint8_t {
        std::size_t best_idx = 0u;
        int best_dist = std::numeric_limits<int>::max();
        const std::size_t limit = pal_count > 0u ? pal_count : 1u;
        for (std::size_t p = 0; p < limit; ++p) {
            const int db = static_cast<int>(b) -
                           static_cast<int>(new_pal[p * kMaskPaletteEntrySize + kBgrBlueIndex]);
            const int dg = static_cast<int>(g) -
                           static_cast<int>(new_pal[p * kMaskPaletteEntrySize + kBgrGreenIndex]);
            const int dr = static_cast<int>(r) -
                           static_cast<int>(new_pal[p * kMaskPaletteEntrySize + kBgrRedIndex]);
            const int dist = dr * dr + dg * dg + db * db;
            if (dist < best_dist) {
                best_dist = dist;
                best_idx = p;
                if (dist == 0) {
                    break;
                }
            }
        }
        return static_cast<std::uint8_t>(best_idx);
    };

    const std::size_t total_px =
        static_cast<std::size_t>(bm_w) * static_cast<std::size_t>(bm_h);
    const bool has_new_alpha = (img.channels == kRgbaChannels);
    Bytes new_color_indices(total_px, 0u);
    Bytes new_alpha(total_px, 0u);

    for (std::size_t i = 0; i < total_px; ++i) {
        const std::uint8_t* src = img.px.data() + i * static_cast<std::size_t>(img.channels);
        const std::uint8_t r = src[kRedIndex];
        const std::uint8_t g = src[kGreenIndex];
        const std::uint8_t b = src[kBlueIndex];
        const std::uint8_t a = has_new_alpha ? src[kAlphaIndex] : orig_alpha[i];
        new_alpha[i] = a;

        const std::uint8_t orig_idx = orig_color_indices[i];
        if (a == kTransparentAlpha && orig_alpha[i] == kTransparentAlpha &&
            orig_idx < pal_count && r == 0u && g == 0u && b == 0u) {
            new_color_indices[i] = orig_idx;
            continue;
        }
        if (orig_idx < pal_count &&
            new_pal[orig_idx * kMaskPaletteEntrySize + kBgrRedIndex] == r &&
            new_pal[orig_idx * kMaskPaletteEntrySize + kBgrGreenIndex] == g &&
            new_pal[orig_idx * kMaskPaletteEntrySize + kBgrBlueIndex] == b) {
            new_color_indices[i] = orig_idx;
            continue;
        }

        const int exact = find_exact(r, g, b);
        if (exact >= 0) {
            new_color_indices[i] = static_cast<std::uint8_t>(exact);
        } else if (pal_count < kMaskPaletteColorCount) {
            const std::size_t idx = pal_count++;
            new_pal[idx * kMaskPaletteEntrySize + kBgrBlueIndex] = b;
            new_pal[idx * kMaskPaletteEntrySize + kBgrGreenIndex] = g;
            new_pal[idx * kMaskPaletteEntrySize + kBgrRedIndex] = r;
            new_pal[idx * kMaskPaletteEntrySize + kAlphaIndex] = 0u;
            new_color_indices[i] = static_cast<std::uint8_t>(idx);
        } else {
            new_color_indices[i] = find_nearest(r, g, b);
        }
    }

    const Bytes encoded_color = encode_rle_xor_mask(new_color_indices.data(), bm_w, bm_h);
    const std::size_t color_pal_bytes = pal_count * kMaskPaletteEntrySize;
    const std::uint32_t new_color_bf_off =
        static_cast<std::uint32_t>(kBmpHeaderSize + color_pal_bytes);
    const std::uint32_t new_data_size =
        new_color_bf_off + static_cast<std::uint32_t>(encoded_color.size());

    Bytes out(orig_raw, orig_raw + GPK_HEADER_SIZE);
    wr_u32le(out.data() + kPackedSizeOffset, new_data_size);
    if (has_new_alpha && has_alpha_flag(orig)) {
        DwqRgbaImage rgba_view{img.width, img.height, img.px};
        write_crop_margins(out.data(), compute_dwq_crop_margins(rgba_view));
    }

    Bytes color_hdr(bmp_hdr, bmp_hdr + kBmpHeaderSize);
    const std::uint32_t orig_color_bf_size = rd_u32le(bmp_hdr, bmp_avail, kBmpFileSizeOffset);
    const std::uint32_t new_color_bf_size =
        (orig_color_bf_size >= color_bf_off)
            ? static_cast<std::uint32_t>(orig_color_bf_size - color_bf_off + new_color_bf_off)
            : new_data_size;
    wr_u32le(color_hdr.data() + kBmpFileSizeOffset, new_color_bf_size);
    wr_u32le(color_hdr.data() + kBmpOffBitsOffset, new_color_bf_off);
    wr_u32le(color_hdr.data() + kBmpClrUsedOffset, static_cast<std::uint32_t>(pal_count));
    wr_u32le(color_hdr.data() + kBmpClrImportantOffset, 0u);
    out.insert(out.end(), color_hdr.begin(), color_hdr.end());
    out.insert(out.end(), new_pal.begin(),
               new_pal.begin() + static_cast<std::ptrdiff_t>(color_pal_bytes));
    out.insert(out.end(), encoded_color.begin(), encoded_color.end());

    if (has_new_alpha) {
        append_identity_mask_bmp(
            out, orig_raw + mask_offset, mask_avail, orig_mask_bf_off, new_alpha, bm_w, bm_h);
    } else {
        out.insert(out.end(), orig_raw + mask_offset, orig_raw + orig_raw_size);
    }
    return out;
}

}  // namespace

const std::array<std::uint8_t, 16> PACKTYPE_DEFAULT = {
    'P', 'A', 'C', 'K', 'T', 'Y', 'P', 'E', '=', '8', 'A', ' ', ' ', ' ', ' ', ' '};
const std::array<std::uint8_t, 8> TRAILER_MARKER = {'o', 'v', 'e', 'r',
                                                    '2', 'G', '!', 0x00};

char packtype_char(const EntryHeader& h) noexcept {
    return static_cast<char>(h.packtype[kPacktypeCharIndex]);
}

bool has_alpha_flag(const EntryHeader& h) noexcept {
    return static_cast<char>(h.packtype[kPacktypeAlphaIndex]) == kAlphaFlagChar;
}

bool is_masked_bmp_packtype(const EntryHeader& h) noexcept {
    const char pt = packtype_char(h);
    return pt == kPackTypeBmpWithMask || pt == kPackTypeIndexedRle;
}

// GTB

Gtb read_gtb(const std::string& gtb_path) {
    const Bytes buf = read_file(gtb_path);

    Gtb gtb;
    gtb.count = rd_u32le(buf, 0u);
    const std::size_t count = gtb.count;

    const std::size_t name_off_base = kU32Bytes;
    if (buf.size() < name_off_base ||
        count > (buf.size() - name_off_base) / (kU32Bytes * 2u)) {
        throw std::runtime_error(gtb_path + ": truncated GTB offset tables");
    }
    const std::size_t data_off_base = name_off_base + count * kU32Bytes;
    const std::size_t name_table_base = data_off_base + count * kU32Bytes;

    gtb.entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t name_off = rd_u32le(buf, name_off_base + i * kU32Bytes);
        const std::uint32_t data_off = rd_u32le(buf, data_off_base + i * kU32Bytes);

        if (name_off >= buf.size() - name_table_base) {
            throw std::runtime_error(gtb_path + ": name offset out of range at entry " +
                                     std::to_string(i));
        }
        const std::size_t start = name_table_base + name_off;
        std::size_t end = std::string::npos;
        for (std::size_t j = start; j < buf.size(); ++j) {
            if (buf[j] == 0x00u) {
                end = j;
                break;
            }
        }
        if (end == std::string::npos) {
            throw std::runtime_error(gtb_path + ": unterminated name at offset " +
                                     std::to_string(name_off));
        }

        gtb.entries.push_back(
            {cp932_to_utf8_replace(buf.data() + start, end - start), data_off});
    }

    gtb.has_64bit =
        buf.size() >= TRAILER_MARKER.size() &&
        std::memcmp(buf.data() + (buf.size() - TRAILER_MARKER.size()),
                    TRAILER_MARKER.data(), TRAILER_MARKER.size()) == 0;

    if (gtb.has_64bit) {
        const std::size_t marker_pos = buf.size() - TRAILER_MARKER.size();
        const std::size_t u64_table_bytes = count * kU64Bytes;
        if (marker_pos < kOver2GZeroPadBytes + u64_table_bytes ||
            marker_pos - kOver2GZeroPadBytes - u64_table_bytes < name_table_base) {
            throw std::runtime_error(gtb_path + ": truncated over2G! 64-bit offset table");
        }
        const std::size_t u64_base = marker_pos - kOver2GZeroPadBytes - u64_table_bytes;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint64_t off64 = rd_u64le(buf, u64_base + i * kU64Bytes);
            if ((off64 & kLowU32Mask) != gtb.entries[i].offset) {
                throw std::runtime_error(
                    gtb_path + ": over2G! 64-bit offset low-32 mismatch at entry " +
                    std::to_string(i));
            }
            gtb.entries[i].offset = off64;
        }
    }
    return gtb;
}

void write_gtb(const std::string& gtb_path, const std::vector<GtbEntry>& entries,
               bool has_64bit) {
    const std::size_t count = entries.size();
    if (!has_64bit) {
        for (const auto& e : entries) {
            if (e.offset > kLowU32Mask) {
                throw std::runtime_error(
                    gtb_path + ": 64-bit offset requires over2G! GTB table");
            }
        }
    }

    Bytes name_table;
    std::vector<std::uint32_t> name_offsets;
    name_offsets.reserve(count);
    for (const auto& e : entries) {
        name_offsets.push_back(static_cast<std::uint32_t>(name_table.size()));
        const Bytes enc = utf8_to_cp932_replace(e.name);
        name_table.insert(name_table.end(), enc.begin(), enc.end());
        name_table.push_back(0u);
    }

    Bytes out;
    put_u32le(out, static_cast<std::uint32_t>(count));
    for (std::uint32_t off : name_offsets) {
        put_u32le(out, off);
    }
    for (const auto& e : entries) {
        put_u32le(out, static_cast<std::uint32_t>(e.offset & kLowU32Mask));
    }
    out.insert(out.end(), name_table.begin(), name_table.end());

    if (has_64bit) {
        while (out.size() % kGtbAlignBytes != 0u) {
            out.push_back(0u);
        }
        for (const auto& e : entries) {
            put_u64le(out, e.offset);
        }
        out.insert(out.end(), kOver2GZeroPadBytes, 0u);
        out.insert(out.end(), TRAILER_MARKER.begin(), TRAILER_MARKER.end());
    }

    write_file(gtb_path, out);
}

// GPK entry headers

EntryHeader read_entry_header(const Bytes& gpk_buf, std::size_t offset) {
    return read_entry_header_ptr(gpk_buf.data(), gpk_buf.size(), offset);
}

Archive read_archive(const std::string& gpk_path, const std::string& gtb_path) {
    Archive arc;
    arc.gtb = read_gtb(gtb_path);
    arc.gpk_buf = read_file(gpk_path);
    arc.gpk_size = arc.gpk_buf.size();

    arc.entries.reserve(arc.gtb.entries.size());
    for (const auto& ge : arc.gtb.entries) {
        EntryHeader h = read_entry_header(arc.gpk_buf, static_cast<std::size_t>(ge.offset));
        h.name = ge.name;
        h.offset = ge.offset;
        arc.entries.push_back(std::move(h));
    }
    return arc;
}

Bytes extract_entry(const Bytes& gpk_buf, std::size_t offset) {
    const EntryHeader h = read_entry_header(gpk_buf, offset);
    const std::size_t start = offset + GPK_HEADER_SIZE;
    const std::size_t end = std::min(gpk_buf.size(), start + h.packed_size);
    return Bytes(gpk_buf.begin() + static_cast<std::ptrdiff_t>(start),
                 gpk_buf.begin() + static_cast<std::ptrdiff_t>(end));
}

Bytes extract_entry_raw(const Archive& arc, std::size_t entry_index) {
    if (entry_index >= arc.entries.size()) {
        throw std::out_of_range("extract_entry_raw: entry_index out of range");
    }
    const auto next_off = build_next_offset_map(arc);
    const std::uint64_t start = arc.entries[entry_index].offset;
    const std::uint64_t end =
        std::min<std::uint64_t>(next_off.at(start), static_cast<std::uint64_t>(arc.gpk_size));
    if (end <= start) {
        return {};
    }
    return Bytes(arc.gpk_buf.begin() + static_cast<std::ptrdiff_t>(start),
                 arc.gpk_buf.begin() + static_cast<std::ptrdiff_t>(end));
}

// RLE-XOR alpha mask & DWQ codecs

Bytes decode_rle_xor_mask(const std::uint8_t* stream, std::size_t stream_size,
                          int width, int height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("decode_rle_xor_mask: invalid dimensions");
    }
    if (stream == nullptr && stream_size > 0u) {
        throw std::invalid_argument("decode_rle_xor_mask: null stream pointer");
    }

    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    Bytes out(w * h, 0u);
    Bytes prev(w, 0u);
    Bytes delta(w, 0u);

    std::size_t pos = 0u;
    for (std::size_t y = 0u; y < h; ++y) {
        std::fill(delta.begin(), delta.end(), 0u);
        std::size_t x = 0u;
        while (x < w) {
            if (pos >= stream_size) {
                throw std::runtime_error("decode_rle_xor_mask: truncated RLE stream");
            }
            const std::uint8_t b = stream[pos++];
            if (b == 0u) {
                if (pos >= stream_size) {
                    throw std::runtime_error("decode_rle_xor_mask: truncated zero run");
                }
                const std::size_t run = stream[pos++];
                if (run == 0u || x + run > w) {
                    throw std::runtime_error("decode_rle_xor_mask: zero run overflows scanline");
                }
                x += run;
                continue;
            }
            delta[x++] = b;
        }
        std::uint8_t* dst_row = out.data() + y * w;
        for (std::size_t i = 0u; i < w; ++i) {
            prev[i] ^= delta[i];
            dst_row[i] = prev[i];
        }
    }
    return out;
}

Bytes encode_rle_xor_mask(const std::uint8_t* alpha, int width, int height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("encode_rle_xor_mask: invalid dimensions");
    }
    if (alpha == nullptr) {
        throw std::invalid_argument("encode_rle_xor_mask: null alpha pointer");
    }

    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    Bytes out;
    out.reserve((w * h) / 2u);

    Bytes prev(w, 0u);
    Bytes delta(w, 0u);
    for (std::size_t y = 0u; y < h; ++y) {
        const std::uint8_t* row = alpha + y * w;
        for (std::size_t i = 0u; i < w; ++i) {
            delta[i] = static_cast<std::uint8_t>(row[i] ^ prev[i]);
            prev[i] = row[i];
        }
        std::size_t x = 0u;
        while (x < w) {
            if (delta[x] != 0u) {
                out.push_back(delta[x]);
                ++x;
                continue;
            }
            std::size_t run = 0u;
            const std::size_t max_run = std::min(w - x, kMaxRleZeroRun);
            while (run < max_run && delta[x + run] == 0u) {
                ++run;
            }
            out.push_back(0u);
            out.push_back(static_cast<std::uint8_t>(run));
            x += run;
        }
    }
    return out;
}

DwqCropMargins compute_dwq_crop_margins(const DwqRgbaImage& img) {
    if (img.width <= 0 || img.height <= 0 ||
        img.pixels.size() != static_cast<std::size_t>(img.width) *
                                 static_cast<std::size_t>(img.height) * kRgbaChannels) {
        throw std::invalid_argument("compute_dwq_crop_margins: invalid RGBA image");
    }

    int min_x = img.width;
    int max_x = -1;
    int min_y = img.height;
    int max_y = -1;
    for (int y = 0; y < img.height; ++y) {
        const std::uint8_t* row =
            img.pixels.data() + static_cast<std::size_t>(y) * img.width * kRgbaChannels;
        for (int x = 0; x < img.width; ++x) {
            if (row[x * kRgbaChannels + kAlphaIndex] > kTransparentAlpha) {
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }
        }
    }
    if (max_x < min_x || max_y < min_y) {
        return DwqCropMargins{0, 0, 0, 0};
    }
    return DwqCropMargins{
        static_cast<std::int32_t>(min_y),
        static_cast<std::int32_t>(min_x),
        static_cast<std::int32_t>(img.width - 1 - max_x),
        static_cast<std::int32_t>(img.height - 1 - max_y),
    };
}

DwqRgbaImage decode_dwq_masked_rgba(const std::uint8_t* raw_dwq, std::size_t raw_size) {
    if (raw_dwq == nullptr || raw_size < GPK_HEADER_SIZE + kBmpHeaderSize) {
        throw std::runtime_error("decode_dwq_masked_rgba: DWQ buffer too small");
    }
    const EntryHeader h = read_entry_header_ptr(raw_dwq, raw_size, 0u);
    const char pt = packtype_char(h);
    if (pt != kPackTypeBmpWithMask && pt != kPackTypeIndexedRle) {
        throw std::runtime_error("decode_dwq_masked_rgba: unsupported PACKTYPE");
    }

    const std::uint8_t* bmp_hdr = raw_dwq + GPK_HEADER_SIZE;
    const std::size_t bmp_avail = raw_size - GPK_HEADER_SIZE;
    if (bmp_hdr[0] != 'B' || bmp_hdr[1] != 'M') {
        throw std::runtime_error("decode_dwq_masked_rgba: expected embedded BM header");
    }
    const int width = static_cast<int>(rd_i32le(bmp_hdr, bmp_avail, kBmpWidthOffset));
    const int height = read_bmp_abs_height(bmp_hdr, bmp_avail);
    const std::uint16_t bit_count = rd_u16le(bmp_hdr, bmp_avail, kBmpBitCountOffset);
    const std::uint32_t color_bf_off = rd_u32le(bmp_hdr, bmp_avail, kBmpOffBitsOffset);
    if (width <= 0 || height <= 0 || color_bf_off < kBmpHeaderSize) {
        throw std::runtime_error("decode_dwq_masked_rgba: invalid BMP dimensions or bfOffBits");
    }

    DwqRgbaImage img;
    img.width = width;
    img.height = height;
    const std::size_t total_px =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    img.pixels.assign(total_px * kRgbaChannels, kOpaqueAlpha);

    if (pt == kPackTypeBmpWithMask && bit_count == kBmp24BitCount) {
        const std::size_t stride = bmp_row_stride_24(width);
        const std::size_t bmp_needed =
            static_cast<std::size_t>(color_bf_off) + stride * static_cast<std::size_t>(height);
        if (bmp_avail < bmp_needed || h.packed_size < bmp_needed ||
            raw_size < GPK_HEADER_SIZE + h.packed_size) {
            throw std::runtime_error("decode_dwq_masked_rgba: truncated 24-bit BMP payload");
        }
        const std::uint8_t* bmp_pixels = bmp_hdr + color_bf_off;
        for (int y = 0; y < height; ++y) {
            const std::uint8_t* src_row = bmp_pixels + static_cast<std::size_t>(y) * stride;
            std::uint8_t* dst_row =
                img.pixels.data() + static_cast<std::size_t>(y) * width * kRgbaChannels;
            for (int x = 0; x < width; ++x) {
                dst_row[x * kRgbaChannels + kRedIndex] =
                    src_row[x * kRgbChannels + kBgrRedIndex];
                dst_row[x * kRgbaChannels + kGreenIndex] =
                    src_row[x * kRgbChannels + kBgrGreenIndex];
                dst_row[x * kRgbaChannels + kBlueIndex] =
                    src_row[x * kRgbChannels + kBgrBlueIndex];
                dst_row[x * kRgbaChannels + kAlphaIndex] = kOpaqueAlpha;
            }
        }
    } else if (pt == kPackTypeIndexedRle && bit_count == kBmp8BitCount) {
        if (h.packed_size <= color_bf_off || raw_size < GPK_HEADER_SIZE + h.packed_size ||
            (color_bf_off - kBmpHeaderSize) % kMaskPaletteEntrySize != 0u) {
            throw std::runtime_error("decode_dwq_masked_rgba: truncated PACKTYPE=3A color stream");
        }
        const std::size_t pal_entries = (color_bf_off - kBmpHeaderSize) / kMaskPaletteEntrySize;
        if (pal_entries == 0u || pal_entries > kMaskPaletteColorCount) {
            throw std::runtime_error("decode_dwq_masked_rgba: invalid PACKTYPE=3A color palette");
        }
        const std::uint8_t* color_pal = bmp_hdr + kBmpHeaderSize;
        const Bytes color_indices = decode_rle_xor_mask(
            bmp_hdr + color_bf_off, h.packed_size - color_bf_off, width, height);
        for (std::size_t i = 0; i < total_px; ++i) {
            const std::size_t idx = std::min<std::size_t>(color_indices[i], pal_entries - 1u);
            const std::uint8_t* entry = color_pal + idx * kMaskPaletteEntrySize;
            img.pixels[i * kRgbaChannels + kBlueIndex] = entry[kBgrBlueIndex];
            img.pixels[i * kRgbaChannels + kGreenIndex] = entry[kBgrGreenIndex];
            img.pixels[i * kRgbaChannels + kRedIndex] = entry[kBgrRedIndex];
            img.pixels[i * kRgbaChannels + kAlphaIndex] = kOpaqueAlpha;
        }
    } else {
        throw std::runtime_error("decode_dwq_masked_rgba: unsupported bit count for PACKTYPE");
    }

    const std::size_t mask_offset = GPK_HEADER_SIZE + h.packed_size;
    if (raw_size < mask_offset + kBmpHeaderSize) {
        throw std::runtime_error("decode_dwq_masked_rgba: truncated MASK section");
    }
    const Bytes alpha =
        decode_mask_section_alpha(raw_dwq + mask_offset, raw_size - mask_offset, width, height);
    for (std::size_t i = 0; i < total_px; ++i) {
        img.pixels[i * kRgbaChannels + kAlphaIndex] = alpha[i];
    }
    return img;
}

// Payload sniffing (repack)

std::array<std::uint8_t, 16> detect_type_tag(const Bytes& data) {
    static const std::uint8_t kPngMagic[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    static const std::uint8_t kBmMagic[2] = {'B', 'M'};
    static const std::uint8_t kJpgMagic[2] = {0xFFu, 0xD8u};
    static const std::uint8_t kGifMagic[4] = {'G', 'I', 'F', '8'};

    const char* prefix = "BIN ";
    if (starts_with(data, kPngMagic, sizeof(kPngMagic))) {
        prefix = "PNG ";
    } else if (starts_with(data, kBmMagic, sizeof(kBmMagic))) {
        prefix = "BMP ";
    } else if (starts_with(data, kJpgMagic, sizeof(kJpgMagic))) {
        prefix = "JPG ";
    } else if (starts_with(data, kGifMagic, sizeof(kGifMagic))) {
        prefix = "GIF ";
    }

    std::array<std::uint8_t, 16> tag{};
    tag.fill(0x20u);
    std::memcpy(tag.data(), prefix, 4u);
    return tag;
}

std::pair<std::uint32_t, std::uint32_t> detect_dims(const Bytes& data) {
    static const std::uint8_t kPng4[4] = {0x89u, 'P', 'N', 'G'};
    static const std::uint8_t kBmMagic[2] = {'B', 'M'};

    if (starts_with(data, kPng4, sizeof(kPng4))) {
        if (data.size() < 24u) {
            return {0u, 0u};
        }
        const auto be = [&](std::size_t o) {
            return (static_cast<std::uint32_t>(data[o]) << kByteShift3) |
                   (static_cast<std::uint32_t>(data[o + 1u]) << kByteShift2) |
                   (static_cast<std::uint32_t>(data[o + 2u]) << kByteShift1) |
                   static_cast<std::uint32_t>(data[o + 3u]);
        };
        return {be(16u), be(20u)};
    }
    if (starts_with(data, kBmMagic, sizeof(kBmMagic))) {
        if (data.size() < 26u) {
            return {0u, 0u};
        }
        const std::int64_t w = rd_i32le(data.data(), data.size(), kBmpWidthOffset);
        const std::int64_t h = rd_i32le(data.data(), data.size(), kBmpHeightOffset);
        return {static_cast<std::uint32_t>(w),
                static_cast<std::uint32_t>(h < 0 ? -h : h)};
    }
    return {0u, 0u};
}

Bytes build_entry(const EntryHeader& orig, const Bytes& payload) {
    const auto tag = has_non_zero_type_raw(orig.type_raw) ? orig.type_raw
                                                          : detect_type_tag(payload);
    auto [width, height] = detect_dims(payload);
    if (width == 0u || height == 0u) {
        width = orig.width;
        height = orig.height;
    }
    if (height > kMaxU16Value) {
        throw std::runtime_error("build_entry: height exceeds DWQ u16 limit");
    }

    Bytes hdr(GPK_HEADER_SIZE, 0u);
    std::memcpy(hdr.data(), tag.data(), kTypeFieldBytes);
    std::memcpy(hdr.data() + kMetaFieldOffset, orig.meta.data(), kMetaFieldBytes);
    wr_u32le(hdr.data() + kPackedSizeOffset, static_cast<std::uint32_t>(payload.size()));
    wr_u32le(hdr.data() + kWidthOffset, width);
    wr_u16le(hdr.data() + kHeightOffset, static_cast<std::uint16_t>(height));
    wr_u16le(hdr.data() + kBppOffset, orig.bpp);
    std::memcpy(hdr.data() + kPadOffset, orig.pad.data(), kPadBytes);
    std::memcpy(hdr.data() + kPacktypeOffset, orig.packtype.data(), kPacktypeBytes);

    if (has_alpha_flag(orig)) {
        const auto decoded = cg::decode_image(payload);
        if (decoded && decoded->channels == kRgbaChannels) {
            const DwqRgbaImage rgba{decoded->width, decoded->height, decoded->px};
            write_crop_margins(hdr.data(), compute_dwq_crop_margins(rgba));
        }
    }

    hdr.insert(hdr.end(), payload.begin(), payload.end());
    return hdr;
}

Bytes build_entry_from_raw(const std::uint8_t* orig_raw,
                           std::size_t orig_raw_size,
                           const Bytes& payload) {
    if (orig_raw == nullptr || orig_raw_size < GPK_HEADER_SIZE) {
        throw std::runtime_error("build_entry_from_raw: truncated original entry");
    }
    const EntryHeader orig = read_entry_header_ptr(orig_raw, orig_raw_size, 0u);
    const char pt = packtype_char(orig);

    if ((pt == kPackTypeBmpWithMask || pt == kPackTypeIndexedRle) &&
        orig_raw_size >= GPK_HEADER_SIZE + kBmpHeaderSize &&
        orig_raw[GPK_HEADER_SIZE] == 'B' && orig_raw[GPK_HEADER_SIZE + 1u] == 'M') {
        const auto decoded = cg::decode_image(payload);
        if (!decoded) {
            throw std::runtime_error(
                "build_entry_from_raw: replacement payload is not a decodable image");
        }
        if (pt == kPackTypeBmpWithMask) {
            return encode_masked_2a_entry(orig_raw, orig_raw_size, orig, *decoded);
        }
        return encode_masked_3a_entry(orig_raw, orig_raw_size, orig, *decoded);
    }

    return build_entry(orig, payload);
}

// Repack

void repack(const std::string& gpk_in, const std::string& gtb_in,
            const std::string& gpk_out, const std::string& gtb_out,
            const std::vector<std::pair<std::string, Bytes>>& replacements) {
    const Archive arc = read_archive(gpk_in, gtb_in);
    if (arc.entries.empty()) {
        throw std::runtime_error(gpk_in + ": archive has no entries");
    }

    std::map<std::string, const Bytes*> repl;
    for (const auto& [name, data] : replacements) {
        repl[name] = &data;
    }

    const auto next_off = build_next_offset_map(arc);

    std::ofstream out(std::filesystem::u8path(gpk_out), std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot create " + gpk_out);
    }

    std::vector<GtbEntry> new_entries;
    new_entries.reserve(arc.entries.size());
    std::uint64_t cursor = 0u;

    for (const auto& entry : arc.entries) {
        const std::uint64_t start = entry.offset;
        const std::uint64_t end =
            std::min<std::uint64_t>(next_off.at(start), static_cast<std::uint64_t>(arc.gpk_size));
        const std::size_t orig_slice_size =
            end > start ? static_cast<std::size_t>(end - start) : 0u;

        Bytes built;
        const std::uint8_t* data = nullptr;
        std::size_t size = 0u;

        const auto it = repl.find(entry.name);
        if (it != repl.end()) {
            built = build_entry_from_raw(
                arc.gpk_buf.data() + static_cast<std::size_t>(start), orig_slice_size, *it->second);
            data = built.data();
            size = built.size();
        } else {
            data = arc.gpk_buf.data() + static_cast<std::size_t>(start);
            size = orig_slice_size;
        }

        new_entries.push_back({entry.name, cursor});
        if (size != 0u) {
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }
        cursor += size;
    }
    out.close();
    if (!out) {
        throw std::runtime_error("write failed: " + gpk_out);
    }

    const bool write_64bit = arc.gtb.has_64bit || cursor > kLowU32Mask;
    write_gtb(gtb_out, new_entries, write_64bit);
}

}  // namespace mgi::gpk
