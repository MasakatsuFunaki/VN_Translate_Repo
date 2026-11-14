// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menus/menu_patcher.h"

#include "charts/charts.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exc::menus {

namespace fs = std::filesystem;

namespace {

inline constexpr std::size_t kGtbCountFieldSize = 4;
inline constexpr std::size_t kGtbOffsetElemSize = 4;
inline constexpr std::size_t kGtbOffsetTableCount = 2;
inline constexpr std::size_t kBmpWidthFieldOffset = 18;
inline constexpr std::size_t kBmpHeightFieldOffset = 22;
inline constexpr std::size_t kBmpBitCountFieldOffset = 28;
inline constexpr std::uint16_t kBmp24BitCount = 24;
inline constexpr std::uint8_t kOpaqueAlpha = 255;
inline constexpr std::uint8_t kTransparentAlpha = 0;
inline constexpr std::size_t kMaskPaletteEntrySize = 4;
inline constexpr std::size_t kMaskPaletteRedChannelIndex = 2;
inline constexpr std::size_t kRleInitialReserveDivisor = 2;
inline constexpr std::size_t kFxfHeaderReservePadding = 128;
inline constexpr std::size_t kFxfBodyReservePadding = 64;

inline constexpr int kByteMask = 0xFF;
inline constexpr unsigned int kByte1Shift = 8;
inline constexpr unsigned int kByte2Shift = 16;
inline constexpr unsigned int kByte3Shift = 24;

// Number of translated menu sprites expected in a full build.
inline constexpr std::size_t kExpectedTranslatedSpriteCount = 64;

inline constexpr std::uint32_t kMurmurMultiplier = 0x5BD1E995u;
inline constexpr unsigned int kMurmurShiftR = 24u;
inline constexpr unsigned int kMurmurFinalShift1 = 13u;
inline constexpr unsigned int kMurmurFinalShift2 = 15u;
inline constexpr std::size_t kWordBytes = 4u;
inline constexpr std::size_t kRemThreeBytes = 3u;
inline constexpr std::size_t kRemTwoBytes = 2u;
inline constexpr std::size_t kRemOneByte = 1u;
inline constexpr std::size_t kByteOffset1 = 1u;
inline constexpr std::size_t kByteOffset2 = 2u;
inline constexpr std::size_t kByteOffset3 = 3u;

inline constexpr std::uint32_t kXorStreamMultiplier = 0x9Eu;
inline constexpr std::uint32_t kXorStreamAddend = 0x37u;
inline constexpr std::size_t kHashByteIndexMask = 3u;
inline constexpr unsigned int kBitsPerByte = 8u;
inline constexpr std::uint32_t kLowByteMask = 0xFFu;

inline constexpr std::size_t kPatchVersionOffset = 8;
inline constexpr std::size_t kPatchStandaloneCountOffset = 12;
inline constexpr std::size_t kPatchOrigGpkSizeOffset = 16;
inline constexpr std::size_t kPatchNewGpkSizeOffset = 20;
inline constexpr std::size_t kPatchOrigGtbSizeOffset = 24;
inline constexpr std::size_t kPatchOrigGtbHashOffset = 28;
inline constexpr std::size_t kPatchNewGtbSizeOffset = 32;
inline constexpr std::size_t kPatchNewGtbHashOffset = 36;
inline constexpr std::size_t kPatchGpkEntryCountOffset = 40;

inline constexpr std::size_t kU16ByteSize = 2;
inline constexpr std::size_t kStandaloneOrigHashRelOffset = 4;
inline constexpr std::size_t kStandaloneNewSizeRelOffset = 8;
inline constexpr std::size_t kStandaloneNewHashRelOffset = 12;

inline constexpr std::size_t kGpkRecOrigOffsetRelOffset = 4;
inline constexpr std::size_t kGpkRecOrigSizeRelOffset = 8;
inline constexpr std::size_t kGpkRecOrigHashRelOffset = 12;
inline constexpr std::size_t kGpkRecNewSizeRelOffset = 16;
inline constexpr std::size_t kGpkRecNewHashRelOffset = 20;

struct StandaloneFileDescriptor {
    const char* rel_path;
    const char* backup_dir;
    const char* filename;
};

constexpr std::array<StandaloneFileDescriptor, kExpectedStandaloneFileCount> kStandaloneFiles = {{
    {"spt\\01.spt",            "spt_backup",   "01.spt"},
    {"spt\\02.spt",            "spt_backup",   "02.spt"},
    {"spt\\03.spt",            "spt_backup",   "03.spt"},
    {"spt\\04.spt",            "spt_backup",   "04.spt"},
    {"spt\\05.spt",            "spt_backup",   "05.spt"},
    {"spt\\06.spt",            "spt_backup",   "06.spt"},
    {"spt\\062.spt",           "spt_backup",   "062.spt"},
    {"spt\\07.spt",            "spt_backup",   "07.spt"},
    {"spt\\08.spt",            "spt_backup",   "08.spt"},
    {"spt\\_exban.spt",        "spt_backup",   "_exban.spt"},
    {"spt\\_exgame.spt",       "spt_backup",   "_exgame.spt"},
    {"spt\\_meta.spt",         "spt_backup",   "_meta.spt"},
    {"spt\\_staff.spt",        "spt_backup",   "_staff.spt"},
    {"spt\\backscript.spt",    "spt_backup",   "backscript.spt"},
    {"spt\\omake.spt",         "spt_backup",   "omake.spt"},
    {"spt\\sys.spt",           "spt_backup",   "sys.spt"},
    {"spt\\charaname.xtx",     "xtx_backup",   "charaname.xtx"},
    {"chart\\chart1.fxf",      "chart_backup", "chart1.fxf"},
    {"chart\\chart2.fxf",      "chart_backup", "chart2.fxf"},
    {"chart\\chart3.fxf",      "chart_backup", "chart3.fxf"},
    {"chart\\chart4.fxf",      "chart_backup", "chart4.fxf"},
    {"chart\\chart5.fxf",      "chart_backup", "chart5.fxf"},
    {"chart\\chart6.fxf",      "chart_backup", "chart6.fxf"},
    {"chart\\chart7.fxf",      "chart_backup", "chart7.fxf"},
    {"chart\\chart8.fxf",      "chart_backup", "chart8.fxf"},
    {"init2\\mainControl.fxf", "init2_backup", "mainControl.fxf"},
    {"init2\\backlog.fxf",     "init2_backup", "backlog.fxf"},
}};

class ScopedComInit {
public:
    ScopedComInit() noexcept {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        should_uninit_ = (hr == S_OK || hr == S_FALSE);
    }
    ~ScopedComInit() {
        if (should_uninit_) {
            CoUninitialize();
        }
    }
    ScopedComInit(const ScopedComInit&) = delete;
    ScopedComInit& operator=(const ScopedComInit&) = delete;

private:
    bool should_uninit_ = false;
};

std::uint16_t read_u16_le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << kByte1Shift);
}

std::uint32_t read_u32_le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << kByte1Shift) |
           (static_cast<std::uint32_t>(p[2]) << kByte2Shift) |
           (static_cast<std::uint32_t>(p[3]) << kByte3Shift);
}

std::int32_t read_i32_le(const std::uint8_t* p) noexcept {
    return static_cast<std::int32_t>(read_u32_le(p));
}

void write_u32_le(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & kByteMask);
    p[1] = static_cast<std::uint8_t>((v >> kByte1Shift) & kByteMask);
    p[2] = static_cast<std::uint8_t>((v >> kByte2Shift) & kByteMask);
    p[3] = static_cast<std::uint8_t>((v >> kByte3Shift) & kByteMask);
}

void write_i32_le(std::uint8_t* p, std::int32_t v) noexcept {
    write_u32_le(p, static_cast<std::uint32_t>(v));
}

void append_u16_le(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kByte1Shift) & kByteMask));
}

void append_u32_le(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kByte1Shift) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kByte2Shift) & kByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kByte3Shift) & kByteMask));
}

fs::path resolve_original_file_path(const fs::path& game_root,
                                    const char* backup_dir,
                                    const char* filename,
                                    const char* rel_path) {
    std::error_code ec;
    const fs::path bak_cand = game_root / fs::u8path(std::string(rel_path) + ".bak");
    if (fs::is_regular_file(bak_cand, ec)) {
        return bak_cand;
    }
    if (backup_dir != nullptr && backup_dir[0] != '\0' &&
        filename != nullptr && filename[0] != '\0') {
        const fs::path backup_cand = game_root / backup_dir / filename;
        if (fs::is_regular_file(backup_cand, ec)) {
            return backup_cand;
        }
    }
    const fs::path live_cand = game_root / fs::u8path(rel_path);
    if (fs::is_regular_file(live_cand, ec)) {
        return live_cand;
    }
    throw std::runtime_error(std::string("resolve_original_file_path: missing original file for ") +
                             rel_path);
}

void ensure_bak_backup_file(const fs::path& target_path, const Bytes& orig_bytes) {
    std::error_code ec;
    fs::path bak_path = target_path;
    bak_path += ".bak";
    if (fs::is_regular_file(bak_path, ec) &&
        fs::file_size(bak_path, ec) == orig_bytes.size() && !ec) {
        return;
    }
    write_file(bak_path.u8string(), orig_bytes);
}

std::string to_lower_ascii(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch + ('a' - 'A'));
        }
    }
    return s;
}

std::size_t bmp_row_stride_24(int width) noexcept {
    const std::size_t raw_row = static_cast<std::size_t>(width) * kBgrChannels;
    const std::size_t align = static_cast<std::size_t>(kBmpRowAlignment);
    return (raw_row + (align - 1)) & ~(align - 1);
}

}  // namespace

Bytes decode_rle_xor_mask(const std::uint8_t* stream, std::size_t stream_size,
                          int width, int height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("decode_rle_xor_mask: invalid dimensions");
    }
    if (!stream && stream_size > 0) {
        throw std::invalid_argument("decode_rle_xor_mask: null stream pointer");
    }

    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    Bytes out(w * h, 0);
    Bytes prev(w, 0);
    Bytes delta(w, 0);

    std::size_t pos = 0;
    for (std::size_t y = 0; y < h; ++y) {
        std::fill(delta.begin(), delta.end(), 0);
        std::size_t x = 0;
        while (x < w) {
            if (pos >= stream_size) {
                throw std::runtime_error("decode_rle_xor_mask: truncated RLE stream");
            }
            const std::uint8_t b = stream[pos++];
            if (b == 0) {
                if (pos >= stream_size) {
                    throw std::runtime_error("decode_rle_xor_mask: truncated zero run");
                }
                const std::size_t run = stream[pos++];
                if (run == 0 || x + run > w) {
                    throw std::runtime_error("decode_rle_xor_mask: zero run overflows scanline");
                }
                x += run;
                continue;
            }
            delta[x++] = b;
        }
        std::uint8_t* dst_row = out.data() + y * w;
        for (std::size_t i = 0; i < w; ++i) {
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
    if (!alpha) {
        throw std::invalid_argument("encode_rle_xor_mask: null alpha pointer");
    }

    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    Bytes out;
    out.reserve(w * h / kRleInitialReserveDivisor);

    Bytes prev(w, 0);
    Bytes delta(w, 0);

    for (std::size_t y = 0; y < h; ++y) {
        const std::uint8_t* row = alpha + y * w;
        for (std::size_t i = 0; i < w; ++i) {
            delta[i] = static_cast<std::uint8_t>(row[i] ^ prev[i]);
            prev[i] = row[i];
        }
        std::size_t x = 0;
        while (x < w) {
            if (delta[x] != 0) {
                out.push_back(delta[x]);
                ++x;
                continue;
            }
            std::size_t run = 0;
            const std::size_t max_run =
                std::min(w - x, static_cast<std::size_t>(kMaxRleZeroRun));
            while (run < max_run && delta[x + run] == 0) {
                ++run;
            }
            out.push_back(0);
            out.push_back(static_cast<std::uint8_t>(run));
            x += run;
        }
    }
    return out;
}

CropMargins compute_crop_margins(const RgbaImage& img) {
    if (img.width <= 0 || img.height <= 0 ||
        img.pixels.size() != static_cast<std::size_t>(img.width) *
                                 static_cast<std::size_t>(img.height) * kRgbaChannels) {
        throw std::invalid_argument("compute_crop_margins: invalid RgbaImage");
    }

    int min_x = img.width;
    int max_x = -1;
    int min_y = img.height;
    int max_y = -1;

    for (int y = 0; y < img.height; ++y) {
        const std::uint8_t* row =
            img.pixels.data() + static_cast<std::size_t>(y) * img.width * kRgbaChannels;
        for (int x = 0; x < img.width; ++x) {
            if (row[x * kRgbaChannels + kAlphaChannel] > kTransparentAlpha) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }

    if (max_x < min_x || max_y < min_y) {
        return CropMargins{0, 0, 0, 0};
    }

    return CropMargins{
        static_cast<std::int32_t>(min_y),
        static_cast<std::int32_t>(min_x),
        static_cast<std::int32_t>(img.width - 1 - max_x),
        static_cast<std::int32_t>(img.height - 1 - max_y),
    };
}

RgbaImage decode_dwq_to_rgba(const std::uint8_t* dwq_data, std::size_t dwq_size) {
    if (!dwq_data || dwq_size < kDwqHeaderSize + kBmpHeaderSize) {
        throw std::runtime_error("decode_dwq_to_rgba: DWQ buffer too small");
    }

    const char pack_type = static_cast<char>(dwq_data[kDwqPackTypeOffset]);
    if (pack_type != kPackTypeUncompressedBmp && pack_type != kPackTypeBmpWithMask) {
        throw std::runtime_error(std::string("decode_dwq_to_rgba: unsupported PACKTYPE=") +
                                 pack_type);
    }

    const std::uint8_t* bmp_hdr = dwq_data + kDwqHeaderSize;
    const int width = static_cast<int>(read_i32_le(bmp_hdr + kBmpWidthFieldOffset));
    const int raw_height = static_cast<int>(read_i32_le(bmp_hdr + kBmpHeightFieldOffset));
    const int height = std::abs(raw_height);
    const std::uint16_t bit_count = read_u16_le(bmp_hdr + kBmpBitCountFieldOffset);

    if (width <= 0 || height <= 0 || bit_count != kBmp24BitCount) {
        throw std::runtime_error("decode_dwq_to_rgba: expected 24-bit BMP payload");
    }

    const std::size_t stride = bmp_row_stride_24(width);
    const std::size_t bmp_bytes_needed = kBmpHeaderSize + stride * static_cast<std::size_t>(height);
    if (dwq_size < kDwqHeaderSize + bmp_bytes_needed) {
        throw std::runtime_error("decode_dwq_to_rgba: truncated BMP pixel data");
    }

    RgbaImage img;
    img.width = width;
    img.height = height;
    img.pixels.resize(static_cast<std::size_t>(width) * height * kRgbaChannels, kOpaqueAlpha);

    const std::uint8_t* bmp_pixels = bmp_hdr + kBmpHeaderSize;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src_row = bmp_pixels + static_cast<std::size_t>(y) * stride;
        std::uint8_t* dst_row =
            img.pixels.data() + static_cast<std::size_t>(y) * width * kRgbaChannels;
        for (int x = 0; x < width; ++x) {
            dst_row[x * kRgbaChannels + kRedChannel] = src_row[x * kBgrChannels + kBgrRedIndex];
            dst_row[x * kRgbaChannels + kGreenChannel] = src_row[x * kBgrChannels + kBgrGreenIndex];
            dst_row[x * kRgbaChannels + kBlueChannel] = src_row[x * kBgrChannels + kBgrBlueIndex];
            dst_row[x * kRgbaChannels + kAlphaChannel] = kOpaqueAlpha;
        }
    }

    if (pack_type == kPackTypeBmpWithMask) {
        const std::int32_t data_size = read_i32_le(dwq_data + kDwqDataSizeOffset);
        if (data_size <= 0 ||
            dwq_size < kDwqHeaderSize + static_cast<std::size_t>(data_size) + kMaskHeaderTotalSize) {
            throw std::runtime_error("decode_dwq_to_rgba: truncated MASK section");
        }
        const std::size_t mask_offset = kDwqHeaderSize + static_cast<std::size_t>(data_size);
        const std::uint8_t* mask_palette = dwq_data + mask_offset + kBmpHeaderSize;
        const std::uint8_t* mask_stream = dwq_data + mask_offset + kMaskHeaderTotalSize;
        const std::size_t mask_stream_size =
            dwq_size - (mask_offset + kMaskHeaderTotalSize);

        const Bytes alpha_indices =
            decode_rle_xor_mask(mask_stream, mask_stream_size, width, height);
        const std::size_t total_px = static_cast<std::size_t>(width) * height;
        for (std::size_t i = 0; i < total_px; ++i) {
            const std::uint8_t idx = alpha_indices[i];
            img.pixels[i * kRgbaChannels + kAlphaChannel] =
                mask_palette[static_cast<std::size_t>(idx) * kMaskPaletteEntrySize +
                             kMaskPaletteRedChannelIndex];
        }
    }

    return img;
}

Bytes encode_dwq_from_rgba(const std::uint8_t* orig_dwq, std::size_t orig_size,
                           const RgbaImage& img, bool update_crop_margins) {
    if (!orig_dwq || orig_size < kDwqHeaderSize + kBmpHeaderSize) {
        throw std::invalid_argument("encode_dwq_from_rgba: invalid original DWQ buffer");
    }
    if (img.width <= 0 || img.height <= 0 ||
        img.pixels.size() != static_cast<std::size_t>(img.width) *
                                 static_cast<std::size_t>(img.height) * kRgbaChannels) {
        throw std::invalid_argument("encode_dwq_from_rgba: invalid RgbaImage buffer");
    }

    const char pack_type = static_cast<char>(orig_dwq[kDwqPackTypeOffset]);
    if (pack_type != kPackTypeUncompressedBmp && pack_type != kPackTypeBmpWithMask) {
        throw std::runtime_error(std::string("encode_dwq_from_rgba: unsupported PACKTYPE=") +
                                 pack_type);
    }

    const std::uint8_t* bmp_hdr = orig_dwq + kDwqHeaderSize;
    const int orig_w = static_cast<int>(read_i32_le(bmp_hdr + kBmpWidthFieldOffset));
    const int orig_h = std::abs(static_cast<int>(read_i32_le(bmp_hdr + kBmpHeightFieldOffset)));
    const std::uint16_t bit_count = read_u16_le(bmp_hdr + kBmpBitCountFieldOffset);

    if (orig_w != img.width || orig_h != img.height || bit_count != kBmp24BitCount) {
        throw std::runtime_error("encode_dwq_from_rgba: image dimensions do not match DWQ header");
    }

    const std::size_t stride = bmp_row_stride_24(img.width);
    const std::size_t bmp_section_size =
        (pack_type == kPackTypeBmpWithMask)
            ? static_cast<std::size_t>(read_i32_le(orig_dwq + kDwqDataSizeOffset))
            : (orig_size - kDwqHeaderSize);
    if (bmp_section_size < kBmpHeaderSize + stride * static_cast<std::size_t>(img.height) ||
        orig_size < kDwqHeaderSize + bmp_section_size) {
        throw std::runtime_error("encode_dwq_from_rgba: corrupt original BMP section size");
    }

    // Decode original alpha if PACKTYPE=2A so pixels that were transparent in both
    // the original and replacement images keep their original hidden BGR bytes
    // when the PNG carries (0, 0, 0, 0).
    Bytes orig_alpha;
    if (pack_type == kPackTypeBmpWithMask) {
        const std::size_t mask_offset = kDwqHeaderSize + bmp_section_size;
        if (orig_size < mask_offset + kMaskHeaderTotalSize) {
            throw std::runtime_error("encode_dwq_from_rgba: corrupt original MASK section");
        }
        const std::uint8_t* orig_mask_palette = orig_dwq + mask_offset + kBmpHeaderSize;
        const Bytes orig_indices = decode_rle_xor_mask(
            orig_dwq + mask_offset + kMaskHeaderTotalSize,
            orig_size - (mask_offset + kMaskHeaderTotalSize), img.width, img.height);
        orig_alpha.resize(orig_indices.size());
        for (std::size_t i = 0; i < orig_indices.size(); ++i) {
            const std::uint8_t idx = orig_indices[i];
            orig_alpha[i] = orig_mask_palette[static_cast<std::size_t>(idx) * kMaskPaletteEntrySize +
                                              kMaskPaletteRedChannelIndex];
        }
    }

    Bytes out(orig_dwq, orig_dwq + kDwqHeaderSize + bmp_section_size);
    std::uint8_t* dst_bmp_pixels = out.data() + kDwqHeaderSize + kBmpHeaderSize;
    const std::uint8_t* orig_bmp_pixels = orig_dwq + kDwqHeaderSize + kBmpHeaderSize;

    for (int y = 0; y < img.height; ++y) {
        std::uint8_t* dst_row = dst_bmp_pixels + static_cast<std::size_t>(y) * stride;
        const std::uint8_t* orig_row = orig_bmp_pixels + static_cast<std::size_t>(y) * stride;
        const std::uint8_t* src_row =
            img.pixels.data() + static_cast<std::size_t>(y) * img.width * kRgbaChannels;
        for (int x = 0; x < img.width; ++x) {
            const std::uint8_t r = src_row[x * kRgbaChannels + kRedChannel];
            const std::uint8_t g = src_row[x * kRgbaChannels + kGreenChannel];
            const std::uint8_t b = src_row[x * kRgbaChannels + kBlueChannel];
            const std::uint8_t a = src_row[x * kRgbaChannels + kAlphaChannel];
            const std::size_t px_idx = static_cast<std::size_t>(y) * img.width + x;

            if (pack_type == kPackTypeBmpWithMask && a == kTransparentAlpha &&
                orig_alpha[px_idx] == kTransparentAlpha && r == 0 && g == 0 && b == 0) {
                dst_row[x * kBgrChannels + kBgrBlueIndex] =
                    orig_row[x * kBgrChannels + kBgrBlueIndex];
                dst_row[x * kBgrChannels + kBgrGreenIndex] =
                    orig_row[x * kBgrChannels + kBgrGreenIndex];
                dst_row[x * kBgrChannels + kBgrRedIndex] =
                    orig_row[x * kBgrChannels + kBgrRedIndex];
                continue;
            }
            dst_row[x * kBgrChannels + kBgrBlueIndex] = b;
            dst_row[x * kBgrChannels + kBgrGreenIndex] = g;
            dst_row[x * kBgrChannels + kBgrRedIndex] = r;
        }
    }

    if (pack_type == kPackTypeBmpWithMask) {
        if (update_crop_margins &&
            orig_dwq[kDwqCropFlagOffset] == static_cast<std::uint8_t>(kCropFlagActive)) {
            const std::int32_t orig_top = read_i32_le(orig_dwq + kDwqCropTopOffset);
            const std::int32_t orig_left = read_i32_le(orig_dwq + kDwqCropLeftOffset);
            const std::int32_t orig_right = read_i32_le(orig_dwq + kDwqCropRightOffset);
            const std::int32_t orig_bottom = read_i32_le(orig_dwq + kDwqCropBottomOffset);

            if (orig_top != 0 || orig_left != 0 || orig_right != 0 || orig_bottom != 0) {
                const CropMargins margins = compute_crop_margins(img);
                write_i32_le(out.data() + kDwqCropTopOffset, margins.top);
                write_i32_le(out.data() + kDwqCropLeftOffset, margins.left);
                write_i32_le(out.data() + kDwqCropRightOffset, margins.right);
                write_i32_le(out.data() + kDwqCropBottomOffset, margins.bottom);
            }
        }

        const std::size_t mask_offset = kDwqHeaderSize + bmp_section_size;
        out.insert(out.end(), orig_dwq + mask_offset,
                   orig_dwq + mask_offset + kBmpHeaderSize);

        // Write a full 256-entry identity grayscale palette (pal[i] = {i, i, i, 0})
        // so every 8-bit alpha value (0..255) in the re-encoded mask maps 1:1 in
        // FUN_00554240 even when the original sprite had a sparse indexed palette.
        constexpr std::size_t kMaskPaletteColorCount =
            kMaskPaletteSize / kMaskPaletteEntrySize;
        for (std::size_t i = 0; i < kMaskPaletteColorCount; ++i) {
            const auto gray = static_cast<std::uint8_t>(i);
            out.push_back(gray);
            out.push_back(gray);
            out.push_back(gray);
            out.push_back(0);
        }

        const std::size_t total_px = static_cast<std::size_t>(img.width) * img.height;
        Bytes new_alpha(total_px, 0);
        for (std::size_t i = 0; i < total_px; ++i) {
            new_alpha[i] = img.pixels[i * kRgbaChannels + kAlphaChannel];
        }
        const Bytes encoded_mask = encode_rle_xor_mask(new_alpha.data(), img.width, img.height);
        out.insert(out.end(), encoded_mask.begin(), encoded_mask.end());
    }

    return out;
}

RgbaImage read_png_rgba(const std::string& path) {
    using Microsoft::WRL::ComPtr;

    const fs::path fs_path = fs::u8path(path);
    std::error_code ec;
    if (!fs::is_regular_file(fs_path, ec)) {
        throw std::runtime_error("read_png_rgba: file not found: " + path);
    }

    ScopedComInit com_guard;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        throw std::runtime_error("read_png_rgba: failed to create IWICImagingFactory");
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(fs_path.wstring().c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || !decoder) {
        throw std::runtime_error("read_png_rgba: failed to open PNG: " + path);
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        throw std::runtime_error("read_png_rgba: failed to decode PNG frame: " + path);
    }

    UINT width = 0;
    UINT height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) {
        throw std::runtime_error("read_png_rgba: invalid PNG dimensions: " + path);
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) {
        throw std::runtime_error("read_png_rgba: failed to create format converter");
    }

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        throw std::runtime_error("read_png_rgba: failed to convert PNG to 32bpp RGBA: " + path);
    }

    RgbaImage img;
    img.width = static_cast<int>(width);
    img.height = static_cast<int>(height);
    const UINT row_stride = width * static_cast<UINT>(kRgbaChannels);
    const UINT buffer_size = row_stride * height;
    img.pixels.resize(buffer_size);

    hr = converter->CopyPixels(nullptr, row_stride, buffer_size, img.pixels.data());
    if (FAILED(hr)) {
        throw std::runtime_error("read_png_rgba: CopyPixels failed for " + path);
    }

    return img;
}

Bytes patch_main_control_fxf(const Bytes& encrypted_fxf) {
    const Bytes dec = charts::decrypt_fxf(encrypted_fxf);
    const std::string utf8 = cp932_to_utf8_replace(dec.data(), dec.size());

    static const std::unordered_map<std::string, std::string> kReplacements = {
        {"windowTitle", "windowTitle EXTRAVAGANZA ~The Bug-Loving Girl~ CE"},
        {"windowTitleTaiken", "windowTitleTaiken EXTRAVAGANZA ~The Bug-Loving Girl~ Trial"},
        {"mutexErrorMessage", "mutexErrorMessage The game is already running."},
        {"mutexErrorTitle", "mutexErrorTitle Already Running"},
        {"bppErrorMessage",
         "bppErrorMessage Please set display to 16-bit color or higher."},
    };

    std::string out_utf8;
    out_utf8.reserve(utf8.size() + kFxfHeaderReservePadding);

    std::size_t pos = 0;
    while (pos < utf8.size()) {
        std::size_t eol = utf8.find('\n', pos);
        const bool has_LF = (eol != std::string::npos);
        if (!has_LF) eol = utf8.size();

        std::string line = utf8.substr(pos, eol - pos);
        bool had_CR = false;
        if (!line.empty() && line.back() == '\r') {
            had_CR = true;
            line.pop_back();
        }

        const std::size_t sp = line.find(' ');
        if (sp != std::string::npos) {
            const std::string key = line.substr(0, sp);
            const auto it = kReplacements.find(key);
            if (it != kReplacements.end()) {
                line = it->second;
            }
        }

        out_utf8 += line;
        if (had_CR) out_utf8.push_back('\r');
        if (has_LF) out_utf8.push_back('\n');
        pos = eol + 1;
    }

    return charts::encrypt_fxf(utf8_to_cp932_replace(out_utf8));
}

Bytes patch_backlog_fxf(const Bytes& encrypted_fxf) {
    const Bytes dec = charts::decrypt_fxf(encrypted_fxf);
    const std::string utf8 = cp932_to_utf8_replace(dec.data(), dec.size());

    // "#青" (3-byte CP932 color command) + 8 fullwidth spaces + "Ｂａｃｋｌｏｇ"
    // (every character after "#青" is 2-byte fullwidth CP932).
    const std::string kBacklogHeaderLine =
        "firstMessage1 #青　　　　　　　　Ｂａｃｋｌｏｇ";

    std::string out_utf8;
    out_utf8.reserve(utf8.size() + kFxfBodyReservePadding);

    std::size_t pos = 0;
    while (pos < utf8.size()) {
        std::size_t eol = utf8.find('\n', pos);
        const bool has_LF = (eol != std::string::npos);
        if (!has_LF) eol = utf8.size();

        std::string line = utf8.substr(pos, eol - pos);
        bool had_CR = false;
        if (!line.empty() && line.back() == '\r') {
            had_CR = true;
            line.pop_back();
        }

        if (line.rfind("firstMessage1 ", 0) == 0) {
            line = kBacklogHeaderLine;
        }

        out_utf8 += line;
        if (had_CR) out_utf8.push_back('\r');
        if (has_LF) out_utf8.push_back('\n');
        pos = eol + 1;
    }

    return charts::encrypt_fxf(utf8_to_cp932_replace(out_utf8));
}

struct MenuArchivePatcher::Impl {
    std::uint32_t count = 0;
    std::vector<std::uint32_t> name_offsets;
    Bytes names_blob;
    std::vector<std::string> entry_names;
    std::unordered_map<std::string, std::size_t> name_to_index;
    std::vector<Bytes> entry_blobs;
};

MenuArchivePatcher::MenuArchivePatcher() : pimpl_(std::make_unique<Impl>()) {}
MenuArchivePatcher::~MenuArchivePatcher() = default;
MenuArchivePatcher::MenuArchivePatcher(MenuArchivePatcher&&) noexcept = default;
MenuArchivePatcher& MenuArchivePatcher::operator=(MenuArchivePatcher&&) noexcept = default;

MenuArchivePatcher MenuArchivePatcher::load(const std::string& gtb_path, const std::string& gpk_path) {
    return load_from_memory(read_file(gtb_path), read_file(gpk_path));
}

MenuArchivePatcher MenuArchivePatcher::load_from_memory(const Bytes& gtb_bytes, const Bytes& gpk_bytes) {
    if (gtb_bytes.size() < kGtbCountFieldSize) {
        throw std::runtime_error("MenuArchivePatcher::load_from_memory: GTB buffer too small");
    }

    const std::uint32_t count = read_u32_le(gtb_bytes.data());
    const std::size_t tables_end =
        kGtbCountFieldSize +
        static_cast<std::size_t>(count) * kGtbOffsetTableCount * kGtbOffsetElemSize;
    if (count == 0 || gtb_bytes.size() < tables_end) {
        throw std::runtime_error("MenuArchivePatcher::load_from_memory: invalid GTB entry count");
    }

    MenuArchivePatcher archive;
    auto& impl = *archive.pimpl_;
    impl.count = count;
    impl.name_offsets.resize(count);

    std::vector<std::uint32_t> data_offsets(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        impl.name_offsets[i] =
            read_u32_le(gtb_bytes.data() + kGtbCountFieldSize + i * kGtbOffsetElemSize);
        data_offsets[i] = read_u32_le(gtb_bytes.data() + kGtbCountFieldSize +
                                      (count + i) * kGtbOffsetElemSize);
    }

    impl.names_blob.assign(gtb_bytes.begin() + tables_end, gtb_bytes.end());
    impl.entry_names.resize(count);
    impl.entry_blobs.resize(count);

    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t noff = impl.name_offsets[i];
        if (noff >= impl.names_blob.size()) {
            throw std::runtime_error("MenuArchivePatcher::load_from_memory: name_offset out of range");
        }
        const char* name_ptr = reinterpret_cast<const char*>(impl.names_blob.data() + noff);
        const std::size_t max_len = impl.names_blob.size() - noff;
        const std::size_t name_len = strnlen(name_ptr, max_len);
        impl.entry_names[i].assign(name_ptr, name_len);
        impl.name_to_index[to_lower_ascii(impl.entry_names[i])] = i;

        const std::uint32_t doff = data_offsets[i];
        const std::uint32_t next_doff =
            (i + 1 < count) ? data_offsets[i + 1]
                            : static_cast<std::uint32_t>(gpk_bytes.size());
        if (doff > next_doff || next_doff > gpk_bytes.size()) {
            throw std::runtime_error("MenuArchivePatcher::load_from_memory: data_offset out of range");
        }
        impl.entry_blobs[i].assign(gpk_bytes.begin() + doff, gpk_bytes.begin() + next_doff);
    }

    return archive;
}

std::size_t MenuArchivePatcher::entry_count() const noexcept {
    return pimpl_ ? pimpl_->entry_blobs.size() : 0;
}

std::vector<GtbEntryInfo> MenuArchivePatcher::entries() const {
    std::vector<GtbEntryInfo> result;
    if (!pimpl_) return result;

    result.reserve(pimpl_->count);
    std::uint32_t cur_offset = 0;
    for (std::uint32_t i = 0; i < pimpl_->count; ++i) {
        const Bytes& blob = pimpl_->entry_blobs[i];
        GtbEntryInfo info;
        info.index = i;
        info.name = pimpl_->entry_names[i];
        info.data_offset = cur_offset;
        info.data_size = static_cast<std::uint32_t>(blob.size());
        if (blob.size() >= kDwqHeaderSize) {
            info.pack_type = static_cast<char>(blob[kDwqPackTypeOffset]);
            info.has_crop_margins =
                (blob[kDwqCropFlagOffset] == static_cast<std::uint8_t>(kCropFlagActive));
            info.width = static_cast<int>(read_i32_le(blob.data() + kDwqWidthOffset));
            info.height = static_cast<int>(read_i32_le(blob.data() + kDwqHeightOffset));
        }
        result.push_back(std::move(info));
        cur_offset += static_cast<std::uint32_t>(blob.size());
    }
    return result;
}

std::optional<std::size_t> MenuArchivePatcher::find_entry_index(const std::string& name) const {
    if (!pimpl_) return std::nullopt;
    const auto it = pimpl_->name_to_index.find(to_lower_ascii(name));
    if (it == pimpl_->name_to_index.end()) return std::nullopt;
    return it->second;
}

Bytes MenuArchivePatcher::raw_entry_bytes(std::size_t index) const {
    if (!pimpl_ || index >= pimpl_->entry_blobs.size()) {
        throw std::out_of_range("MenuArchivePatcher::raw_entry_bytes: index out of range");
    }
    return pimpl_->entry_blobs[index];
}

void MenuArchivePatcher::replace_entry_rgba(std::size_t index, const RgbaImage& img) {
    if (!pimpl_ || index >= pimpl_->entry_blobs.size()) {
        throw std::out_of_range("MenuArchivePatcher::replace_entry_rgba: index out of range");
    }
    const Bytes& orig_blob = pimpl_->entry_blobs[index];
    pimpl_->entry_blobs[index] =
        encode_dwq_from_rgba(orig_blob.data(), orig_blob.size(), img, true);
}

void MenuArchivePatcher::replace_entry_raw(std::size_t index, Bytes raw_dwq) {
    if (!pimpl_ || index >= pimpl_->entry_blobs.size()) {
        throw std::out_of_range("MenuArchivePatcher::replace_entry_raw: index out of range");
    }
    pimpl_->entry_blobs[index] = std::move(raw_dwq);
}

std::pair<Bytes, Bytes> MenuArchivePatcher::serialize() const {
    if (!pimpl_) {
        throw std::runtime_error("MenuArchivePatcher::serialize: empty archive");
    }

    const std::uint32_t count = pimpl_->count;
    const std::size_t gtb_header_bytes =
        kGtbCountFieldSize +
        static_cast<std::size_t>(count) * kGtbOffsetTableCount * kGtbOffsetElemSize;
    Bytes gtb_out(gtb_header_bytes + pimpl_->names_blob.size(), 0);
    write_u32_le(gtb_out.data(), count);

    std::size_t total_gpk_bytes = 0;
    for (const Bytes& blob : pimpl_->entry_blobs) {
        total_gpk_bytes += blob.size();
    }

    Bytes gpk_out;
    gpk_out.reserve(total_gpk_bytes);

    std::uint32_t cur_data_offset = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        write_u32_le(gtb_out.data() + kGtbCountFieldSize + i * kGtbOffsetElemSize,
                     pimpl_->name_offsets[i]);
        write_u32_le(gtb_out.data() + kGtbCountFieldSize + (count + i) * kGtbOffsetElemSize,
                     cur_data_offset);

        const Bytes& blob = pimpl_->entry_blobs[i];
        gpk_out.insert(gpk_out.end(), blob.begin(), blob.end());
        cur_data_offset += static_cast<std::uint32_t>(blob.size());
    }

    std::memcpy(gtb_out.data() + gtb_header_bytes, pimpl_->names_blob.data(),
                pimpl_->names_blob.size());
    return {std::move(gtb_out), std::move(gpk_out)};
}

void MenuArchivePatcher::save(const std::string& out_gtb_path, const std::string& out_gpk_path) const {
    const auto [gtb_bytes, gpk_bytes] = serialize();
    write_file(out_gpk_path, gpk_bytes);
    write_file(out_gtb_path, gtb_bytes);
}

std::uint32_t ComputeMurmurHash2(const std::uint8_t* data, std::size_t len) noexcept {
    std::uint32_t h = static_cast<std::uint32_t>(len);
    std::size_t i = 0;
    while (i + kWordBytes <= len) {
        std::uint32_t k = read_u32_le(data + i);
        k *= kMurmurMultiplier;
        k ^= k >> kMurmurShiftR;
        k *= kMurmurMultiplier;
        h *= kMurmurMultiplier;
        h ^= k;
        i += kWordBytes;
    }
    const std::size_t rem = len - i;
    if (rem == kRemThreeBytes) {
        h ^= static_cast<std::uint32_t>(data[i + kByteOffset2]) << kByte2Shift;
    }
    if (rem >= kRemTwoBytes) {
        h ^= static_cast<std::uint32_t>(data[i + kByteOffset1]) << kByte1Shift;
    }
    if (rem >= kRemOneByte) {
        h ^= static_cast<std::uint32_t>(data[i]);
        h *= kMurmurMultiplier;
    }
    h ^= h >> kMurmurFinalShift1;
    h *= kMurmurMultiplier;
    h ^= h >> kMurmurFinalShift2;
    return h;
}

void XorMaskPayload(std::uint8_t* buf,
                    std::size_t buf_len,
                    const std::uint8_t* orig_slice,
                    std::size_t orig_len,
                    std::uint32_t orig_data_hash) noexcept {
    if (!buf || !orig_slice || orig_len == 0) {
        return;
    }
    for (std::size_t i = 0; i < buf_len; ++i) {
        const std::uint8_t key_byte = orig_slice[i % orig_len];
        const std::uint8_t pos_mix = static_cast<std::uint8_t>(
            ((orig_data_hash >> ((i & kHashByteIndexMask) * kBitsPerByte)) & kLowByteMask) ^
            (static_cast<std::uint32_t>(i) * kXorStreamMultiplier + kXorStreamAddend));
        buf[i] ^= static_cast<std::uint8_t>(key_byte ^ pos_mix);
    }
}

ParsedPatchBin parse_patch_bin(const Bytes& patch_bin) {
    if (patch_bin.size() < kPatchHeaderSize ||
        std::memcmp(patch_bin.data(), kPatchMagic, kPatchMagicSize) != 0) {
        throw std::runtime_error("parse_patch_bin: invalid EXCEPAT1 magic or header");
    }

    ParsedPatchBin parsed;
    parsed.version = read_u32_le(patch_bin.data() + kPatchVersionOffset);
    if (parsed.version != kPatchFormatVersion) {
        throw std::runtime_error("parse_patch_bin: unsupported EXCEPAT1 version");
    }
    parsed.standalone_file_count = read_u32_le(patch_bin.data() + kPatchStandaloneCountOffset);
    parsed.orig_gpk_size = read_u32_le(patch_bin.data() + kPatchOrigGpkSizeOffset);
    parsed.new_gpk_size = read_u32_le(patch_bin.data() + kPatchNewGpkSizeOffset);
    parsed.orig_gtb_size = read_u32_le(patch_bin.data() + kPatchOrigGtbSizeOffset);
    parsed.orig_gtb_hash = read_u32_le(patch_bin.data() + kPatchOrigGtbHashOffset);
    parsed.new_gtb_size = read_u32_le(patch_bin.data() + kPatchNewGtbSizeOffset);
    parsed.new_gtb_hash = read_u32_le(patch_bin.data() + kPatchNewGtbHashOffset);
    parsed.gpk_entry_count = read_u32_le(patch_bin.data() + kPatchGpkEntryCountOffset);

    std::size_t off = kPatchHeaderSize;
    parsed.standalone_records.reserve(parsed.standalone_file_count);
    for (std::uint32_t i = 0; i < parsed.standalone_file_count; ++i) {
        if (off + kU16ByteSize > patch_bin.size()) {
            throw std::runtime_error("parse_patch_bin: truncated standalone path length");
        }
        const std::uint16_t path_len = read_u16_le(patch_bin.data() + off);
        off += kU16ByteSize;
        if (path_len == 0 || off + path_len + kStandaloneRecordMetaSize > patch_bin.size()) {
            throw std::runtime_error("parse_patch_bin: truncated standalone record header");
        }

        StandalonePatchRecord rec;
        rec.rel_path.assign(reinterpret_cast<const char*>(patch_bin.data() + off), path_len);
        off += path_len;

        rec.orig_file_size = read_u32_le(patch_bin.data() + off);
        rec.orig_data_hash = read_u32_le(patch_bin.data() + off + kStandaloneOrigHashRelOffset);
        rec.new_file_size = read_u32_le(patch_bin.data() + off + kStandaloneNewSizeRelOffset);
        rec.new_data_hash = read_u32_le(patch_bin.data() + off + kStandaloneNewHashRelOffset);
        off += kStandaloneRecordMetaSize;

        if (rec.orig_file_size == 0 || rec.new_file_size == 0 ||
            off + rec.new_file_size > patch_bin.size()) {
            throw std::runtime_error("parse_patch_bin: truncated standalone payload");
        }
        rec.masked_payload.assign(patch_bin.begin() + static_cast<std::ptrdiff_t>(off),
                                  patch_bin.begin() +
                                      static_cast<std::ptrdiff_t>(off + rec.new_file_size));
        off += rec.new_file_size;
        parsed.standalone_records.push_back(std::move(rec));
    }

    if (parsed.new_gtb_size == 0 || off + parsed.new_gtb_size > patch_bin.size()) {
        throw std::runtime_error("parse_patch_bin: truncated masked GTB section");
    }
    parsed.masked_gtb.assign(patch_bin.begin() + static_cast<std::ptrdiff_t>(off),
                             patch_bin.begin() +
                                 static_cast<std::ptrdiff_t>(off + parsed.new_gtb_size));
    off += parsed.new_gtb_size;

    parsed.gpk_records.reserve(parsed.gpk_entry_count);
    for (std::uint32_t i = 0; i < parsed.gpk_entry_count; ++i) {
        if (off + kGpkEntryRecordHeaderSize > patch_bin.size()) {
            throw std::runtime_error("parse_patch_bin: truncated GPK entry header");
        }
        GpkEntryPatchRecord rec;
        rec.entry_index = read_u32_le(patch_bin.data() + off);
        rec.orig_offset = read_u32_le(patch_bin.data() + off + kGpkRecOrigOffsetRelOffset);
        rec.orig_size = read_u32_le(patch_bin.data() + off + kGpkRecOrigSizeRelOffset);
        rec.orig_data_hash = read_u32_le(patch_bin.data() + off + kGpkRecOrigHashRelOffset);
        rec.new_size = read_u32_le(patch_bin.data() + off + kGpkRecNewSizeRelOffset);
        rec.new_data_hash = read_u32_le(patch_bin.data() + off + kGpkRecNewHashRelOffset);
        off += kGpkEntryRecordHeaderSize;

        if (rec.orig_size == 0 || rec.new_size == 0 || off + rec.new_size > patch_bin.size()) {
            throw std::runtime_error("parse_patch_bin: truncated GPK entry payload");
        }
        rec.masked_entry.assign(patch_bin.begin() + static_cast<std::ptrdiff_t>(off),
                                patch_bin.begin() +
                                    static_cast<std::ptrdiff_t>(off + rec.new_size));
        off += rec.new_size;
        parsed.gpk_records.push_back(std::move(rec));
    }

    if (off != patch_bin.size()) {
        throw std::runtime_error("parse_patch_bin: trailing bytes after GPK records");
    }
    return parsed;
}

Bytes MenuArchivePatcher::build_patch_bin_from_game_dir(const std::string& game_dir) {
    const fs::path game_root = fs::u8path(game_dir);

    const fs::path orig_gtb_path =
        resolve_original_file_path(game_root, "dwq_backup", "sys0.gtb", "dwq\\sys0.gtb");
    const fs::path orig_gpk_path =
        resolve_original_file_path(game_root, "dwq_backup", "sys0.gpk", "dwq\\sys0.gpk");
    const fs::path new_gtb_path = game_root / "dwq" / "sys0.gtb";
    const fs::path new_gpk_path = game_root / "dwq" / "sys0.gpk";

    const Bytes orig_gtb = read_file(orig_gtb_path.u8string());
    const Bytes orig_gpk = read_file(orig_gpk_path.u8string());
    const Bytes new_gtb = read_file(new_gtb_path.u8string());
    const Bytes new_gpk = read_file(new_gpk_path.u8string());

    const MenuArchivePatcher orig_arch = load_from_memory(orig_gtb, orig_gpk);
    const MenuArchivePatcher new_arch = load_from_memory(new_gtb, new_gpk);
    if (orig_arch.entry_count() != new_arch.entry_count()) {
        throw std::runtime_error(
            "MenuArchivePatcher::build_patch_bin_from_game_dir: GTB entry count mismatch");
    }

    const std::vector<GtbEntryInfo> orig_infos = orig_arch.entries();
    std::vector<GpkEntryPatchRecord> gpk_records;
    std::size_t total_gpk_payload_bytes = 0;

    for (std::size_t i = 0; i < orig_arch.entry_count(); ++i) {
        const Bytes orig_slice = orig_arch.raw_entry_bytes(i);
        const Bytes new_slice = new_arch.raw_entry_bytes(i);
        if (orig_slice == new_slice) {
            continue;
        }
        GpkEntryPatchRecord rec;
        rec.entry_index = static_cast<std::uint32_t>(i);
        rec.orig_offset = orig_infos[i].data_offset;
        rec.orig_size = static_cast<std::uint32_t>(orig_slice.size());
        rec.orig_data_hash = ComputeMurmurHash2(orig_slice.data(), orig_slice.size());
        rec.new_size = static_cast<std::uint32_t>(new_slice.size());
        rec.new_data_hash = ComputeMurmurHash2(new_slice.data(), new_slice.size());
        rec.masked_entry = new_slice;
        XorMaskPayload(rec.masked_entry.data(), rec.masked_entry.size(),
                       orig_slice.data(), orig_slice.size(), rec.orig_data_hash);
        total_gpk_payload_bytes += kGpkEntryRecordHeaderSize + rec.masked_entry.size();
        gpk_records.push_back(std::move(rec));
    }

    std::vector<StandalonePatchRecord> standalone_records;
    standalone_records.reserve(kStandaloneFiles.size());
    std::size_t total_standalone_bytes = 0;

    for (const StandaloneFileDescriptor& desc : kStandaloneFiles) {
        const fs::path orig_path = resolve_original_file_path(
            game_root, desc.backup_dir, desc.filename, desc.rel_path);
        const fs::path new_path = game_root / fs::u8path(desc.rel_path);

        const Bytes orig_bytes = read_file(orig_path.u8string());
        const Bytes new_bytes = read_file(new_path.u8string());

        StandalonePatchRecord rec;
        rec.rel_path = desc.rel_path;
        rec.orig_file_size = static_cast<std::uint32_t>(orig_bytes.size());
        rec.orig_data_hash = ComputeMurmurHash2(orig_bytes.data(), orig_bytes.size());
        rec.new_file_size = static_cast<std::uint32_t>(new_bytes.size());
        rec.new_data_hash = ComputeMurmurHash2(new_bytes.data(), new_bytes.size());
        rec.masked_payload = new_bytes;
        XorMaskPayload(rec.masked_payload.data(), rec.masked_payload.size(),
                       orig_bytes.data(), orig_bytes.size(), rec.orig_data_hash);
        total_standalone_bytes += kU16ByteSize + rec.rel_path.size() +
                                  kStandaloneRecordMetaSize + rec.masked_payload.size();
        standalone_records.push_back(std::move(rec));
    }

    const std::uint32_t orig_gtb_hash = ComputeMurmurHash2(orig_gtb.data(), orig_gtb.size());
    const std::uint32_t new_gtb_hash = ComputeMurmurHash2(new_gtb.data(), new_gtb.size());
    Bytes masked_gtb = new_gtb;
    XorMaskPayload(masked_gtb.data(), masked_gtb.size(),
                   orig_gtb.data(), orig_gtb.size(), orig_gtb_hash);

    Bytes out;
    out.reserve(kPatchHeaderSize + total_standalone_bytes + masked_gtb.size() +
                total_gpk_payload_bytes);
    out.insert(out.end(), kPatchMagic, kPatchMagic + kPatchMagicSize);
    append_u32_le(out, kPatchFormatVersion);
    append_u32_le(out, static_cast<std::uint32_t>(standalone_records.size()));
    append_u32_le(out, static_cast<std::uint32_t>(orig_gpk.size()));
    append_u32_le(out, static_cast<std::uint32_t>(new_gpk.size()));
    append_u32_le(out, static_cast<std::uint32_t>(orig_gtb.size()));
    append_u32_le(out, orig_gtb_hash);
    append_u32_le(out, static_cast<std::uint32_t>(new_gtb.size()));
    append_u32_le(out, new_gtb_hash);
    append_u32_le(out, static_cast<std::uint32_t>(gpk_records.size()));

    for (const StandalonePatchRecord& rec : standalone_records) {
        append_u16_le(out, static_cast<std::uint16_t>(rec.rel_path.size()));
        out.insert(out.end(), rec.rel_path.begin(), rec.rel_path.end());
        append_u32_le(out, rec.orig_file_size);
        append_u32_le(out, rec.orig_data_hash);
        append_u32_le(out, rec.new_file_size);
        append_u32_le(out, rec.new_data_hash);
        out.insert(out.end(), rec.masked_payload.begin(), rec.masked_payload.end());
    }

    out.insert(out.end(), masked_gtb.begin(), masked_gtb.end());

    for (const GpkEntryPatchRecord& rec : gpk_records) {
        append_u32_le(out, rec.entry_index);
        append_u32_le(out, rec.orig_offset);
        append_u32_le(out, rec.orig_size);
        append_u32_le(out, rec.orig_data_hash);
        append_u32_le(out, rec.new_size);
        append_u32_le(out, rec.new_data_hash);
        out.insert(out.end(), rec.masked_entry.begin(), rec.masked_entry.end());
    }

    return out;
}

void MenuArchivePatcher::write_patch_bin_from_game_dir(
    const std::string& game_dir, const std::string& out_patch_bin_path) {
    const Bytes patch_bin = build_patch_bin_from_game_dir(game_dir);
    write_file(out_patch_bin_path, patch_bin);
}

std::pair<Bytes, Bytes> MenuArchivePatcher::reconstruct_archive_from_patch_bin(
    const Bytes& patch_bin,
    const Bytes& orig_gtb_bytes,
    const Bytes& orig_gpk_bytes) {
    const ParsedPatchBin parsed = parse_patch_bin(patch_bin);
    if (orig_gtb_bytes.size() != parsed.orig_gtb_size ||
        orig_gpk_bytes.size() != parsed.orig_gpk_size) {
        throw std::runtime_error(
            "MenuArchivePatcher::reconstruct_archive_from_patch_bin: original size mismatch");
    }
    if (ComputeMurmurHash2(orig_gtb_bytes.data(), orig_gtb_bytes.size()) !=
        parsed.orig_gtb_hash) {
        throw std::runtime_error(
            "MenuArchivePatcher::reconstruct_archive_from_patch_bin: original GTB hash mismatch");
    }

    Bytes new_gtb = parsed.masked_gtb;
    XorMaskPayload(new_gtb.data(), new_gtb.size(),
                   orig_gtb_bytes.data(), orig_gtb_bytes.size(), parsed.orig_gtb_hash);
    if (ComputeMurmurHash2(new_gtb.data(), new_gtb.size()) != parsed.new_gtb_hash) {
        throw std::runtime_error(
            "MenuArchivePatcher::reconstruct_archive_from_patch_bin: new GTB hash mismatch");
    }

    MenuArchivePatcher arch = load_from_memory(orig_gtb_bytes, orig_gpk_bytes);
    const std::vector<GtbEntryInfo> infos = arch.entries();

    for (const GpkEntryPatchRecord& rec : parsed.gpk_records) {
        if (rec.entry_index >= arch.entry_count()) {
            throw std::runtime_error(
                "MenuArchivePatcher::reconstruct_archive_from_patch_bin: entry_index out of range");
        }
        const Bytes orig_slice = arch.raw_entry_bytes(rec.entry_index);
        if (infos[rec.entry_index].data_offset != rec.orig_offset ||
            orig_slice.size() != rec.orig_size ||
            ComputeMurmurHash2(orig_slice.data(), orig_slice.size()) != rec.orig_data_hash) {
            throw std::runtime_error(
                "MenuArchivePatcher::reconstruct_archive_from_patch_bin: entry orig hash mismatch");
        }

        Bytes unmasked = rec.masked_entry;
        XorMaskPayload(unmasked.data(), unmasked.size(),
                       orig_slice.data(), orig_slice.size(), rec.orig_data_hash);
        if (ComputeMurmurHash2(unmasked.data(), unmasked.size()) != rec.new_data_hash) {
            throw std::runtime_error(
                "MenuArchivePatcher::reconstruct_archive_from_patch_bin: entry new hash mismatch");
        }
        arch.replace_entry_raw(rec.entry_index, std::move(unmasked));
    }

    auto [rebuilt_gtb, rebuilt_gpk] = arch.serialize();
    if (rebuilt_gtb != new_gtb || rebuilt_gpk.size() != parsed.new_gpk_size) {
        throw std::runtime_error(
            "MenuArchivePatcher::reconstruct_archive_from_patch_bin: serialized archive mismatch");
    }
    return {std::move(rebuilt_gtb), std::move(rebuilt_gpk)};
}

MenuBuildSummary MenuArchivePatcher::apply_patch_bin_to_game_dir(
    const std::string& game_dir,
    const std::string& patch_bin_path,
    bool test_mode) {
    const fs::path game_root = fs::u8path(game_dir);
    const Bytes patch_bin = read_file(patch_bin_path);
    const ParsedPatchBin parsed = parse_patch_bin(patch_bin);

    const fs::path orig_gtb_path =
        resolve_original_file_path(game_root, "dwq_backup", "sys0.gtb", "dwq\\sys0.gtb");
    const fs::path orig_gpk_path =
        resolve_original_file_path(game_root, "dwq_backup", "sys0.gpk", "dwq\\sys0.gpk");
    const Bytes orig_gtb = read_file(orig_gtb_path.u8string());
    const Bytes orig_gpk = read_file(orig_gpk_path.u8string());

    auto [new_gtb, new_gpk] =
        reconstruct_archive_from_patch_bin(patch_bin, orig_gtb, orig_gpk);

    struct StagedStandaloneWrite {
        fs::path target_path;
        Bytes orig_bytes;
        Bytes new_bytes;
    };
    std::vector<StagedStandaloneWrite> standalone_outputs;
    standalone_outputs.reserve(parsed.standalone_records.size());

    for (std::size_t i = 0; i < parsed.standalone_records.size(); ++i) {
        const StandalonePatchRecord& rec = parsed.standalone_records[i];
        fs::path orig_path;
        if (i < kStandaloneFiles.size() && rec.rel_path == kStandaloneFiles[i].rel_path) {
            orig_path = resolve_original_file_path(
                game_root, kStandaloneFiles[i].backup_dir, kStandaloneFiles[i].filename,
                kStandaloneFiles[i].rel_path);
        } else {
            orig_path = resolve_original_file_path(
                game_root, "", "", rec.rel_path.c_str());
        }

        Bytes orig_bytes = read_file(orig_path.u8string());
        if (orig_bytes.size() != rec.orig_file_size ||
            ComputeMurmurHash2(orig_bytes.data(), orig_bytes.size()) != rec.orig_data_hash) {
            throw std::runtime_error("apply_patch_bin_to_game_dir: orig hash mismatch for " +
                                     rec.rel_path);
        }

        Bytes unmasked = rec.masked_payload;
        XorMaskPayload(unmasked.data(), unmasked.size(),
                       orig_bytes.data(), orig_bytes.size(), rec.orig_data_hash);
        if (ComputeMurmurHash2(unmasked.data(), unmasked.size()) != rec.new_data_hash) {
            throw std::runtime_error("apply_patch_bin_to_game_dir: new hash mismatch for " +
                                     rec.rel_path);
        }
        standalone_outputs.push_back(StagedStandaloneWrite{
            game_root / fs::u8path(rec.rel_path),
            std::move(orig_bytes),
            std::move(unmasked)});
    }

    if (!test_mode) {
        const fs::path gpk_target = game_root / "dwq" / "sys0.gpk";
        const fs::path gtb_target = game_root / "dwq" / "sys0.gtb";
        ensure_bak_backup_file(gpk_target, orig_gpk);
        ensure_bak_backup_file(gtb_target, orig_gtb);
        write_file(gpk_target.u8string(), new_gpk);
        write_file(gtb_target.u8string(), new_gtb);
        for (const StagedStandaloneWrite& item : standalone_outputs) {
            ensure_bak_backup_file(item.target_path, item.orig_bytes);
            write_file(item.target_path.u8string(), item.new_bytes);
        }
    }

    const std::uint32_t total_entries = read_u32_le(new_gtb.data());
    MenuBuildSummary summary;
    summary.total_archive_entries = total_entries;
    summary.patched_sprite_entries = parsed.gpk_entry_count;
    summary.untouched_sprite_entries = total_entries - parsed.gpk_entry_count;
    summary.patched_standalone_files = parsed.standalone_file_count;
    summary.patched_main_control_fxf = true;
    summary.patched_backlog_fxf = true;
    return summary;
}

std::string ResolveDefaultSpritesDir(const std::string& project_dir) {
    std::error_code ec;
    const fs::path proj = fs::u8path(project_dir);

    const std::array<fs::path, 2> candidates = {{
        proj / "build" / "menu_sprites",
        proj / "menu_assets" / "rendered_pngs",
    }};

    for (const fs::path& cand : candidates) {
        if (fs::is_directory(cand, ec)) {
            return cand.u8string();
        }
    }
    return candidates[0].u8string();
}

std::string default_rendered_png_dir(const std::string& project_dir) {
    return ResolveDefaultSpritesDir(project_dir);
}

MenuBuildSummary run_build_menus(const MenuBuildOptions& opt) {
    if (opt.game_dir.empty()) {
        throw std::invalid_argument("run_build_menus: game_dir is required");
    }

    std::error_code ec;
    const fs::path png_dir = fs::u8path(opt.rendered_png_dir);
    if (!fs::is_directory(png_dir, ec)) {
        if (!opt.write_patch_bin_path.empty() && !opt.test_mode) {
            log_info("Building EXCEPAT1 container directly from game directory -> " +
                     opt.write_patch_bin_path);
            MenuArchivePatcher::write_patch_bin_from_game_dir(opt.game_dir,
                                                              opt.write_patch_bin_path);
        }
        if (!opt.patch_bin_path.empty() &&
            fs::is_regular_file(fs::u8path(opt.patch_bin_path), ec)) {
            log_info("Applying keyless XOR-delta container " + opt.patch_bin_path + "...");
            return MenuArchivePatcher::apply_patch_bin_to_game_dir(
                opt.game_dir, opt.patch_bin_path, opt.test_mode);
        }
        throw std::runtime_error("run_build_menus: rendered PNG directory not found: " +
                                 opt.rendered_png_dir);
    }

    const fs::path game_root = fs::u8path(opt.game_dir);
    const fs::path backup_gtb =
        resolve_original_file_path(game_root, "dwq_backup", "sys0.gtb", "dwq\\sys0.gtb");
    const fs::path backup_gpk =
        resolve_original_file_path(game_root, "dwq_backup", "sys0.gpk", "dwq\\sys0.gpk");
    const fs::path live_gtb = game_root / "dwq" / "sys0.gtb";
    const fs::path live_gpk = game_root / "dwq" / "sys0.gpk";

    log_info("Loading dwq_backup\\sys0.gtb and dwq_backup\\sys0.gpk...");
    GpkArchive archive = GpkArchive::load(backup_gtb.u8string(), backup_gpk.u8string());

    // Collect rendered PNG files named "<NNN>_<entry_name>.png" or "<entry_name>.png".
    std::vector<fs::path> png_files;
    for (const fs::directory_entry& de : fs::directory_iterator(png_dir)) {
        if (!de.is_regular_file(ec)) continue;
        if (to_lower_ascii(de.path().extension().u8string()) == ".png") {
            png_files.push_back(de.path());
        }
    }
    std::sort(png_files.begin(), png_files.end());

    std::size_t patched_count = 0;
    for (const fs::path& png_path : png_files) {
        const std::string stem = png_path.stem().u8string();
        std::string entry_name = stem;
        if (stem.size() > kNumberedPrefixLength &&
            stem[kNumberedPrefixSeparatorIndex] == kNumberedPrefixSeparator &&
            std::isdigit(static_cast<unsigned char>(stem[0])) &&
            std::isdigit(static_cast<unsigned char>(stem[1])) &&
            std::isdigit(static_cast<unsigned char>(stem[2]))) {
            entry_name = stem.substr(kNumberedPrefixLength);
        }

        const auto idx_opt = archive.find_entry_index(entry_name);
        if (!idx_opt) {
            throw std::runtime_error("run_build_menus: unknown GTB entry name '" + entry_name +
                                     "' from file " + png_path.filename().u8string());
        }

        const RgbaImage img = read_png_rgba(png_path.u8string());
        archive.replace_entry_rgba(*idx_opt, img);
        ++patched_count;
    }

    if (patched_count != kExpectedTranslatedSpriteCount) {
        log_info("[WARN] Expected " + std::to_string(kExpectedTranslatedSpriteCount) +
                 " rendered menu PNGs, found " + std::to_string(patched_count));
    }

    MenuBuildSummary summary;
    summary.total_archive_entries = archive.entry_count();
    summary.patched_sprite_entries = patched_count;
    summary.untouched_sprite_entries = summary.total_archive_entries - patched_count;

    const fs::path main_ctrl_bak = resolve_original_file_path(
        game_root, "init2_backup", "mainControl.fxf", "init2\\mainControl.fxf");
    const fs::path main_ctrl_live = game_root / "init2" / "mainControl.fxf";
    const fs::path backlog_bak = resolve_original_file_path(
        game_root, "init2_backup", "backlog.fxf", "init2\\backlog.fxf");
    const fs::path backlog_live = game_root / "init2" / "backlog.fxf";

    const Bytes patched_main_ctrl = patch_main_control_fxf(read_file(main_ctrl_bak.u8string()));
    const Bytes patched_backlog = patch_backlog_fxf(read_file(backlog_bak.u8string()));

    if (!opt.test_mode) {
        log_info("Writing repacked dwq\\sys0.gpk and dwq\\sys0.gtb (" +
                 std::to_string(summary.patched_sprite_entries) + " translated, " +
                 std::to_string(summary.untouched_sprite_entries) + " untouched)...");
        archive.save(live_gtb.u8string(), live_gpk.u8string());

        log_info("Writing patched init2\\mainControl.fxf and init2\\backlog.fxf...");
        write_file(main_ctrl_live.u8string(), patched_main_ctrl);
        write_file(backlog_live.u8string(), patched_backlog);

        if (!opt.write_patch_bin_path.empty()) {
            log_info("Writing keyless XOR-delta container -> " + opt.write_patch_bin_path);
            MenuArchivePatcher::write_patch_bin_from_game_dir(opt.game_dir,
                                                              opt.write_patch_bin_path);
        }
    } else {
        log_info("[DRY RUN] Verified " + std::to_string(summary.patched_sprite_entries) +
                 " translated sprites and init2 FXF patches without writing files.");
    }

    summary.patched_standalone_files = kExpectedStandaloneFileCount;
    summary.patched_main_control_fxf = true;
    summary.patched_backlog_fxf = true;
    return summary;
}

}  // namespace exc::menus
