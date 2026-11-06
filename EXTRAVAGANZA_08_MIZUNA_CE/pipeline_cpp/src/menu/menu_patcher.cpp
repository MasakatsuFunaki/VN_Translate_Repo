// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/menu_patcher.h"

#include "common/util.h"
#include "xtx/xtx.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exm::menu {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {

constexpr size_t kDwqHeaderSize = 64;
constexpr size_t kDwqCropMarginsOffset = 0x10;
constexpr size_t kDwqDataSizeOffset = 0x20;
constexpr size_t kDwqWidthOffset = 0x24;
constexpr size_t kDwqHeightOffset = 0x28;
constexpr size_t kDwqBppOffset = 0x2A;
constexpr size_t kDwqPackStrOffset = 0x2C;
constexpr size_t kDwqPackStrLen = 20;
constexpr size_t kDwqPackTypeCharOffset = 0x39;
constexpr size_t kDwqCropFlagOffset = 0x3A;

constexpr size_t kCropTopIndex = 0;
constexpr size_t kCropLeftIndex = 1;
constexpr size_t kCropRightIndex = 2;
constexpr size_t kCropBottomIndex = 3;
constexpr size_t kCropTopByteOffset = 0;
constexpr size_t kCropLeftByteOffset = 4;
constexpr size_t kCropRightByteOffset = 8;
constexpr size_t kCropBottomByteOffset = 12;
constexpr int32_t kInvalidCoord = -1;

constexpr uint8_t kPackTypeUncompressed = '0';
constexpr uint8_t kPackTypeLzss = '2';
constexpr uint8_t kPackTypeIndexedRle = '3';
constexpr uint8_t kPackTypeJpeg = '5';
constexpr uint8_t kPackTypePng = '8';
constexpr uint8_t kPackAlphaFlag = 'A';

constexpr size_t kBmpHeaderSize = 54;
constexpr size_t kBmpFileSizeFieldOffset = 2;
constexpr size_t kBmpOffBitsFieldOffset = 10;
constexpr size_t kBmpWidthFieldOffset = 18;
constexpr size_t kBmpHeightFieldOffset = 22;
constexpr size_t kBmpBitCountFieldOffset = 28;
constexpr size_t kBmpSizeImageFieldOffset = 34;
constexpr size_t kBmpClrUsedFieldOffset = 46;
constexpr uint16_t kBmp8BitCount = 8;
constexpr uint16_t kBmp24BitCount = 24;
constexpr size_t kBmpRowAlignment = 4;
constexpr size_t kMaskPaletteColorCount = 256;
constexpr size_t kMaskPaletteEntrySize = 4;
constexpr size_t kMaskPaletteSize = kMaskPaletteColorCount * kMaskPaletteEntrySize;
constexpr size_t kMaskHeaderTotalSize = kBmpHeaderSize + kMaskPaletteSize;
constexpr size_t kMaskPaletteRedChannelIndex = 2;
constexpr size_t kMaxRleZeroRun = 255;
constexpr size_t kRleInitialReserveDivisor = 2;
constexpr uint8_t kBmpMagic0 = 'B';
constexpr uint8_t kBmpMagic1 = 'M';

constexpr size_t kLzssSubHeaderSize = 0x44;
constexpr size_t kLzssUncompSizeOffset = 0x10;
constexpr size_t kLzssRingSize = 0x1000;
constexpr size_t kLzssRingMask = 0x0FFF;
constexpr size_t kLzssInitRingPos = 0x0FEE;
constexpr size_t kLzssMaxMatchLen = 18;
constexpr size_t kLzssMinMatchLen = 3;
constexpr size_t kLzssBitsPerFlagByte = 8;
constexpr size_t kLzssLowNibbleMask = 0x0F;
constexpr size_t kLzssHighNibbleMask = 0xF0;
constexpr size_t kLzssHighNibbleShift = 4;
constexpr size_t kLzssReserveDivisor = 2;
constexpr uint32_t kLzssFlagReloadSentinel = 0x100u;
constexpr uint32_t kLzssFlagInitHighMask = 0xFF00u;
constexpr uint8_t kLzssLiteralFlagBit = 1;
constexpr uint8_t kByteMask = 0xFF;

constexpr uint32_t kBitsPerByte = 8;
constexpr uint32_t kByteShift1 = 8;
constexpr uint32_t kByteShift2 = 16;
constexpr uint32_t kByteShift3 = 24;
constexpr size_t kByteIndex0 = 0;
constexpr size_t kByteIndex1 = 1;
constexpr size_t kByteIndex2 = 2;
constexpr size_t kByteIndex3 = 3;

constexpr size_t kRgbaChannels = 4;
constexpr size_t kRgbChannels = 3;
constexpr size_t kRgbaRedIndex = 0;
constexpr size_t kRgbaGreenIndex = 1;
constexpr size_t kRgbaBlueIndex = 2;
constexpr size_t kRgbaAlphaIndex = 3;
constexpr size_t kBgrBlueIndex = 0;
constexpr size_t kBgrGreenIndex = 1;
constexpr size_t kBgrRedIndex = 2;
constexpr uint16_t kBpp24 = 24;
constexpr uint16_t kBpp32 = 32;
constexpr uint8_t kOpaqueAlpha = 255;

constexpr size_t kPngMinHeaderBytes = 26;
constexpr size_t kPngIhdrColorTypeOffset = 25;
constexpr uint8_t kPngMagic0 = 0x89;
constexpr uint8_t kPngMagic1 = 'P';
constexpr uint8_t kPngMagic2 = 'N';
constexpr uint8_t kPngMagic3 = 'G';
constexpr uint8_t kPngColorTypeRgb = 2;
constexpr uint8_t kPngColorTypeRgba = 6;

constexpr uint32_t kMurmurMultiplier = 0x5BD1E995u;
constexpr int kMurmurShift = 24;
constexpr int kMurmurFinalShift1 = 13;
constexpr int kMurmurFinalShift2 = 15;
constexpr size_t kMurmurBlockBytes = 4;
constexpr size_t kMurmurTailBytes3 = 3;
constexpr size_t kMurmurTailBytes2 = 2;
constexpr size_t kMurmurTailBytes1 = 1;
constexpr size_t kXorKeyBytes = 4;

constexpr uint32_t kXorStreamMultiplier = 0x9Eu;
constexpr uint32_t kXorStreamAddend = 0x37u;
constexpr size_t kHashByteIndexMask = 3u;
constexpr uint32_t kLowByteMask = 0xFFu;

constexpr size_t kPatchVersionOffset = 8;
constexpr size_t kPatchStandaloneCountOffset = 12;
constexpr size_t kPatchOrigGpkSizeOffset = 16;
constexpr size_t kPatchNewGpkSizeOffset = 20;
constexpr size_t kPatchOrigGtbSizeOffset = 24;
constexpr size_t kPatchOrigGtbHashOffset = 28;
constexpr size_t kPatchNewGtbSizeOffset = 32;
constexpr size_t kPatchNewGtbHashOffset = 36;
constexpr size_t kPatchGpkEntryCountOffset = 40;

constexpr size_t kU16ByteSize = 2;
constexpr size_t kStandaloneOrigHashRelOffset = 4;
constexpr size_t kStandaloneNewSizeRelOffset = 8;
constexpr size_t kStandaloneNewHashRelOffset = 12;

constexpr size_t kGpkRecOrigOffsetRelOffset = 4;
constexpr size_t kGpkRecOrigSizeRelOffset = 8;
constexpr size_t kGpkRecOrigHashRelOffset = 12;
constexpr size_t kGpkRecNewSizeRelOffset = 16;
constexpr size_t kGpkRecNewHashRelOffset = 20;

constexpr uint8_t kFxfXorByteMask = 0xFF;
constexpr uint8_t kAsciiCarriageReturn = '\r';
constexpr uint8_t kAsciiLineFeed = '\n';
constexpr uint8_t kAsciiTab = '\t';
constexpr uint8_t kAsciiHash = '#';
constexpr size_t kFxfReservePaddingBytes = 64;
constexpr size_t kTabColorTagLength = 4;
constexpr size_t kHashColorTagLength = 3;

constexpr const char kUtf8FullwidthSpace[] = "　";
constexpr size_t kBacklogLeadingFullwidthSpaces = 8;
constexpr const char kUtf8FullwidthBacklog[] = "Ｂａｃｋｌｏｇ";
constexpr const char kUtf8DefaultBacklogColorTag[] = "\t#苺";

constexpr size_t kGtbOffsetTableCount = 2;
constexpr size_t kExtractFilenameBufferSize = 64;
constexpr size_t kSpriteFilenameBufferSize = 128;

struct StandaloneFileDescriptor {
    const char* rel_path;
    const char* backup_dir;
    const char* filename;
};

constexpr std::array<StandaloneFileDescriptor, kExpectedStandaloneFileCount>
    kStandaloneFiles = {{
        {"spt\\charaname.xtx",     "xtx_backup",   "charaname.xtx"},
        {"init2\\mainControl.fxf", "init2_backup", "mainControl.fxf"},
        {"init2\\backlog.fxf",     "init2_backup", "backlog.fxf"},
    }};

uint16_t read_u16_le(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[kByteIndex0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[kByteIndex1]) << kBitsPerByte);
}

uint32_t read_u32_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[kByteIndex0]) |
           (static_cast<uint32_t>(p[kByteIndex1]) << kByteShift1) |
           (static_cast<uint32_t>(p[kByteIndex2]) << kByteShift2) |
           (static_cast<uint32_t>(p[kByteIndex3]) << kByteShift3);
}

int32_t read_i32_le(const uint8_t* p) noexcept {
    return static_cast<int32_t>(read_u32_le(p));
}

void write_u32_le(uint8_t* p, uint32_t v) noexcept {
    p[kByteIndex0] = static_cast<uint8_t>(v & kByteMask);
    p[kByteIndex1] = static_cast<uint8_t>((v >> kByteShift1) & kByteMask);
    p[kByteIndex2] = static_cast<uint8_t>((v >> kByteShift2) & kByteMask);
    p[kByteIndex3] = static_cast<uint8_t>((v >> kByteShift3) & kByteMask);
}

void write_i32_le(uint8_t* p, int32_t v) noexcept {
    write_u32_le(p, static_cast<uint32_t>(v));
}

void append_u16_le(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & kByteMask));
    out.push_back(static_cast<uint8_t>((v >> kBitsPerByte) & kByteMask));
}

void append_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & kByteMask));
    out.push_back(static_cast<uint8_t>((v >> kByteShift1) & kByteMask));
    out.push_back(static_cast<uint8_t>((v >> kByteShift2) & kByteMask));
    out.push_back(static_cast<uint8_t>((v >> kByteShift3) & kByteMask));
}

void write_crop_margins_le(uint8_t* dwq_header,
                           const std::array<int32_t, kCropMarginCount>& margins) noexcept {
    write_i32_le(dwq_header + kDwqCropMarginsOffset + kCropTopByteOffset,
                 margins[kCropTopIndex]);
    write_i32_le(dwq_header + kDwqCropMarginsOffset + kCropLeftByteOffset,
                 margins[kCropLeftIndex]);
    write_i32_le(dwq_header + kDwqCropMarginsOffset + kCropRightByteOffset,
                 margins[kCropRightIndex]);
    write_i32_le(dwq_header + kDwqCropMarginsOffset + kCropBottomByteOffset,
                 margins[kCropBottomIndex]);
}

std::vector<uint8_t> read_file_bytes(const fs::path& path) {
    return exm::read_file(path.u8string());
}

void write_file_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path());
    }
    exm::write_file(path.u8string(), bytes);
}

std::string read_file_string(const fs::path& path) {
    auto bytes = exm::read_file(path.u8string());
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

fs::path resolve_original_file_path(const fs::path& game_root,
                                    const char* backup_dir,
                                    const char* filename,
                                    const char* rel_path) {
    std::error_code ec;
    if (backup_dir != nullptr && backup_dir[0] != '\0') {
        if (filename != nullptr && filename[0] != '\0') {
            const fs::path backup_cand = game_root / backup_dir / filename;
            if (fs::is_regular_file(backup_cand, ec)) {
                return backup_cand;
            }
        }
        const fs::path backup_rel = game_root / backup_dir / fs::u8path(rel_path);
        if (fs::is_regular_file(backup_rel, ec)) {
            return backup_rel;
        }
    }
    const fs::path bak_cand = game_root / fs::u8path(std::string(rel_path) + ".bak");
    if (fs::is_regular_file(bak_cand, ec)) {
        return bak_cand;
    }
    const fs::path live_cand = game_root / fs::u8path(rel_path);
    if (fs::is_regular_file(live_cand, ec)) {
        return live_cand;
    }
    throw std::runtime_error(
        std::string("resolve_original_file_path: missing original file for ") + rel_path);
}

class ScopedComInit {
public:
    ScopedComInit() {
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    ~ScopedComInit() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    ScopedComInit(const ScopedComInit&) = delete;
    ScopedComInit& operator=(const ScopedComInit&) = delete;

private:
    HRESULT hr_ = E_FAIL;
};

ComPtr<IWICImagingFactory> create_wic_factory() {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        throw std::runtime_error("Failed to create IWICImagingFactory");
    }
    return factory;
}

std::vector<uint8_t> lzss_decompress(const uint8_t* src, size_t src_len, size_t out_len) {
    std::vector<uint8_t> out(out_len, 0);
    std::array<uint8_t, kLzssRingSize> ring{};
    size_t ring_pos = kLzssInitRingPos;
    size_t sp = 0;
    size_t dp = 0;
    uint32_t flags = 0;

    while (dp < out_len && sp < src_len) {
        flags >>= kLzssLiteralFlagBit;
        if ((flags & kLzssFlagReloadSentinel) == 0) {
            if (sp >= src_len) {
                break;
            }
            flags = static_cast<uint32_t>(src[sp++]) | kLzssFlagInitHighMask;
        }
        if ((flags & kLzssLiteralFlagBit) != 0) {
            if (sp >= src_len) {
                break;
            }
            uint8_t b = src[sp++];
            out[dp++] = b;
            ring[ring_pos] = b;
            ring_pos = (ring_pos + 1) & kLzssRingMask;
        } else {
            if (sp + 1 >= src_len) {
                break;
            }
            uint8_t b1 = src[sp++];
            uint8_t b2 = src[sp++];
            size_t offset = static_cast<size_t>(b1) |
                            ((static_cast<size_t>(b2) & kLzssHighNibbleMask) << kLzssHighNibbleShift);
            size_t length = (static_cast<size_t>(b2) & kLzssLowNibbleMask) + kLzssMinMatchLen;
            for (size_t k = 0; k < length && dp < out_len; ++k) {
                uint8_t b = ring[(offset + k) & kLzssRingMask];
                out[dp++] = b;
                ring[ring_pos] = b;
                ring_pos = (ring_pos + 1) & kLzssRingMask;
            }
        }
    }
    return out;
}

std::vector<uint8_t> lzss_compress(const uint8_t* src, size_t src_len) {
    std::vector<uint8_t> out;
    out.reserve(src_len / kLzssReserveDivisor + kDwqHeaderSize);

    std::array<uint8_t, kLzssRingSize> ring{};
    size_t ring_pos = kLzssInitRingPos;
    size_t sp = 0;

    while (sp < src_len) {
        size_t flag_idx = out.size();
        out.push_back(0);
        uint8_t flags = 0;

        for (size_t bit = 0; bit < kLzssBitsPerFlagByte && sp < src_len; ++bit) {
            size_t best_len = 0;
            size_t best_off = 0;
            size_t max_len = std::min(kLzssMaxMatchLen, src_len - sp);

            if (max_len >= kLzssMinMatchLen) {
                uint8_t first = src[sp];
                for (size_t off = 0; off < kLzssRingSize; ++off) {
                    if (ring[off] != first) {
                        continue;
                    }
                    size_t mlen = 1;
                    while (mlen < max_len) {
                        size_t ro = (off + mlen) & kLzssRingMask;
                        if (ro == ring_pos) {
                            break;
                        }
                        if (ring[ro] != src[sp + mlen]) {
                            break;
                        }
                        ++mlen;
                    }
                    if (mlen > best_len) {
                        best_len = mlen;
                        best_off = off;
                        if (best_len == max_len) {
                            break;
                        }
                    }
                }
            }

            if (best_len >= kLzssMinMatchLen) {
                uint8_t b1 = static_cast<uint8_t>(best_off & kByteMask);
                uint8_t b2 = static_cast<uint8_t>(
                    ((best_off >> kLzssHighNibbleShift) & kLzssHighNibbleMask) |
                    ((best_len - kLzssMinMatchLen) & kLzssLowNibbleMask));
                out.push_back(b1);
                out.push_back(b2);
                for (size_t k = 0; k < best_len; ++k) {
                    ring[ring_pos] = src[sp++];
                    ring_pos = (ring_pos + 1) & kLzssRingMask;
                }
            } else {
                flags |= static_cast<uint8_t>(kLzssLiteralFlagBit << bit);
                uint8_t b = src[sp++];
                out.push_back(b);
                ring[ring_pos] = b;
                ring_pos = (ring_pos + 1) & kLzssRingMask;
            }
        }
        out[flag_idx] = flags;
    }
    return out;
}

bool is_png_signature(const uint8_t* data, size_t size) noexcept {
    return size >= kPngMinHeaderBytes &&
           data[kByteIndex0] == kPngMagic0 &&
           data[kByteIndex1] == kPngMagic1 &&
           data[kByteIndex2] == kPngMagic2 &&
           data[kByteIndex3] == kPngMagic3;
}

bool is_orig_png_rgba(const uint8_t* orig_dwq, size_t orig_size) {
    if (orig_size >= kDwqHeaderSize + kPngMinHeaderBytes &&
        is_png_signature(orig_dwq + kDwqHeaderSize, orig_size - kDwqHeaderSize)) {
        uint8_t color_type = orig_dwq[kDwqHeaderSize + kPngIhdrColorTypeOffset];
        if (color_type == kPngColorTypeRgb) {
            return false;
        }
        if (color_type == kPngColorTypeRgba) {
            return true;
        }
    }
    uint16_t bpp = read_u16_le(orig_dwq + kDwqBppOffset);
    return bpp == kBpp32;
}

size_t bmp_row_stride_8(int width) noexcept {
    const size_t raw_row = static_cast<size_t>(width);
    return (raw_row + (kBmpRowAlignment - 1)) & ~(kBmpRowAlignment - 1);
}

size_t bmp_row_stride_24(int width) noexcept {
    const size_t raw_row = static_cast<size_t>(width) * kRgbChannels;
    return (raw_row + (kBmpRowAlignment - 1)) & ~(kBmpRowAlignment - 1);
}

bool has_embedded_bm_header(const uint8_t* dwq, size_t size) noexcept {
    return size >= kDwqHeaderSize + kBmpHeaderSize &&
           dwq[kDwqHeaderSize] == kBmpMagic0 &&
           dwq[kDwqHeaderSize + 1] == kBmpMagic1;
}

}  // namespace

std::vector<uint8_t> decode_rle_xor_mask(const uint8_t* stream, size_t stream_size,
                                         int width, int height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("decode_rle_xor_mask: invalid dimensions");
    }
    if (!stream && stream_size > 0) {
        throw std::invalid_argument("decode_rle_xor_mask: null stream pointer");
    }

    const size_t w = static_cast<size_t>(width);
    const size_t h = static_cast<size_t>(height);
    std::vector<uint8_t> out(w * h, 0);
    std::vector<uint8_t> prev(w, 0);
    std::vector<uint8_t> delta(w, 0);

    size_t pos = 0;
    for (size_t y = 0; y < h; ++y) {
        std::fill(delta.begin(), delta.end(), 0);
        size_t x = 0;
        while (x < w) {
            if (pos >= stream_size) {
                throw std::runtime_error("decode_rle_xor_mask: truncated RLE stream");
            }
            const uint8_t b = stream[pos++];
            if (b == 0) {
                if (pos >= stream_size) {
                    throw std::runtime_error("decode_rle_xor_mask: truncated zero run");
                }
                const size_t run = stream[pos++];
                if (run == 0 || x + run > w) {
                    throw std::runtime_error("decode_rle_xor_mask: zero run overflows scanline");
                }
                x += run;
                continue;
            }
            delta[x++] = b;
        }
        uint8_t* dst_row = out.data() + y * w;
        for (size_t i = 0; i < w; ++i) {
            prev[i] ^= delta[i];
            dst_row[i] = prev[i];
        }
    }
    return out;
}

std::vector<uint8_t> encode_rle_xor_mask(const uint8_t* alpha, int width, int height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("encode_rle_xor_mask: invalid dimensions");
    }
    if (!alpha) {
        throw std::invalid_argument("encode_rle_xor_mask: null alpha pointer");
    }

    const size_t w = static_cast<size_t>(width);
    const size_t h = static_cast<size_t>(height);
    std::vector<uint8_t> out;
    out.reserve(w * h / kRleInitialReserveDivisor);

    std::vector<uint8_t> prev(w, 0);
    std::vector<uint8_t> delta(w, 0);

    for (size_t y = 0; y < h; ++y) {
        const uint8_t* row = alpha + y * w;
        for (size_t i = 0; i < w; ++i) {
            delta[i] = static_cast<uint8_t>(row[i] ^ prev[i]);
            prev[i] = row[i];
        }
        size_t x = 0;
        while (x < w) {
            if (delta[x] != 0) {
                out.push_back(delta[x]);
                ++x;
                continue;
            }
            size_t run = 0;
            const size_t max_run = std::min(w - x, kMaxRleZeroRun);
            while (run < max_run && delta[x + run] == 0) {
                ++run;
            }
            out.push_back(0);
            out.push_back(static_cast<uint8_t>(run));
            x += run;
        }
    }
    return out;
}

uint32_t ComputeMurmurHash2(const uint8_t* data, size_t len, uint32_t seed) noexcept {
    uint32_t h = seed ^ static_cast<uint32_t>(len);
    while (len >= kMurmurBlockBytes) {
        uint32_t k = read_u32_le(data);
        k *= kMurmurMultiplier;
        k ^= k >> kMurmurShift;
        k *= kMurmurMultiplier;

        h *= kMurmurMultiplier;
        h ^= k;

        data += kMurmurBlockBytes;
        len -= kMurmurBlockBytes;
    }

    switch (len) {
        case kMurmurTailBytes3:
            h ^= static_cast<uint32_t>(data[kByteIndex2]) << kByteShift2;
            [[fallthrough]];
        case kMurmurTailBytes2:
            h ^= static_cast<uint32_t>(data[kByteIndex1]) << kByteShift1;
            [[fallthrough]];
        case kMurmurTailBytes1:
            h ^= static_cast<uint32_t>(data[kByteIndex0]);
            h *= kMurmurMultiplier;
            break;
        default:
            break;
    }

    h ^= h >> kMurmurFinalShift1;
    h *= kMurmurMultiplier;
    h ^= h >> kMurmurFinalShift2;
    return h;
}

void XorMaskPayload(uint8_t* data, size_t len, uint32_t key) noexcept {
    if (data == nullptr || len == 0) {
        return;
    }
    std::array<uint8_t, kXorKeyBytes> key_bytes{
        static_cast<uint8_t>(key & kByteMask),
        static_cast<uint8_t>((key >> kByteShift1) & kByteMask),
        static_cast<uint8_t>((key >> kByteShift2) & kByteMask),
        static_cast<uint8_t>((key >> kByteShift3) & kByteMask),
    };
    for (size_t i = 0; i < len; ++i) {
        data[i] ^= key_bytes[i % kXorKeyBytes];
    }
}

void XorMaskPayload(uint8_t* buf,
                    size_t buf_len,
                    const uint8_t* orig_slice,
                    size_t orig_len,
                    uint32_t orig_data_hash) noexcept {
    if (buf == nullptr || buf_len == 0 || orig_slice == nullptr || orig_len == 0) {
        return;
    }
    for (size_t i = 0; i < buf_len; ++i) {
        const uint8_t key_byte = orig_slice[i % orig_len];
        const uint8_t pos_mix = static_cast<uint8_t>(
            ((orig_data_hash >> ((i & kHashByteIndexMask) * kBitsPerByte)) & kLowByteMask) ^
            (static_cast<uint32_t>(i) * kXorStreamMultiplier + kXorStreamAddend));
        buf[i] ^= static_cast<uint8_t>(key_byte ^ pos_mix);
    }
}

ParsedPatchBin parse_patch_bin(const std::vector<uint8_t>& patch_bin) {
    if (patch_bin.size() < kPatchHeaderSize ||
        std::memcmp(patch_bin.data(), kPatchMagic, kPatchMagicSize) != 0) {
        throw std::runtime_error("parse_patch_bin: invalid MIZMENU1 magic or header");
    }

    ParsedPatchBin parsed;
    parsed.version = read_u32_le(patch_bin.data() + kPatchVersionOffset);
    if (parsed.version != kPatchFormatVersion) {
        throw std::runtime_error("parse_patch_bin: unsupported MIZMENU1 version");
    }
    parsed.standalone_file_count =
        read_u32_le(patch_bin.data() + kPatchStandaloneCountOffset);
    parsed.orig_gpk_size = read_u32_le(patch_bin.data() + kPatchOrigGpkSizeOffset);
    parsed.new_gpk_size = read_u32_le(patch_bin.data() + kPatchNewGpkSizeOffset);
    parsed.orig_gtb_size = read_u32_le(patch_bin.data() + kPatchOrigGtbSizeOffset);
    parsed.orig_gtb_hash = read_u32_le(patch_bin.data() + kPatchOrigGtbHashOffset);
    parsed.new_gtb_size = read_u32_le(patch_bin.data() + kPatchNewGtbSizeOffset);
    parsed.new_gtb_hash = read_u32_le(patch_bin.data() + kPatchNewGtbHashOffset);
    parsed.gpk_entry_count = read_u32_le(patch_bin.data() + kPatchGpkEntryCountOffset);

    size_t off = kPatchHeaderSize;
    parsed.standalone_records.reserve(parsed.standalone_file_count);
    for (uint32_t i = 0; i < parsed.standalone_file_count; ++i) {
        if (off + kU16ByteSize > patch_bin.size()) {
            throw std::runtime_error("parse_patch_bin: truncated standalone path length");
        }
        const uint16_t path_len = read_u16_le(patch_bin.data() + off);
        off += kU16ByteSize;
        if (path_len == 0 || off + path_len + kStandaloneRecordMetaSize > patch_bin.size()) {
            throw std::runtime_error("parse_patch_bin: truncated standalone record header");
        }

        StandalonePatchRecord rec;
        rec.rel_path.assign(reinterpret_cast<const char*>(patch_bin.data() + off), path_len);
        off += path_len;

        rec.orig_file_size = read_u32_le(patch_bin.data() + off);
        rec.orig_data_hash =
            read_u32_le(patch_bin.data() + off + kStandaloneOrigHashRelOffset);
        rec.new_file_size =
            read_u32_le(patch_bin.data() + off + kStandaloneNewSizeRelOffset);
        rec.new_data_hash =
            read_u32_le(patch_bin.data() + off + kStandaloneNewHashRelOffset);
        off += kStandaloneRecordMetaSize;

        if (rec.orig_file_size == 0 || rec.new_file_size == 0 ||
            off + rec.new_file_size > patch_bin.size()) {
            throw std::runtime_error("parse_patch_bin: truncated standalone payload");
        }
        rec.masked_payload.assign(
            patch_bin.begin() + static_cast<std::ptrdiff_t>(off),
            patch_bin.begin() + static_cast<std::ptrdiff_t>(off + rec.new_file_size));
        off += rec.new_file_size;
        parsed.standalone_records.push_back(std::move(rec));
    }

    if (parsed.new_gtb_size == 0 || off + parsed.new_gtb_size > patch_bin.size()) {
        throw std::runtime_error("parse_patch_bin: truncated masked GTB section");
    }
    parsed.masked_gtb.assign(
        patch_bin.begin() + static_cast<std::ptrdiff_t>(off),
        patch_bin.begin() + static_cast<std::ptrdiff_t>(off + parsed.new_gtb_size));
    off += parsed.new_gtb_size;

    parsed.gpk_records.reserve(parsed.gpk_entry_count);
    for (uint32_t i = 0; i < parsed.gpk_entry_count; ++i) {
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
        rec.masked_entry.assign(
            patch_bin.begin() + static_cast<std::ptrdiff_t>(off),
            patch_bin.begin() + static_cast<std::ptrdiff_t>(off + rec.new_size));
        off += rec.new_size;
        parsed.gpk_records.push_back(std::move(rec));
    }

    if (off != patch_bin.size()) {
        throw std::runtime_error("parse_patch_bin: trailing bytes after GPK records");
    }
    return parsed;
}

std::string decrypt_fxf(const std::vector<uint8_t>& raw) {
    std::string out(raw.size(), '\0');
    for (size_t i = 0; i < raw.size(); ++i) {
        out[i] = static_cast<char>(raw[i] ^ kFxfXorByteMask);
    }
    return out;
}

std::vector<uint8_t> encrypt_fxf(const std::string& cp932_text) {
    std::vector<uint8_t> out(cp932_text.size(), 0);
    for (size_t i = 0; i < cp932_text.size(); ++i) {
        out[i] = static_cast<uint8_t>(static_cast<uint8_t>(cp932_text[i]) ^ kFxfXorByteMask);
    }
    return out;
}

std::array<int32_t, kCropMarginCount> compute_crop_margins(const RgbaImage& img) {
    if (img.width == 0 || img.height == 0 ||
        img.pixels.size() < static_cast<size_t>(img.width) * img.height * kRgbaChannels) {
        return {0, 0, 0, 0};
    }
    int32_t min_x = img.width;
    int32_t max_x = kInvalidCoord;
    int32_t min_y = img.height;
    int32_t max_y = kInvalidCoord;

    for (int32_t y = 0; y < static_cast<int32_t>(img.height); ++y) {
        const uint8_t* row =
            img.pixels.data() + static_cast<size_t>(y) * img.width * kRgbaChannels;
        for (int32_t x = 0; x < static_cast<int32_t>(img.width); ++x) {
            if (row[static_cast<size_t>(x) * kRgbaChannels + kRgbaAlphaIndex] != 0) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }

    if (max_x < 0 || max_y < 0) {
        return {0, 0, 0, 0};
    }

    int32_t top = min_y;
    int32_t left = min_x;
    int32_t right = static_cast<int32_t>(img.width) - 1 - max_x;
    int32_t bottom = static_cast<int32_t>(img.height) - 1 - max_y;
    return {top, left, right, bottom};
}

RgbaImage decode_png_bytes_to_rgba(const uint8_t* png_data, size_t png_size) {
    if (!png_data || png_size < kPngMinHeaderBytes) {
        throw std::runtime_error("Invalid PNG buffer passed to decode_png_bytes_to_rgba");
    }
    ScopedComInit com;
    auto factory = create_wic_factory();

    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(&stream);
    if (FAILED(hr) || !stream) {
        throw std::runtime_error("Failed to create IWICStream for PNG decoding");
    }
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(png_data),
                                      static_cast<DWORD>(png_size));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to initialize IWICStream from PNG memory buffer");
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                          WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || !decoder) {
        throw std::runtime_error("Failed to create WIC PNG decoder from memory stream");
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        throw std::runtime_error("Failed to decode PNG frame 0");
    }

    UINT w = 0;
    UINT h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0) {
        throw std::runtime_error("Invalid PNG dimensions");
    }

    WICPixelFormatGUID pixel_format{};
    frame->GetPixelFormat(&pixel_format);
    const bool png_rgba_hdr =
        is_png_signature(png_data, png_size) &&
        (png_data[kPngIhdrColorTypeOffset] == kPngColorTypeRgba);
    bool has_alpha = png_rgba_hdr ||
                     (IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppBGRA) != 0) ||
                     (IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppRGBA) != 0);

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) {
        throw std::runtime_error("Failed to create WIC format converter");
    }
    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                               WICBitmapDitherTypeNone, nullptr, 0.0f,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to initialize WIC format converter to 32bppRGBA");
    }

    RgbaImage img;
    img.width = static_cast<uint16_t>(w);
    img.height = static_cast<uint16_t>(h);
    img.bpp = has_alpha ? kBpp32 : kBpp24;
    img.has_alpha = has_alpha;
    img.pixels.resize(static_cast<size_t>(w) * h * kRgbaChannels);

    UINT stride = w * static_cast<UINT>(kRgbaChannels);
    UINT buf_size = static_cast<UINT>(img.pixels.size());
    hr = converter->CopyPixels(nullptr, stride, buf_size, img.pixels.data());
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to copy decoded RGBA pixels from PNG stream");
    }
    return img;
}

std::vector<uint8_t> encode_rgba_to_png_bytes(const RgbaImage& img, bool write_alpha) {
    if (img.width == 0 || img.height == 0 ||
        img.pixels.size() < static_cast<size_t>(img.width) * img.height * kRgbaChannels) {
        throw std::runtime_error("Invalid RgbaImage passed to encode_rgba_to_png_bytes");
    }
    ScopedComInit com;
    auto factory = create_wic_factory();

    ComPtr<IStream> mem_stream;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &mem_stream);
    if (FAILED(hr) || !mem_stream) {
        throw std::runtime_error("CreateStreamOnHGlobal failed");
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr) || !encoder) {
        throw std::runtime_error("CreateEncoder(PNG) failed");
    }
    hr = encoder->Initialize(mem_stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapEncoder::Initialize failed");
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr) || !frame) {
        throw std::runtime_error("CreateNewFrame failed");
    }
    hr = frame->Initialize(props.Get());
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapFrameEncode::Initialize failed");
    }
    hr = frame->SetSize(img.width, img.height);
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapFrameEncode::SetSize failed");
    }

    if (write_alpha) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&fmt);
        if (FAILED(hr)) {
            throw std::runtime_error("IWICBitmapFrameEncode::SetPixelFormat(32bppBGRA) failed");
        }
        std::vector<uint8_t> bgra(img.pixels.size());
        size_t total_px = static_cast<size_t>(img.width) * img.height;
        for (size_t i = 0; i < total_px; ++i) {
            bgra[i * kRgbaChannels + kBgrBlueIndex] = img.pixels[i * kRgbaChannels + kRgbaBlueIndex];
            bgra[i * kRgbaChannels + kBgrGreenIndex] = img.pixels[i * kRgbaChannels + kRgbaGreenIndex];
            bgra[i * kRgbaChannels + kBgrRedIndex] = img.pixels[i * kRgbaChannels + kRgbaRedIndex];
            bgra[i * kRgbaChannels + kRgbaAlphaIndex] = img.pixels[i * kRgbaChannels + kRgbaAlphaIndex];
        }
        UINT stride = static_cast<UINT>(img.width) * static_cast<UINT>(kRgbaChannels);
        hr = frame->WritePixels(img.height, stride, static_cast<UINT>(bgra.size()),
                                bgra.data());
    } else {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        hr = frame->SetPixelFormat(&fmt);
        if (FAILED(hr)) {
            throw std::runtime_error("IWICBitmapFrameEncode::SetPixelFormat(24bppBGR) failed");
        }
        size_t stride = static_cast<size_t>(img.width) * kRgbChannels;
        std::vector<uint8_t> bgr(stride * img.height);
        size_t total_px = static_cast<size_t>(img.width) * img.height;
        for (size_t i = 0; i < total_px; ++i) {
            bgr[i * kRgbChannels + kBgrBlueIndex] = img.pixels[i * kRgbaChannels + kRgbaBlueIndex];
            bgr[i * kRgbChannels + kBgrGreenIndex] = img.pixels[i * kRgbaChannels + kRgbaGreenIndex];
            bgr[i * kRgbChannels + kBgrRedIndex] = img.pixels[i * kRgbaChannels + kRgbaRedIndex];
        }
        hr = frame->WritePixels(img.height, static_cast<UINT>(stride),
                                static_cast<UINT>(bgr.size()), bgr.data());
    }
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapFrameEncode::WritePixels failed");
    }

    hr = frame->Commit();
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapFrameEncode::Commit failed");
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapEncoder::Commit failed");
    }

    STATSTG stat{};
    hr = mem_stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr) || stat.cbSize.QuadPart == 0) {
        throw std::runtime_error("Failed to query encoded PNG stream size");
    }

    size_t out_size = static_cast<size_t>(stat.cbSize.QuadPart);
    std::vector<uint8_t> png_bytes(out_size);
    LARGE_INTEGER zero{};
    mem_stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG bytes_read = 0;
    hr = mem_stream->Read(png_bytes.data(), static_cast<ULONG>(out_size), &bytes_read);
    if (FAILED(hr) || bytes_read != out_size) {
        throw std::runtime_error("Failed to read encoded PNG bytes from memory stream");
    }
    return png_bytes;
}

std::vector<uint8_t> encode_rgba_to_jpeg_bytes(const RgbaImage& img) {
    if (img.width == 0 || img.height == 0 ||
        img.pixels.size() < static_cast<size_t>(img.width) * img.height * kRgbaChannels) {
        throw std::runtime_error("Invalid RgbaImage passed to encode_rgba_to_jpeg_bytes");
    }
    ScopedComInit com;
    auto factory = create_wic_factory();

    ComPtr<IStream> mem_stream;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &mem_stream);
    if (FAILED(hr) || !mem_stream) {
        throw std::runtime_error("CreateStreamOnHGlobal failed for JPEG");
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (FAILED(hr) || !encoder) {
        throw std::runtime_error("CreateEncoder(JPEG) failed");
    }
    hr = encoder->Initialize(mem_stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapEncoder::Initialize(JPEG) failed");
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr) || !frame) {
        throw std::runtime_error("CreateNewFrame(JPEG) failed");
    }
    hr = frame->Initialize(props.Get());
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapFrameEncode::Initialize(JPEG) failed");
    }
    hr = frame->SetSize(img.width, img.height);
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapFrameEncode::SetSize(JPEG) failed");
    }

    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    hr = frame->SetPixelFormat(&fmt);
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapFrameEncode::SetPixelFormat(24bppBGR JPEG) failed");
    }

    const size_t stride = static_cast<size_t>(img.width) * kRgbChannels;
    std::vector<uint8_t> bgr(stride * img.height);
    const size_t total_px = static_cast<size_t>(img.width) * img.height;
    for (size_t i = 0; i < total_px; ++i) {
        bgr[i * kRgbChannels + kBgrBlueIndex] = img.pixels[i * kRgbaChannels + kRgbaBlueIndex];
        bgr[i * kRgbChannels + kBgrGreenIndex] = img.pixels[i * kRgbaChannels + kRgbaGreenIndex];
        bgr[i * kRgbChannels + kBgrRedIndex] = img.pixels[i * kRgbaChannels + kRgbaRedIndex];
    }
    hr = frame->WritePixels(img.height, static_cast<UINT>(stride),
                            static_cast<UINT>(bgr.size()), bgr.data());
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapFrameEncode::WritePixels(JPEG) failed");
    }

    hr = frame->Commit();
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapFrameEncode::Commit(JPEG) failed");
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        throw std::runtime_error("IWICBitmapEncoder::Commit(JPEG) failed");
    }

    STATSTG stat{};
    hr = mem_stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr) || stat.cbSize.QuadPart == 0) {
        throw std::runtime_error("Failed to query encoded JPEG stream size");
    }

    const size_t out_size = static_cast<size_t>(stat.cbSize.QuadPart);
    std::vector<uint8_t> jpeg_bytes(out_size);
    LARGE_INTEGER zero{};
    mem_stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG bytes_read = 0;
    hr = mem_stream->Read(jpeg_bytes.data(), static_cast<ULONG>(out_size), &bytes_read);
    if (FAILED(hr) || bytes_read != out_size) {
        throw std::runtime_error("Failed to read encoded JPEG bytes from memory stream");
    }
    return jpeg_bytes;
}

RgbaImage decode_dwq_to_rgba(const uint8_t* dwq, size_t size) {
    if (!dwq || size < kDwqHeaderSize) {
        throw std::runtime_error("DWQ buffer too small (< 64 bytes)");
    }

    uint32_t data_size = read_u32_le(dwq + kDwqDataSizeOffset);
    uint16_t width = read_u16_le(dwq + kDwqWidthOffset);
    uint16_t height = read_u16_le(dwq + kDwqHeightOffset);
    uint16_t bpp = read_u16_le(dwq + kDwqBppOffset);
    uint8_t pack_char = dwq[kDwqPackTypeCharOffset];
    bool has_alpha_flag = (dwq[kDwqCropFlagOffset] == kPackAlphaFlag);

    std::array<int32_t, kCropMarginCount> crop_margins{
        read_i32_le(dwq + kDwqCropMarginsOffset + kCropTopByteOffset),
        read_i32_le(dwq + kDwqCropMarginsOffset + kCropLeftByteOffset),
        read_i32_le(dwq + kDwqCropMarginsOffset + kCropRightByteOffset),
        read_i32_le(dwq + kDwqCropMarginsOffset + kCropBottomByteOffset),
    };
    std::string pack_str(reinterpret_cast<const char*>(dwq + kDwqPackStrOffset),
                         kDwqPackStrLen);

    if (has_embedded_bm_header(dwq, size) &&
        (pack_char == kPackTypeUncompressed || pack_char == kPackTypeLzss ||
         pack_char == kPackTypeIndexedRle)) {
        const uint8_t* bmp_hdr = dwq + kDwqHeaderSize;
        const int bm_w = static_cast<int>(read_i32_le(bmp_hdr + kBmpWidthFieldOffset));
        const int bm_h = std::abs(static_cast<int>(read_i32_le(bmp_hdr + kBmpHeightFieldOffset)));
        const uint16_t bit_count = read_u16_le(bmp_hdr + kBmpBitCountFieldOffset);
        const uint32_t color_bf_off = read_u32_le(bmp_hdr + kBmpOffBitsFieldOffset);
        if (bm_w <= 0 || bm_h <= 0 || color_bf_off < kBmpHeaderSize) {
            throw std::runtime_error("decode_dwq_to_rgba: invalid BMP dimensions or bfOffBits");
        }
        RgbaImage img;
        img.width = static_cast<uint16_t>(bm_w);
        img.height = static_cast<uint16_t>(bm_h);
        img.has_alpha = has_alpha_flag;
        img.bpp = has_alpha_flag ? kBpp32 : kBpp24;
        img.pack_type = pack_str;
        img.crop_margins = crop_margins;
        img.pixels.assign(static_cast<size_t>(bm_w) * bm_h * kRgbaChannels, kOpaqueAlpha);

        if (bit_count == kBmp24BitCount &&
            (pack_char == kPackTypeUncompressed || pack_char == kPackTypeLzss)) {
            const size_t stride = bmp_row_stride_24(bm_w);
            const size_t bmp_bytes_needed = color_bf_off + stride * static_cast<size_t>(bm_h);
            if (size < kDwqHeaderSize + bmp_bytes_needed) {
                throw std::runtime_error("decode_dwq_to_rgba: truncated 24-bit BMP payload");
            }
            const uint8_t* bmp_pixels = bmp_hdr + color_bf_off;
            for (int y = 0; y < bm_h; ++y) {
                const uint8_t* src_row = bmp_pixels + static_cast<size_t>(y) * stride;
                uint8_t* dst_row =
                    img.pixels.data() + static_cast<size_t>(y) * bm_w * kRgbaChannels;
                for (int x = 0; x < bm_w; ++x) {
                    dst_row[x * kRgbaChannels + kRgbaRedIndex] =
                        src_row[x * kRgbChannels + kBgrRedIndex];
                    dst_row[x * kRgbaChannels + kRgbaGreenIndex] =
                        src_row[x * kRgbChannels + kBgrGreenIndex];
                    dst_row[x * kRgbaChannels + kRgbaBlueIndex] =
                        src_row[x * kRgbChannels + kBgrBlueIndex];
                    dst_row[x * kRgbaChannels + kRgbaAlphaIndex] = kOpaqueAlpha;
                }
            }
        } else if (bit_count == kBmp8BitCount && pack_char == kPackTypeUncompressed) {
            const size_t stride = bmp_row_stride_8(bm_w);
            const size_t bmp_bytes_needed = color_bf_off + stride * static_cast<size_t>(bm_h);
            if (size < kDwqHeaderSize + bmp_bytes_needed) {
                throw std::runtime_error("decode_dwq_to_rgba: truncated 8-bit BMP payload");
            }
            const uint8_t* color_pal = bmp_hdr + kBmpHeaderSize;
            const size_t pal_entries = (color_bf_off - kBmpHeaderSize) / kMaskPaletteEntrySize;
            const uint8_t* bmp_pixels = bmp_hdr + color_bf_off;
            for (int y = 0; y < bm_h; ++y) {
                const uint8_t* src_row = bmp_pixels + static_cast<size_t>(y) * stride;
                uint8_t* dst_row =
                    img.pixels.data() + static_cast<size_t>(y) * bm_w * kRgbaChannels;
                for (int x = 0; x < bm_w; ++x) {
                    const size_t idx = std::min<size_t>(src_row[x], pal_entries > 0 ? pal_entries - 1 : 0);
                    const uint8_t* entry = color_pal + idx * kMaskPaletteEntrySize;
                    dst_row[x * kRgbaChannels + kRgbaBlueIndex] = entry[kBgrBlueIndex];
                    dst_row[x * kRgbaChannels + kRgbaGreenIndex] = entry[kBgrGreenIndex];
                    dst_row[x * kRgbaChannels + kRgbaRedIndex] = entry[kBgrRedIndex];
                    dst_row[x * kRgbaChannels + kRgbaAlphaIndex] = kOpaqueAlpha;
                }
            }
        } else if (bit_count == kBmp8BitCount && pack_char == kPackTypeIndexedRle) {
            if (data_size <= color_bf_off ||
                size < kDwqHeaderSize + static_cast<size_t>(data_size)) {
                throw std::runtime_error("decode_dwq_to_rgba: truncated PACKTYPE=3A BMP section");
            }
            const uint8_t* color_pal = bmp_hdr + kBmpHeaderSize;
            const size_t pal_entries = (color_bf_off - kBmpHeaderSize) / kMaskPaletteEntrySize;
            const uint8_t* rle_stream = bmp_hdr + color_bf_off;
            const size_t rle_size = static_cast<size_t>(data_size) - color_bf_off;
            const auto color_indices = decode_rle_xor_mask(rle_stream, rle_size, bm_w, bm_h);
            const size_t total_px = static_cast<size_t>(bm_w) * bm_h;
            for (size_t i = 0; i < total_px; ++i) {
                const size_t idx =
                    std::min<size_t>(color_indices[i], pal_entries > 0 ? pal_entries - 1 : 0);
                const uint8_t* entry = color_pal + idx * kMaskPaletteEntrySize;
                img.pixels[i * kRgbaChannels + kRgbaBlueIndex] = entry[kBgrBlueIndex];
                img.pixels[i * kRgbaChannels + kRgbaGreenIndex] = entry[kBgrGreenIndex];
                img.pixels[i * kRgbaChannels + kRgbaRedIndex] = entry[kBgrRedIndex];
                img.pixels[i * kRgbaChannels + kRgbaAlphaIndex] = kOpaqueAlpha;
            }
        } else {
            throw std::runtime_error("decode_dwq_to_rgba: unsupported BM bit_count/pack_type");
        }

        if (pack_char == kPackTypeLzss || pack_char == kPackTypeIndexedRle) {
            const size_t mask_offset = kDwqHeaderSize + static_cast<size_t>(data_size);
            if (size < mask_offset + kBmpHeaderSize) {
                throw std::runtime_error("decode_dwq_to_rgba: truncated MASK header");
            }
            const uint8_t* mask_hdr = dwq + mask_offset;
            const uint32_t mask_bf_off = read_u32_le(mask_hdr + kBmpOffBitsFieldOffset);
            if (mask_bf_off < kBmpHeaderSize || size < mask_offset + mask_bf_off) {
                throw std::runtime_error("decode_dwq_to_rgba: truncated MASK palette");
            }
            const uint8_t* mask_palette = mask_hdr + kBmpHeaderSize;
            const size_t mask_pal_entries =
                (mask_bf_off - kBmpHeaderSize) / kMaskPaletteEntrySize;
            const uint8_t* mask_stream = mask_hdr + mask_bf_off;
            const size_t mask_stream_size = size - (mask_offset + mask_bf_off);
            const auto alpha_indices =
                decode_rle_xor_mask(mask_stream, mask_stream_size, bm_w, bm_h);
            const size_t total_px = static_cast<size_t>(bm_w) * bm_h;
            for (size_t i = 0; i < total_px; ++i) {
                const size_t idx =
                    std::min<size_t>(alpha_indices[i], mask_pal_entries > 0 ? mask_pal_entries - 1 : 0);
                img.pixels[i * kRgbaChannels + kRgbaAlphaIndex] =
                    mask_palette[idx * kMaskPaletteEntrySize + kMaskPaletteRedChannelIndex];
            }
        }
        return img;
    }

    if (pack_char == kPackTypePng || pack_char == kPackTypeJpeg) {
        if (kDwqHeaderSize + data_size > size || data_size < kPngMinHeaderBytes) {
            throw std::runtime_error("Truncated PACKTYPE=8A/5 payload in DWQ");
        }
        RgbaImage img = decode_png_bytes_to_rgba(dwq + kDwqHeaderSize, data_size);
        if (width != 0 && height != 0 &&
            (img.width != width || img.height != height)) {
            throw std::runtime_error("PACKTYPE=8A/5 dimensions do not match DWQ header");
        }
        if (bpp != 0) {
            img.bpp = bpp;
        }
        img.pack_type = pack_str;
        img.crop_margins = crop_margins;
        return img;
    }

    if (width == 0 || height == 0) {
        throw std::runtime_error("DWQ has zero width or height");
    }

    RgbaImage img;
    img.width = width;
    img.height = height;
    img.bpp = bpp;
    img.has_alpha = has_alpha_flag || (bpp == kBpp32);
    img.pack_type = pack_str;
    img.crop_margins = crop_margins;
    img.pixels.resize(static_cast<size_t>(width) * height * kRgbaChannels, kOpaqueAlpha);

    const uint8_t* payload = dwq + kDwqHeaderSize;
    size_t payload_len = size - kDwqHeaderSize;

    if (pack_char == kPackTypeUncompressed) {
        if (bpp == kBpp32) {
            size_t row_bytes = static_cast<size_t>(width) * kRgbaChannels;
            size_t needed = row_bytes * height;
            if (payload_len < needed) {
                throw std::runtime_error("Truncated PACKTYPE=0 32bpp payload");
            }
            for (uint16_t y = 0; y < height; ++y) {
                const uint8_t* src_row = payload + static_cast<size_t>(height - 1 - y) * row_bytes;
                uint8_t* dst_row =
                    img.pixels.data() + static_cast<size_t>(y) * width * kRgbaChannels;
                for (uint16_t x = 0; x < width; ++x) {
                    dst_row[x * kRgbaChannels + kRgbaRedIndex] =
                        src_row[x * kRgbaChannels + kBgrRedIndex];
                    dst_row[x * kRgbaChannels + kRgbaGreenIndex] =
                        src_row[x * kRgbaChannels + kBgrGreenIndex];
                    dst_row[x * kRgbaChannels + kRgbaBlueIndex] =
                        src_row[x * kRgbaChannels + kBgrBlueIndex];
                    dst_row[x * kRgbaChannels + kRgbaAlphaIndex] =
                        src_row[x * kRgbaChannels + kRgbaAlphaIndex];
                }
            }
        } else if (bpp == kBpp24) {
            size_t row_bytes = static_cast<size_t>(width) * kRgbChannels;
            size_t bgr_bytes = row_bytes * height;
            if (payload_len < bgr_bytes) {
                throw std::runtime_error("Truncated PACKTYPE=0 24bpp payload");
            }
            for (uint16_t y = 0; y < height; ++y) {
                const uint8_t* src_row = payload + static_cast<size_t>(height - 1 - y) * row_bytes;
                uint8_t* dst_row =
                    img.pixels.data() + static_cast<size_t>(y) * width * kRgbaChannels;
                for (uint16_t x = 0; x < width; ++x) {
                    dst_row[x * kRgbaChannels + kRgbaRedIndex] =
                        src_row[x * kRgbChannels + kBgrRedIndex];
                    dst_row[x * kRgbaChannels + kRgbaGreenIndex] =
                        src_row[x * kRgbChannels + kBgrGreenIndex];
                    dst_row[x * kRgbaChannels + kRgbaBlueIndex] =
                        src_row[x * kRgbChannels + kBgrBlueIndex];
                    dst_row[x * kRgbaChannels + kRgbaAlphaIndex] = kOpaqueAlpha;
                }
            }
            size_t alpha_bytes = static_cast<size_t>(width) * height;
            if (has_alpha_flag && payload_len >= bgr_bytes + alpha_bytes) {
                const uint8_t* alpha_plane = payload + bgr_bytes;
                for (uint16_t y = 0; y < height; ++y) {
                    const uint8_t* a_row =
                        alpha_plane + static_cast<size_t>(height - 1 - y) * width;
                    uint8_t* dst_row =
                        img.pixels.data() + static_cast<size_t>(y) * width * kRgbaChannels;
                    for (uint16_t x = 0; x < width; ++x) {
                        dst_row[x * kRgbaChannels + kRgbaAlphaIndex] = a_row[x];
                    }
                }
            }
        } else {
            throw std::runtime_error("Unsupported bpp in PACKTYPE=0");
        }
    } else if (pack_char == kPackTypeLzss) {
        if (payload_len < kLzssSubHeaderSize || data_size < kLzssSubHeaderSize ||
            data_size > payload_len) {
            throw std::runtime_error("Truncated PACKTYPE=2A LZSS sub-header");
        }
        uint32_t uncomp_size = read_u32_le(payload + kLzssUncompSizeOffset);
        size_t expected_bgr = static_cast<size_t>(width) * height * kRgbChannels;
        if (uncomp_size < expected_bgr) {
            uncomp_size = static_cast<uint32_t>(expected_bgr);
        }
        auto bgr = lzss_decompress(payload + kLzssSubHeaderSize,
                                   data_size - kLzssSubHeaderSize, uncomp_size);
        size_t row_bytes = static_cast<size_t>(width) * kRgbChannels;
        for (uint16_t y = 0; y < height; ++y) {
            const uint8_t* src_row = bgr.data() + static_cast<size_t>(height - 1 - y) * row_bytes;
            uint8_t* dst_row =
                img.pixels.data() + static_cast<size_t>(y) * width * kRgbaChannels;
            for (uint16_t x = 0; x < width; ++x) {
                dst_row[x * kRgbaChannels + kRgbaRedIndex] =
                    src_row[x * kRgbChannels + kBgrRedIndex];
                dst_row[x * kRgbaChannels + kRgbaGreenIndex] =
                    src_row[x * kRgbChannels + kBgrGreenIndex];
                dst_row[x * kRgbaChannels + kRgbaBlueIndex] =
                    src_row[x * kRgbChannels + kBgrBlueIndex];
                dst_row[x * kRgbaChannels + kRgbaAlphaIndex] = kOpaqueAlpha;
            }
        }
        size_t alpha_bytes = static_cast<size_t>(width) * height;
        if (has_alpha_flag && payload_len >= data_size + alpha_bytes) {
            const uint8_t* alpha_plane = payload + data_size;
            for (uint16_t y = 0; y < height; ++y) {
                const uint8_t* a_row =
                    alpha_plane + static_cast<size_t>(height - 1 - y) * width;
                uint8_t* dst_row =
                    img.pixels.data() + static_cast<size_t>(y) * width * kRgbaChannels;
                for (uint16_t x = 0; x < width; ++x) {
                    dst_row[x * kRgbaChannels + kRgbaAlphaIndex] = a_row[x];
                }
            }
        }
    } else {
        throw std::runtime_error("Unsupported DWQ pack type");
    }

    return img;
}

std::vector<uint8_t> encode_dwq_from_png_bytes(const uint8_t* orig_dwq, size_t orig_size,
                                               const std::vector<uint8_t>& png_bytes,
                                               bool update_crop_margins) {
    if (!orig_dwq || orig_size < kDwqHeaderSize) {
        throw std::runtime_error("Original DWQ buffer too small (< 64 bytes)");
    }
    if (orig_dwq[kDwqPackTypeCharOffset] != kPackTypePng) {
        throw std::runtime_error("encode_dwq_from_png_bytes requires PACKTYPE=8A entry");
    }
    uint16_t orig_w = read_u16_le(orig_dwq + kDwqWidthOffset);
    uint16_t orig_h = read_u16_le(orig_dwq + kDwqHeightOffset);

    RgbaImage decoded = decode_png_bytes_to_rgba(png_bytes.data(), png_bytes.size());
    if (orig_w != 0 && orig_h != 0 &&
        (decoded.width != orig_w || decoded.height != orig_h)) {
        throw std::runtime_error("Replacement PNG dimensions do not match DWQ header");
    }

    std::vector<uint8_t> out(kDwqHeaderSize + png_bytes.size());
    std::memcpy(out.data(), orig_dwq, kDwqHeaderSize);
    write_u32_le(out.data() + kDwqDataSizeOffset,
                 static_cast<uint32_t>(png_bytes.size()));

    bool has_alpha_flag = (orig_dwq[kDwqCropFlagOffset] == kPackAlphaFlag);
    uint16_t bpp = read_u16_le(orig_dwq + kDwqBppOffset);
    if (update_crop_margins && has_alpha_flag &&
        (bpp == kBpp32 || is_orig_png_rgba(orig_dwq, orig_size))) {
        auto margins = compute_crop_margins(decoded);
        write_crop_margins_le(out.data(), margins);
    }

    std::memcpy(out.data() + kDwqHeaderSize, png_bytes.data(), png_bytes.size());
    return out;
}

std::vector<uint8_t> encode_dwq_from_rgba(const uint8_t* orig_dwq, size_t orig_size,
                                          const RgbaImage& img,
                                          bool update_crop_margins) {
    if (!orig_dwq || orig_size < kDwqHeaderSize) {
        throw std::runtime_error("Original DWQ buffer too small (< 64 bytes)");
    }

    uint16_t orig_w = read_u16_le(orig_dwq + kDwqWidthOffset);
    uint16_t orig_h = read_u16_le(orig_dwq + kDwqHeightOffset);
    uint16_t bpp = read_u16_le(orig_dwq + kDwqBppOffset);
    uint8_t pack_char = orig_dwq[kDwqPackTypeCharOffset];
    bool has_alpha_flag = (orig_dwq[kDwqCropFlagOffset] == kPackAlphaFlag);

    if (orig_w != 0 && orig_h != 0 &&
        (img.width != orig_w || img.height != orig_h)) {
        throw std::runtime_error("RgbaImage dimensions do not match original DWQ header");
    }

    if (has_embedded_bm_header(orig_dwq, orig_size) &&
        (pack_char == kPackTypeUncompressed || pack_char == kPackTypeLzss ||
         pack_char == kPackTypeIndexedRle)) {
        const uint8_t* bmp_hdr = orig_dwq + kDwqHeaderSize;
        const int bm_w = static_cast<int>(read_i32_le(bmp_hdr + kBmpWidthFieldOffset));
        const int bm_h =
            std::abs(static_cast<int>(read_i32_le(bmp_hdr + kBmpHeightFieldOffset)));
        const uint16_t bit_count = read_u16_le(bmp_hdr + kBmpBitCountFieldOffset);
        const uint32_t color_bf_off = read_u32_le(bmp_hdr + kBmpOffBitsFieldOffset);
        if (bm_w != static_cast<int>(img.width) ||
            bm_h != static_cast<int>(img.height) ||
            color_bf_off < kBmpHeaderSize) {
            throw std::runtime_error(
                "encode_dwq_from_rgba: image dimensions do not match BM DWQ header");
        }

        if (bit_count == kBmp8BitCount && pack_char == kPackTypeIndexedRle) {
            const uint32_t orig_data_size = read_u32_le(orig_dwq + kDwqDataSizeOffset);
            if (orig_data_size <= color_bf_off ||
                orig_size < kDwqHeaderSize + static_cast<size_t>(orig_data_size) + kBmpHeaderSize) {
                throw std::runtime_error("encode_dwq_from_rgba: corrupt original PACKTYPE=3A");
            }
            const size_t orig_pal_entries =
                std::min<size_t>((color_bf_off - kBmpHeaderSize) / kMaskPaletteEntrySize,
                                 kMaskPaletteColorCount);
            const uint8_t* orig_color_pal = bmp_hdr + kBmpHeaderSize;
            const auto orig_color_indices = decode_rle_xor_mask(
                bmp_hdr + color_bf_off, orig_data_size - color_bf_off, bm_w, bm_h);

            const size_t mask_offset = kDwqHeaderSize + static_cast<size_t>(orig_data_size);
            const uint8_t* orig_mask_hdr = orig_dwq + mask_offset;
            const uint32_t orig_mask_bf_off = read_u32_le(orig_mask_hdr + kBmpOffBitsFieldOffset);
            if (orig_mask_bf_off < kBmpHeaderSize ||
                orig_size < mask_offset + orig_mask_bf_off) {
                throw std::runtime_error("encode_dwq_from_rgba: corrupt PACKTYPE=3A mask");
            }
            const uint8_t* orig_mask_pal = orig_mask_hdr + kBmpHeaderSize;
            const size_t orig_mask_pal_entries =
                (orig_mask_bf_off - kBmpHeaderSize) / kMaskPaletteEntrySize;
            const auto orig_mask_indices = decode_rle_xor_mask(
                orig_mask_hdr + orig_mask_bf_off,
                orig_size - (mask_offset + orig_mask_bf_off),
                bm_w, bm_h);

            const size_t total_px = static_cast<size_t>(bm_w) * bm_h;
            std::vector<uint8_t> orig_alpha(total_px, 0);
            for (size_t i = 0; i < total_px; ++i) {
                const size_t idx = std::min<size_t>(
                    orig_mask_indices[i],
                    orig_mask_pal_entries > 0 ? orig_mask_pal_entries - 1 : 0);
                orig_alpha[i] =
                    orig_mask_pal[idx * kMaskPaletteEntrySize + kMaskPaletteRedChannelIndex];
            }

            std::array<uint8_t, kMaskPaletteSize> new_pal{};
            std::memcpy(new_pal.data(), orig_color_pal,
                        orig_pal_entries * kMaskPaletteEntrySize);
            size_t pal_count = orig_pal_entries;

            auto find_exact_color = [&](uint8_t r, uint8_t g, uint8_t b) -> int {
                for (size_t p = 0; p < pal_count; ++p) {
                    if (new_pal[p * kMaskPaletteEntrySize + kBgrBlueIndex] == b &&
                        new_pal[p * kMaskPaletteEntrySize + kBgrGreenIndex] == g &&
                        new_pal[p * kMaskPaletteEntrySize + kBgrRedIndex] == r) {
                        return static_cast<int>(p);
                    }
                }
                return -1;
            };

            auto find_nearest_color = [&](uint8_t r, uint8_t g, uint8_t b) -> uint8_t {
                size_t best_idx = 0;
                int best_dist = std::numeric_limits<int>::max();
                const size_t limit = pal_count > 0 ? pal_count : 1;
                for (size_t p = 0; p < limit; ++p) {
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
                        if (dist == 0) break;
                    }
                }
                return static_cast<uint8_t>(best_idx);
            };

            std::vector<uint8_t> new_color_indices(total_px, 0);
            std::vector<uint8_t> new_alpha(total_px, 0);
            for (size_t i = 0; i < total_px; ++i) {
                const uint8_t r = img.pixels[i * kRgbaChannels + kRgbaRedIndex];
                const uint8_t g = img.pixels[i * kRgbaChannels + kRgbaGreenIndex];
                const uint8_t b = img.pixels[i * kRgbaChannels + kRgbaBlueIndex];
                const uint8_t a = img.pixels[i * kRgbaChannels + kRgbaAlphaIndex];
                new_alpha[i] = a;

                if (a == 0 && orig_alpha[i] == 0) {
                    const uint8_t orig_idx = orig_color_indices[i];
                    if (orig_idx < pal_count &&
                        ((r == 0 && g == 0 && b == 0) ||
                         (new_pal[orig_idx * kMaskPaletteEntrySize + kBgrRedIndex] == r &&
                          new_pal[orig_idx * kMaskPaletteEntrySize + kBgrGreenIndex] == g &&
                          new_pal[orig_idx * kMaskPaletteEntrySize + kBgrBlueIndex] == b))) {
                        new_color_indices[i] = orig_idx;
                        continue;
                    }
                }

                const uint8_t orig_idx = orig_color_indices[i];
                if (orig_idx < pal_count &&
                    new_pal[orig_idx * kMaskPaletteEntrySize + kBgrRedIndex] == r &&
                    new_pal[orig_idx * kMaskPaletteEntrySize + kBgrGreenIndex] == g &&
                    new_pal[orig_idx * kMaskPaletteEntrySize + kBgrBlueIndex] == b) {
                    new_color_indices[i] = orig_idx;
                    continue;
                }

                int exact = find_exact_color(r, g, b);
                if (exact >= 0) {
                    new_color_indices[i] = static_cast<uint8_t>(exact);
                } else if (pal_count < kMaskPaletteColorCount) {
                    const size_t idx = pal_count++;
                    new_pal[idx * kMaskPaletteEntrySize + kBgrBlueIndex] = b;
                    new_pal[idx * kMaskPaletteEntrySize + kBgrGreenIndex] = g;
                    new_pal[idx * kMaskPaletteEntrySize + kBgrRedIndex] = r;
                    new_pal[idx * kMaskPaletteEntrySize + kRgbaAlphaIndex] = 0;
                    new_color_indices[i] = static_cast<uint8_t>(idx);
                } else {
                    new_color_indices[i] = find_nearest_color(r, g, b);
                }
            }

            const auto encoded_color =
                encode_rle_xor_mask(new_color_indices.data(), bm_w, bm_h);
            const auto encoded_mask =
                encode_rle_xor_mask(new_alpha.data(), bm_w, bm_h);

            const size_t color_pal_bytes = pal_count * kMaskPaletteEntrySize;
            const uint32_t new_color_bf_off =
                static_cast<uint32_t>(kBmpHeaderSize + color_pal_bytes);
            const uint32_t new_data_size =
                new_color_bf_off + static_cast<uint32_t>(encoded_color.size());

            std::vector<uint8_t> out(orig_dwq, orig_dwq + kDwqHeaderSize);
            write_u32_le(out.data() + kDwqDataSizeOffset, new_data_size);
            if (update_crop_margins && has_alpha_flag) {
                auto margins = compute_crop_margins(img);
                write_crop_margins_le(out.data(), margins);
            }

            std::vector<uint8_t> color_bmp_hdr(bmp_hdr, bmp_hdr + kBmpHeaderSize);
            write_u32_le(color_bmp_hdr.data() + kBmpOffBitsFieldOffset, new_color_bf_off);
            write_u32_le(color_bmp_hdr.data() + kBmpClrUsedFieldOffset,
                         pal_count == kMaskPaletteColorCount ? 0u : static_cast<uint32_t>(pal_count));
            out.insert(out.end(), color_bmp_hdr.begin(), color_bmp_hdr.end());
            out.insert(out.end(), new_pal.begin(),
                       new_pal.begin() + static_cast<std::ptrdiff_t>(color_pal_bytes));
            out.insert(out.end(), encoded_color.begin(), encoded_color.end());

            std::vector<uint8_t> mask_bmp_hdr(orig_mask_hdr, orig_mask_hdr + kBmpHeaderSize);
            write_u32_le(mask_bmp_hdr.data() + kBmpOffBitsFieldOffset,
                         static_cast<uint32_t>(kMaskHeaderTotalSize));
            write_u32_le(mask_bmp_hdr.data() + kBmpClrUsedFieldOffset, 0u);
            out.insert(out.end(), mask_bmp_hdr.begin(), mask_bmp_hdr.end());
            for (size_t i = 0; i < kMaskPaletteColorCount; ++i) {
                const auto gray = static_cast<uint8_t>(i);
                out.push_back(gray);
                out.push_back(gray);
                out.push_back(gray);
                out.push_back(0);
            }
            out.insert(out.end(), encoded_mask.begin(), encoded_mask.end());
            return out;
        }

        if (bit_count != kBmp24BitCount) {
            throw std::runtime_error(
                "encode_dwq_from_rgba: unsupported BM bit_count for PACKTYPE=0/2A");
        }
        const size_t stride = bmp_row_stride_24(img.width);
        const size_t bmp_section_size =
            (pack_char == kPackTypeLzss)
                ? static_cast<size_t>(read_u32_le(orig_dwq + kDwqDataSizeOffset))
                : (orig_size - kDwqHeaderSize);
        if (bmp_section_size < color_bf_off + stride * static_cast<size_t>(img.height) ||
            orig_size < kDwqHeaderSize + bmp_section_size) {
            throw std::runtime_error("encode_dwq_from_rgba: corrupt original BM section size");
        }

        std::vector<uint8_t> orig_alpha;
        if (pack_char == kPackTypeLzss) {
            const size_t mask_offset = kDwqHeaderSize + bmp_section_size;
            if (orig_size < mask_offset + kBmpHeaderSize) {
                throw std::runtime_error("encode_dwq_from_rgba: corrupt original MASK section");
            }
            const uint8_t* orig_mask_hdr = orig_dwq + mask_offset;
            const uint32_t orig_mask_bf_off =
                read_u32_le(orig_mask_hdr + kBmpOffBitsFieldOffset);
            if (orig_mask_bf_off < kBmpHeaderSize ||
                orig_size < mask_offset + orig_mask_bf_off) {
                throw std::runtime_error("encode_dwq_from_rgba: corrupt original MASK palette");
            }
            const uint8_t* orig_mask_palette = orig_mask_hdr + kBmpHeaderSize;
            const size_t orig_mask_pal_entries =
                (orig_mask_bf_off - kBmpHeaderSize) / kMaskPaletteEntrySize;
            const auto orig_indices = decode_rle_xor_mask(
                orig_mask_hdr + orig_mask_bf_off,
                orig_size - (mask_offset + orig_mask_bf_off),
                img.width, img.height);
            orig_alpha.resize(orig_indices.size());
            for (size_t i = 0; i < orig_indices.size(); ++i) {
                const size_t idx = std::min<size_t>(
                    orig_indices[i],
                    orig_mask_pal_entries > 0 ? orig_mask_pal_entries - 1 : 0);
                orig_alpha[i] =
                    orig_mask_palette[idx * kMaskPaletteEntrySize + kMaskPaletteRedChannelIndex];
            }
        }

        std::vector<uint8_t> out(orig_dwq, orig_dwq + kDwqHeaderSize + bmp_section_size);
        if (update_crop_margins && has_alpha_flag) {
            auto margins = compute_crop_margins(img);
            write_crop_margins_le(out.data(), margins);
        }

        uint8_t* dst_bmp_pixels = out.data() + kDwqHeaderSize + color_bf_off;
        const uint8_t* orig_bmp_pixels = orig_dwq + kDwqHeaderSize + color_bf_off;
        for (uint16_t y = 0; y < img.height; ++y) {
            uint8_t* dst_row = dst_bmp_pixels + static_cast<size_t>(y) * stride;
            const uint8_t* orig_row = orig_bmp_pixels + static_cast<size_t>(y) * stride;
            const uint8_t* src_row =
                img.pixels.data() + static_cast<size_t>(y) * img.width * kRgbaChannels;
            for (uint16_t x = 0; x < img.width; ++x) {
                const uint8_t r = src_row[x * kRgbaChannels + kRgbaRedIndex];
                const uint8_t g = src_row[x * kRgbaChannels + kRgbaGreenIndex];
                const uint8_t b = src_row[x * kRgbaChannels + kRgbaBlueIndex];
                const uint8_t a = src_row[x * kRgbaChannels + kRgbaAlphaIndex];
                const size_t px_idx = static_cast<size_t>(y) * img.width + x;

                if (pack_char == kPackTypeLzss && a == 0 && orig_alpha[px_idx] == 0 &&
                    r == 0 && g == 0 && b == 0) {
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

        if (pack_char == kPackTypeLzss) {
            const size_t mask_offset = kDwqHeaderSize + bmp_section_size;
            std::vector<uint8_t> mask_bmp_hdr(
                orig_dwq + mask_offset, orig_dwq + mask_offset + kBmpHeaderSize);
            write_u32_le(mask_bmp_hdr.data() + kBmpOffBitsFieldOffset,
                         static_cast<uint32_t>(kMaskHeaderTotalSize));
            write_u32_le(mask_bmp_hdr.data() + kBmpClrUsedFieldOffset, 0u);
            out.insert(out.end(), mask_bmp_hdr.begin(), mask_bmp_hdr.end());
            for (size_t i = 0; i < kMaskPaletteColorCount; ++i) {
                const auto gray = static_cast<uint8_t>(i);
                out.push_back(gray);
                out.push_back(gray);
                out.push_back(gray);
                out.push_back(0);
            }
            const size_t total_px = static_cast<size_t>(img.width) * img.height;
            std::vector<uint8_t> new_alpha(total_px, 0);
            for (size_t i = 0; i < total_px; ++i) {
                new_alpha[i] = img.pixels[i * kRgbaChannels + kRgbaAlphaIndex];
            }
            const auto encoded_mask =
                encode_rle_xor_mask(new_alpha.data(), img.width, img.height);
            out.insert(out.end(), encoded_mask.begin(), encoded_mask.end());
        }
        return out;
    }

    if (pack_char == kPackTypePng) {
        bool write_alpha = is_orig_png_rgba(orig_dwq, orig_size);
        auto png_bytes = encode_rgba_to_png_bytes(img, write_alpha);

        std::vector<uint8_t> out(kDwqHeaderSize + png_bytes.size());
        std::memcpy(out.data(), orig_dwq, kDwqHeaderSize);
        write_u32_le(out.data() + kDwqDataSizeOffset,
                     static_cast<uint32_t>(png_bytes.size()));

        if (update_crop_margins && has_alpha_flag && write_alpha) {
            auto margins = compute_crop_margins(img);
            write_crop_margins_le(out.data(), margins);
        }

        std::memcpy(out.data() + kDwqHeaderSize, png_bytes.data(), png_bytes.size());
        return out;
    }

    if (pack_char == kPackTypeJpeg) {
        auto jpeg_bytes = encode_rgba_to_jpeg_bytes(img);

        std::vector<uint8_t> out(kDwqHeaderSize + jpeg_bytes.size());
        std::memcpy(out.data(), orig_dwq, kDwqHeaderSize);
        write_u32_le(out.data() + kDwqDataSizeOffset,
                     static_cast<uint32_t>(jpeg_bytes.size()));
        std::memcpy(out.data() + kDwqHeaderSize, jpeg_bytes.data(), jpeg_bytes.size());
        return out;
    }

    std::vector<uint8_t> out;
    out.resize(kDwqHeaderSize);
    std::memcpy(out.data(), orig_dwq, kDwqHeaderSize);

    if (update_crop_margins && has_alpha_flag && bpp == kBpp32) {
        auto margins = compute_crop_margins(img);
        write_crop_margins_le(out.data(), margins);
    }

    if (pack_char == kPackTypeUncompressed) {
        if (bpp == kBpp32) {
            size_t row_bytes = static_cast<size_t>(img.width) * kRgbaChannels;
            out.resize(kDwqHeaderSize + row_bytes * img.height);
            uint8_t* dst = out.data() + kDwqHeaderSize;
            for (uint16_t y = 0; y < img.height; ++y) {
                const uint8_t* src_row =
                    img.pixels.data() + static_cast<size_t>(img.height - 1 - y) * row_bytes;
                uint8_t* dst_row = dst + static_cast<size_t>(y) * row_bytes;
                for (uint16_t x = 0; x < img.width; ++x) {
                    dst_row[x * kRgbaChannels + kBgrBlueIndex] =
                        src_row[x * kRgbaChannels + kRgbaBlueIndex];
                    dst_row[x * kRgbaChannels + kBgrGreenIndex] =
                        src_row[x * kRgbaChannels + kRgbaGreenIndex];
                    dst_row[x * kRgbaChannels + kBgrRedIndex] =
                        src_row[x * kRgbaChannels + kRgbaRedIndex];
                    dst_row[x * kRgbaChannels + kRgbaAlphaIndex] =
                        src_row[x * kRgbaChannels + kRgbaAlphaIndex];
                }
            }
            write_u32_le(out.data() + kDwqDataSizeOffset,
                         static_cast<uint32_t>(row_bytes * img.height));
        } else if (bpp == kBpp24) {
            size_t row_bytes = static_cast<size_t>(img.width) * kRgbChannels;
            size_t bgr_bytes = row_bytes * img.height;
            size_t alpha_bytes =
                has_alpha_flag ? (static_cast<size_t>(img.width) * img.height) : 0;
            out.resize(kDwqHeaderSize + bgr_bytes + alpha_bytes);
            uint8_t* dst = out.data() + kDwqHeaderSize;
            for (uint16_t y = 0; y < img.height; ++y) {
                const uint8_t* src_row = img.pixels.data() +
                                         static_cast<size_t>(img.height - 1 - y) * img.width *
                                             kRgbaChannels;
                uint8_t* dst_row = dst + static_cast<size_t>(y) * row_bytes;
                for (uint16_t x = 0; x < img.width; ++x) {
                    dst_row[x * kRgbChannels + kBgrBlueIndex] =
                        src_row[x * kRgbaChannels + kRgbaBlueIndex];
                    dst_row[x * kRgbChannels + kBgrGreenIndex] =
                        src_row[x * kRgbaChannels + kRgbaGreenIndex];
                    dst_row[x * kRgbChannels + kBgrRedIndex] =
                        src_row[x * kRgbaChannels + kRgbaRedIndex];
                }
            }
            if (has_alpha_flag) {
                uint8_t* a_dst = dst + bgr_bytes;
                for (uint16_t y = 0; y < img.height; ++y) {
                    const uint8_t* src_row = img.pixels.data() +
                                             static_cast<size_t>(img.height - 1 - y) * img.width *
                                                 kRgbaChannels;
                    uint8_t* a_row = a_dst + static_cast<size_t>(y) * img.width;
                    for (uint16_t x = 0; x < img.width; ++x) {
                        a_row[x] = src_row[x * kRgbaChannels + kRgbaAlphaIndex];
                    }
                }
            }
            write_u32_le(out.data() + kDwqDataSizeOffset, static_cast<uint32_t>(bgr_bytes));
        } else {
            throw std::runtime_error("Unsupported bpp in PACKTYPE=0 encode");
        }
    } else if (pack_char == kPackTypeLzss) {
        if (orig_size < kDwqHeaderSize + kLzssSubHeaderSize) {
            throw std::runtime_error("Original PACKTYPE=2A DWQ missing sub-header");
        }
        size_t row_bytes = static_cast<size_t>(img.width) * kRgbChannels;
        std::vector<uint8_t> bgr(row_bytes * img.height);
        for (uint16_t y = 0; y < img.height; ++y) {
            const uint8_t* src_row = img.pixels.data() +
                                     static_cast<size_t>(img.height - 1 - y) * img.width *
                                         kRgbaChannels;
            uint8_t* dst_row = bgr.data() + static_cast<size_t>(y) * row_bytes;
            for (uint16_t x = 0; x < img.width; ++x) {
                dst_row[x * kRgbChannels + kBgrBlueIndex] =
                    src_row[x * kRgbaChannels + kRgbaBlueIndex];
                dst_row[x * kRgbChannels + kBgrGreenIndex] =
                    src_row[x * kRgbaChannels + kRgbaGreenIndex];
                dst_row[x * kRgbChannels + kBgrRedIndex] =
                    src_row[x * kRgbaChannels + kRgbaRedIndex];
            }
        }
        auto comp = lzss_compress(bgr.data(), bgr.size());
        uint32_t new_data_size = static_cast<uint32_t>(kLzssSubHeaderSize + comp.size());
        write_u32_le(out.data() + kDwqDataSizeOffset, new_data_size);

        size_t alpha_bytes =
            has_alpha_flag ? (static_cast<size_t>(img.width) * img.height) : 0;
        out.resize(kDwqHeaderSize + new_data_size + alpha_bytes);

        uint8_t* sub_hdr = out.data() + kDwqHeaderSize;
        std::memcpy(sub_hdr, orig_dwq + kDwqHeaderSize, kLzssSubHeaderSize);
        write_u32_le(sub_hdr + kLzssUncompSizeOffset, static_cast<uint32_t>(bgr.size()));
        write_u32_le(sub_hdr + kDwqDataSizeOffset, static_cast<uint32_t>(comp.size()));
        std::memcpy(sub_hdr + kLzssSubHeaderSize, comp.data(), comp.size());

        if (has_alpha_flag) {
            uint8_t* a_dst = out.data() + kDwqHeaderSize + new_data_size;
            for (uint16_t y = 0; y < img.height; ++y) {
                const uint8_t* src_row = img.pixels.data() +
                                         static_cast<size_t>(img.height - 1 - y) * img.width *
                                             kRgbaChannels;
                uint8_t* a_row = a_dst + static_cast<size_t>(y) * img.width;
                for (uint16_t x = 0; x < img.width; ++x) {
                    a_row[x] = src_row[x * kRgbaChannels + kRgbaAlphaIndex];
                }
            }
        }
    } else {
        throw std::runtime_error("Unsupported DWQ pack type in encode_dwq_from_rgba");
    }

    return out;
}

RgbaImage read_png_rgba(const fs::path& path) {
    auto bytes = read_file_bytes(path);
    return decode_png_bytes_to_rgba(bytes.data(), bytes.size());
}

void write_png_rgba(const fs::path& path, const RgbaImage& img) {
    bool write_alpha = (img.bpp == kBpp32) || img.has_alpha;
    auto bytes = encode_rgba_to_png_bytes(img, write_alpha);
    write_file_bytes(path, bytes);
}

std::string patch_main_control_fxf(const std::string& cp932_text) {
    std::string out;
    out.reserve(cp932_text.size() + kFxfReservePaddingBytes);

    size_t pos = 0;
    while (pos < cp932_text.size()) {
        size_t eol = cp932_text.find(static_cast<char>(kAsciiLineFeed), pos);
        std::string line = (eol == std::string::npos)
                               ? cp932_text.substr(pos)
                               : cp932_text.substr(pos, eol - pos + 1);
        pos = (eol == std::string::npos) ? cp932_text.size() : (eol + 1);

        std::string ending;
        while (!line.empty() &&
               (line.back() == static_cast<char>(kAsciiCarriageReturn) ||
                line.back() == static_cast<char>(kAsciiLineFeed))) {
            ending.insert(ending.begin(), line.back());
            line.pop_back();
        }

        if (line.rfind("windowTitle ", 0) == 0) {
            line = "windowTitle EXTRA VA MIZUNA ~EXTRAVAGANZA~ CE";
        } else if (line.rfind("windowTitleTaiken ", 0) == 0) {
            line = "windowTitleTaiken EXTRA VA MIZUNA ~EXTRAVAGANZA~ CE Trial";
        } else if (line.rfind("bppErrorMessage ", 0) == 0) {
            line = "bppErrorMessage Please set display to 16-bit color or higher.";
        } else if (line.rfind("mutexErrorMessage ", 0) == 0) {
            line = "mutexErrorMessage The game is already running.";
        } else if (line.rfind("mutexErrorTitle ", 0) == 0) {
            line = "mutexErrorTitle Already Running";
        }

        out.append(line);
        out.append(ending);
    }
    return out;
}

std::string patch_backlog_fxf(const std::string& cp932_text) {
    const std::string cp932_fullwidth_space = exm::utf8_to_cp932(kUtf8FullwidthSpace);
    const std::string cp932_fullwidth_backlog = exm::utf8_to_cp932(kUtf8FullwidthBacklog);
    const std::string cp932_default_color_tag =
        exm::utf8_to_cp932(kUtf8DefaultBacklogColorTag);

    std::string out;
    out.reserve(cp932_text.size() + kFxfReservePaddingBytes);

    size_t pos = 0;
    while (pos < cp932_text.size()) {
        size_t eol = cp932_text.find(static_cast<char>(kAsciiLineFeed), pos);
        std::string line = (eol == std::string::npos)
                               ? cp932_text.substr(pos)
                               : cp932_text.substr(pos, eol - pos + 1);
        pos = (eol == std::string::npos) ? cp932_text.size() : (eol + 1);

        std::string ending;
        while (!line.empty() &&
               (line.back() == static_cast<char>(kAsciiCarriageReturn) ||
                line.back() == static_cast<char>(kAsciiLineFeed))) {
            ending.insert(ending.begin(), line.back());
            line.pop_back();
        }

        if (line.rfind("firstMessage1 ", 0) == 0) {
            std::string val = line.substr(std::strlen("firstMessage1 "));
            std::string color_prefix = cp932_default_color_tag;
            if (val.size() >= kTabColorTagLength &&
                val[kByteIndex0] == static_cast<char>(kAsciiTab) &&
                val[kByteIndex1] == static_cast<char>(kAsciiHash)) {
                color_prefix = val.substr(0, kTabColorTagLength);
            } else if (val.size() >= kHashColorTagLength &&
                       val[kByteIndex0] == static_cast<char>(kAsciiHash)) {
                color_prefix = val.substr(0, kHashColorTagLength);
            }

            std::string spaces;
            for (size_t i = 0; i < kBacklogLeadingFullwidthSpaces; ++i) {
                spaces.append(cp932_fullwidth_space);
            }
            line = "firstMessage1 " + color_prefix + spaces + cp932_fullwidth_backlog;
        }

        out.append(line);
        out.append(ending);
    }
    return out;
}

struct MenuArchivePatcher::Impl {
    std::vector<uint32_t> name_offsets;
    std::vector<uint8_t> names_blob;
    std::vector<std::string> names;
    std::vector<std::vector<uint8_t>> entries;
    std::vector<uint32_t> offsets;

    void recompute_offsets() {
        offsets.resize(entries.size() + 1, 0);
        uint32_t cur = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            offsets[i] = cur;
            cur += static_cast<uint32_t>(entries[i].size());
        }
        offsets[entries.size()] = cur;
    }

    void check_index(size_t index) const {
        if (index >= entries.size()) {
            throw std::out_of_range("MenuArchivePatcher entry index out of range");
        }
    }
};

MenuArchivePatcher::MenuArchivePatcher() : pimpl_(std::make_unique<Impl>()) {}
MenuArchivePatcher::~MenuArchivePatcher() = default;
MenuArchivePatcher::MenuArchivePatcher(MenuArchivePatcher&&) noexcept = default;
MenuArchivePatcher& MenuArchivePatcher::operator=(MenuArchivePatcher&&) noexcept = default;

void MenuArchivePatcher::load_from_memory(const std::vector<uint8_t>& gpk_bytes,
                                          const std::vector<uint8_t>& gtb_bytes) {
    if (gtb_bytes.size() < sizeof(uint32_t)) {
        throw std::runtime_error("GTB file too small");
    }
    uint32_t count = read_u32_le(gtb_bytes.data());
    size_t tables_end =
        sizeof(uint32_t) + static_cast<size_t>(count) * kGtbOffsetTableCount * sizeof(uint32_t);
    if (gtb_bytes.size() < tables_end) {
        throw std::runtime_error("GTB file truncated for entry count");
    }

    pimpl_->name_offsets.resize(count);
    std::vector<uint32_t> data_offsets(count);
    for (uint32_t i = 0; i < count; ++i) {
        pimpl_->name_offsets[i] =
            read_u32_le(gtb_bytes.data() + sizeof(uint32_t) + static_cast<size_t>(i) * sizeof(uint32_t));
        data_offsets[i] = read_u32_le(
            gtb_bytes.data() + sizeof(uint32_t) + static_cast<size_t>(count + i) * sizeof(uint32_t));
    }

    pimpl_->names_blob.assign(gtb_bytes.begin() + static_cast<std::ptrdiff_t>(tables_end),
                              gtb_bytes.end());
    pimpl_->names.resize(count);
    pimpl_->entries.resize(count);

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t noff = pimpl_->name_offsets[i];
        if (noff >= pimpl_->names_blob.size()) {
            throw std::runtime_error("GTB name_offset out of range");
        }
        const char* name_ptr =
            reinterpret_cast<const char*>(pimpl_->names_blob.data() + noff);
        size_t max_len = pimpl_->names_blob.size() - noff;
        size_t slen = strnlen(name_ptr, max_len);
        pimpl_->names[i].assign(name_ptr, slen);

        uint32_t start = data_offsets[i];
        uint32_t end = (i + 1 < count) ? data_offsets[i + 1]
                                       : static_cast<uint32_t>(gpk_bytes.size());
        if (end < start || end > gpk_bytes.size()) {
            throw std::runtime_error("Invalid GTB entry offset range");
        }
        pimpl_->entries[i].assign(gpk_bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                  gpk_bytes.begin() + static_cast<std::ptrdiff_t>(end));
    }

    pimpl_->recompute_offsets();
}

void MenuArchivePatcher::load(const fs::path& gpk_path, const fs::path& gtb_path) {
    load_from_memory(read_file_bytes(gpk_path), read_file_bytes(gtb_path));
}

size_t MenuArchivePatcher::entry_count() const {
    return pimpl_->entries.size();
}

std::string MenuArchivePatcher::entry_name(size_t index) const {
    pimpl_->check_index(index);
    return pimpl_->names[index];
}

uint32_t MenuArchivePatcher::entry_offset(size_t index) const {
    if (index >= pimpl_->offsets.size()) {
        throw std::out_of_range("MenuArchivePatcher offset index out of range");
    }
    return pimpl_->offsets[index];
}

uint32_t MenuArchivePatcher::entry_size(size_t index) const {
    pimpl_->check_index(index);
    return static_cast<uint32_t>(pimpl_->entries[index].size());
}

const std::vector<uint8_t>& MenuArchivePatcher::entry_raw_bytes(size_t index) const {
    pimpl_->check_index(index);
    return pimpl_->entries[index];
}

RgbaImage MenuArchivePatcher::decode_entry_rgba(size_t index) const {
    pimpl_->check_index(index);
    const auto& raw = pimpl_->entries[index];
    return decode_dwq_to_rgba(raw.data(), raw.size());
}

void MenuArchivePatcher::replace_entry_rgba(size_t index, const RgbaImage& img,
                                            bool update_crop_margins) {
    pimpl_->check_index(index);
    const auto& orig = pimpl_->entries[index];
    pimpl_->entries[index] =
        encode_dwq_from_rgba(orig.data(), orig.size(), img, update_crop_margins);
    pimpl_->recompute_offsets();
}

void MenuArchivePatcher::replace_entry_png_bytes(size_t index,
                                                 const std::vector<uint8_t>& png_bytes,
                                                 bool update_crop_margins) {
    pimpl_->check_index(index);
    const auto& orig = pimpl_->entries[index];
    pimpl_->entries[index] =
        encode_dwq_from_png_bytes(orig.data(), orig.size(), png_bytes, update_crop_margins);
    pimpl_->recompute_offsets();
}

void MenuArchivePatcher::replace_entry_raw(size_t index, std::vector<uint8_t> raw_dwq) {
    pimpl_->check_index(index);
    pimpl_->entries[index] = std::move(raw_dwq);
    pimpl_->recompute_offsets();
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>> MenuArchivePatcher::serialize() const {
    uint32_t count = static_cast<uint32_t>(pimpl_->entries.size());
    uint32_t total_gpk_bytes = pimpl_->offsets.empty() ? 0 : pimpl_->offsets.back();

    std::vector<uint8_t> gpk_out;
    gpk_out.reserve(total_gpk_bytes);
    for (const auto& entry : pimpl_->entries) {
        gpk_out.insert(gpk_out.end(), entry.begin(), entry.end());
    }

    size_t gtb_header_bytes =
        sizeof(uint32_t) +
        static_cast<size_t>(count) * kGtbOffsetTableCount * sizeof(uint32_t);
    std::vector<uint8_t> gtb_out(gtb_header_bytes + pimpl_->names_blob.size(), 0);
    write_u32_le(gtb_out.data(), count);

    for (uint32_t i = 0; i < count; ++i) {
        write_u32_le(
            gtb_out.data() + sizeof(uint32_t) + static_cast<size_t>(i) * sizeof(uint32_t),
            pimpl_->name_offsets[i]);
        write_u32_le(gtb_out.data() + sizeof(uint32_t) +
                         static_cast<size_t>(count + i) * sizeof(uint32_t),
                     pimpl_->offsets[i]);
    }

    if (!pimpl_->names_blob.empty()) {
        std::memcpy(gtb_out.data() + gtb_header_bytes, pimpl_->names_blob.data(),
                    pimpl_->names_blob.size());
    }
    return {std::move(gtb_out), std::move(gpk_out)};
}

void MenuArchivePatcher::save(const fs::path& out_gpk_path,
                              const fs::path& out_gtb_path) const {
    auto [gtb_out, gpk_out] = serialize();
    write_file_bytes(out_gpk_path, gpk_out);
    write_file_bytes(out_gtb_path, gtb_out);
}

std::vector<uint8_t> MenuArchivePatcher::build_patch_bin_from_game_dir(
    const fs::path& game_dir) {
    const fs::path orig_gtb_path =
        resolve_original_file_path(game_dir, "dwq_backup", "sys0.gtb", "dwq\\sys0.gtb");
    const fs::path orig_gpk_path =
        resolve_original_file_path(game_dir, "dwq_backup", "sys0.gpk", "dwq\\sys0.gpk");
    const fs::path new_gtb_path = game_dir / "dwq" / "sys0.gtb";
    const fs::path new_gpk_path = game_dir / "dwq" / "sys0.gpk";

    const auto orig_gtb = read_file_bytes(orig_gtb_path);
    const auto orig_gpk = read_file_bytes(orig_gpk_path);
    const auto new_gtb = read_file_bytes(new_gtb_path);
    const auto new_gpk = read_file_bytes(new_gpk_path);

    MenuArchivePatcher orig_arch;
    orig_arch.load_from_memory(orig_gpk, orig_gtb);
    MenuArchivePatcher new_arch;
    new_arch.load_from_memory(new_gpk, new_gtb);

    if (orig_arch.entry_count() != new_arch.entry_count()) {
        throw std::runtime_error(
            "MenuArchivePatcher::build_patch_bin_from_game_dir: GTB entry count mismatch");
    }

    std::vector<GpkEntryPatchRecord> gpk_records;
    size_t total_gpk_payload_bytes = 0;

    for (size_t i = 0; i < orig_arch.entry_count(); ++i) {
        const auto& orig_slice = orig_arch.entry_raw_bytes(i);
        const auto& new_slice = new_arch.entry_raw_bytes(i);
        if (orig_slice == new_slice) {
            continue;
        }
        GpkEntryPatchRecord rec;
        rec.entry_index = static_cast<uint32_t>(i);
        rec.orig_offset = orig_arch.entry_offset(i);
        rec.orig_size = static_cast<uint32_t>(orig_slice.size());
        rec.orig_data_hash = ComputeMurmurHash2(orig_slice.data(), orig_slice.size());
        rec.new_size = static_cast<uint32_t>(new_slice.size());
        rec.new_data_hash = ComputeMurmurHash2(new_slice.data(), new_slice.size());
        rec.masked_entry = new_slice;
        XorMaskPayload(rec.masked_entry.data(), rec.masked_entry.size(),
                       orig_slice.data(), orig_slice.size(), rec.orig_data_hash);
        total_gpk_payload_bytes += kGpkEntryRecordHeaderSize + rec.masked_entry.size();
        gpk_records.push_back(std::move(rec));
    }

    std::vector<StandalonePatchRecord> standalone_records;
    standalone_records.reserve(kStandaloneFiles.size());
    size_t total_standalone_bytes = 0;

    for (const StandaloneFileDescriptor& desc : kStandaloneFiles) {
        const fs::path orig_path = resolve_original_file_path(
            game_dir, desc.backup_dir, desc.filename, desc.rel_path);
        const fs::path new_path = game_dir / fs::u8path(desc.rel_path);

        const auto orig_bytes = read_file_bytes(orig_path);
        const auto new_bytes = read_file_bytes(new_path);

        StandalonePatchRecord rec;
        rec.rel_path = desc.rel_path;
        rec.orig_file_size = static_cast<uint32_t>(orig_bytes.size());
        rec.orig_data_hash = ComputeMurmurHash2(orig_bytes.data(), orig_bytes.size());
        rec.new_file_size = static_cast<uint32_t>(new_bytes.size());
        rec.new_data_hash = ComputeMurmurHash2(new_bytes.data(), new_bytes.size());
        rec.masked_payload = new_bytes;
        XorMaskPayload(rec.masked_payload.data(), rec.masked_payload.size(),
                       orig_bytes.data(), orig_bytes.size(), rec.orig_data_hash);
        total_standalone_bytes += kU16ByteSize + rec.rel_path.size() +
                                  kStandaloneRecordMetaSize + rec.masked_payload.size();
        standalone_records.push_back(std::move(rec));
    }

    const uint32_t orig_gtb_hash = ComputeMurmurHash2(orig_gtb.data(), orig_gtb.size());
    const uint32_t new_gtb_hash = ComputeMurmurHash2(new_gtb.data(), new_gtb.size());
    std::vector<uint8_t> masked_gtb = new_gtb;
    XorMaskPayload(masked_gtb.data(), masked_gtb.size(),
                   orig_gtb.data(), orig_gtb.size(), orig_gtb_hash);

    std::vector<uint8_t> out;
    out.reserve(kPatchHeaderSize + total_standalone_bytes + masked_gtb.size() +
                total_gpk_payload_bytes);
    out.insert(out.end(), kPatchMagic, kPatchMagic + kPatchMagicSize);
    append_u32_le(out, kPatchFormatVersion);
    append_u32_le(out, static_cast<uint32_t>(standalone_records.size()));
    append_u32_le(out, static_cast<uint32_t>(orig_gpk.size()));
    append_u32_le(out, static_cast<uint32_t>(new_gpk.size()));
    append_u32_le(out, static_cast<uint32_t>(orig_gtb.size()));
    append_u32_le(out, orig_gtb_hash);
    append_u32_le(out, static_cast<uint32_t>(new_gtb.size()));
    append_u32_le(out, new_gtb_hash);
    append_u32_le(out, static_cast<uint32_t>(gpk_records.size()));

    for (const StandalonePatchRecord& rec : standalone_records) {
        append_u16_le(out, static_cast<uint16_t>(rec.rel_path.size()));
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
    const fs::path& game_dir, const fs::path& out_patch_bin_path) {
    const auto patch_bin = build_patch_bin_from_game_dir(game_dir);
    write_file_bytes(out_patch_bin_path, patch_bin);
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>>
MenuArchivePatcher::reconstruct_archive_from_patch_bin(
    const std::vector<uint8_t>& patch_bin,
    const std::vector<uint8_t>& orig_gtb_bytes,
    const std::vector<uint8_t>& orig_gpk_bytes) {
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

    std::vector<uint8_t> new_gtb = parsed.masked_gtb;
    XorMaskPayload(new_gtb.data(), new_gtb.size(),
                   orig_gtb_bytes.data(), orig_gtb_bytes.size(), parsed.orig_gtb_hash);
    if (ComputeMurmurHash2(new_gtb.data(), new_gtb.size()) != parsed.new_gtb_hash) {
        throw std::runtime_error(
            "MenuArchivePatcher::reconstruct_archive_from_patch_bin: new GTB hash mismatch");
    }

    MenuArchivePatcher arch;
    arch.load_from_memory(orig_gpk_bytes, orig_gtb_bytes);

    std::vector<uint32_t> orig_offsets(arch.entry_count());
    for (size_t i = 0; i < arch.entry_count(); ++i) {
        orig_offsets[i] = arch.entry_offset(i);
    }

    for (const GpkEntryPatchRecord& rec : parsed.gpk_records) {
        if (rec.entry_index >= arch.entry_count()) {
            throw std::runtime_error(
                "MenuArchivePatcher::reconstruct_archive_from_patch_bin: entry_index out of range");
        }
        const auto& orig_slice = arch.entry_raw_bytes(rec.entry_index);
        if (orig_offsets[rec.entry_index] != rec.orig_offset ||
            orig_slice.size() != rec.orig_size ||
            ComputeMurmurHash2(orig_slice.data(), orig_slice.size()) != rec.orig_data_hash) {
            throw std::runtime_error(
                "MenuArchivePatcher::reconstruct_archive_from_patch_bin: entry orig hash mismatch");
        }

        std::vector<uint8_t> unmasked = rec.masked_entry;
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
    const fs::path& game_dir,
    const fs::path& patch_bin_path,
    bool test_mode) {
    const auto patch_bin = read_file_bytes(patch_bin_path);
    const ParsedPatchBin parsed = parse_patch_bin(patch_bin);

    const fs::path orig_gtb_path =
        resolve_original_file_path(game_dir, "dwq_backup", "sys0.gtb", "dwq\\sys0.gtb");
    const fs::path orig_gpk_path =
        resolve_original_file_path(game_dir, "dwq_backup", "sys0.gpk", "dwq\\sys0.gpk");
    const auto orig_gtb = read_file_bytes(orig_gtb_path);
    const auto orig_gpk = read_file_bytes(orig_gpk_path);

    auto [new_gtb, new_gpk] =
        reconstruct_archive_from_patch_bin(patch_bin, orig_gtb, orig_gpk);

    struct StagedStandalone {
        fs::path orig_path;
        fs::path live_path;
        std::vector<uint8_t> orig_bytes;
        std::vector<uint8_t> new_bytes;
    };
    std::vector<StagedStandalone> standalone_outputs;
    standalone_outputs.reserve(parsed.standalone_records.size());

    for (size_t i = 0; i < parsed.standalone_records.size(); ++i) {
        const StandalonePatchRecord& rec = parsed.standalone_records[i];
        fs::path orig_path;
        if (i < kStandaloneFiles.size() && rec.rel_path == kStandaloneFiles[i].rel_path) {
            orig_path = resolve_original_file_path(
                game_dir, kStandaloneFiles[i].backup_dir, kStandaloneFiles[i].filename,
                kStandaloneFiles[i].rel_path);
        } else {
            orig_path = resolve_original_file_path(
                game_dir, "", "", rec.rel_path.c_str());
        }

        const auto orig_bytes = read_file_bytes(orig_path);
        if (orig_bytes.size() != rec.orig_file_size ||
            ComputeMurmurHash2(orig_bytes.data(), orig_bytes.size()) != rec.orig_data_hash) {
            throw std::runtime_error("apply_patch_bin_to_game_dir: orig hash mismatch for " +
                                     rec.rel_path);
        }

        std::vector<uint8_t> unmasked = rec.masked_payload;
        XorMaskPayload(unmasked.data(), unmasked.size(),
                       orig_bytes.data(), orig_bytes.size(), rec.orig_data_hash);
        if (ComputeMurmurHash2(unmasked.data(), unmasked.size()) != rec.new_data_hash) {
            throw std::runtime_error("apply_patch_bin_to_game_dir: new hash mismatch for " +
                                     rec.rel_path);
        }
        standalone_outputs.push_back(StagedStandalone{
            orig_path,
            game_dir / fs::u8path(rec.rel_path),
            orig_bytes,
            std::move(unmasked),
        });
    }

    if (!test_mode) {
        const fs::path live_gpk = game_dir / "dwq" / "sys0.gpk";
        const fs::path live_gtb = game_dir / "dwq" / "sys0.gtb";
        const fs::path bak_gpk = game_dir / "dwq" / "sys0.gpk.bak";
        const fs::path bak_gtb = game_dir / "dwq" / "sys0.gtb.bak";

        if (!fs::exists(bak_gpk)) {
            write_file_bytes(bak_gpk, orig_gpk);
        }
        if (!fs::exists(bak_gtb)) {
            write_file_bytes(bak_gtb, orig_gtb);
        }
        write_file_bytes(live_gpk, new_gpk);
        write_file_bytes(live_gtb, new_gtb);

        for (const auto& item : standalone_outputs) {
            const fs::path bak_path = fs::u8path(item.live_path.u8string() + ".bak");
            if (!fs::exists(bak_path)) {
                write_file_bytes(bak_path, item.orig_bytes);
            }
            write_file_bytes(item.live_path, item.new_bytes);
        }
    } else {
        const fs::path live_gpk = game_dir / "dwq" / "sys0.gpk";
        const fs::path live_gtb = game_dir / "dwq" / "sys0.gtb";
        if (fs::exists(live_gpk)) {
            const auto cur_gpk = read_file_bytes(live_gpk);
            if (cur_gpk != new_gpk && cur_gpk != orig_gpk) {
                throw std::runtime_error(
                    "apply_patch_bin_to_game_dir: live dwq\\sys0.gpk matches neither original nor patched");
            }
        }
        if (fs::exists(live_gtb)) {
            const auto cur_gtb = read_file_bytes(live_gtb);
            if (cur_gtb != new_gtb && cur_gtb != orig_gtb) {
                throw std::runtime_error(
                    "apply_patch_bin_to_game_dir: live dwq\\sys0.gtb matches neither original nor patched");
            }
        }
        for (const auto& item : standalone_outputs) {
            if (fs::exists(item.live_path)) {
                const auto cur_bytes = read_file_bytes(item.live_path);
                if (cur_bytes != item.new_bytes && cur_bytes != item.orig_bytes) {
                    throw std::runtime_error(
                        "apply_patch_bin_to_game_dir: live file matches neither original nor patched: " +
                        item.live_path.u8string());
                }
            }
        }
    }

    const uint32_t total_entries = read_u32_le(new_gtb.data());
    MenuBuildSummary summary;
    summary.total_archive_entries = total_entries;
    summary.patched_sprite_entries = parsed.gpk_entry_count;
    summary.untouched_sprite_entries = total_entries - parsed.gpk_entry_count;
    summary.patched_standalone_files = parsed.standalone_file_count;
    summary.patched_main_control_fxf = true;
    summary.patched_backlog_fxf = true;
    return summary;
}

int run_patch_menus(const PatchMenusOptions& opt) {
    const fs::path dwq_dir = opt.game_dir / "dwq";
    const fs::path dwq_backup_dir = opt.game_dir / "dwq_backup";
    const fs::path gpk_live = dwq_dir / "sys0.gpk";
    const fs::path gtb_live = dwq_dir / "sys0.gtb";
    const fs::path gpk_backup = dwq_backup_dir / "sys0.gpk";
    const fs::path gtb_backup = dwq_backup_dir / "sys0.gtb";

    if (!opt.extract_only &&
        (opt.rendered_dir.empty() || !fs::is_directory(opt.rendered_dir))) {
        if (!opt.write_patch_bin_path.empty() && !opt.test_mode) {
            MenuArchivePatcher::write_patch_bin_from_game_dir(
                opt.game_dir, opt.write_patch_bin_path);
            std::cout << "Wrote MIZMENU1 patch container to "
                      << opt.write_patch_bin_path.u8string() << "\n";
        }
        if (!opt.patch_bin_path.empty() && fs::is_regular_file(opt.patch_bin_path)) {
            const auto summary = MenuArchivePatcher::apply_patch_bin_to_game_dir(
                opt.game_dir, opt.patch_bin_path, opt.test_mode);
            std::cout << (opt.test_mode ? "[test_mode] Verified " : "Applied ")
                      << opt.patch_bin_path.u8string() << ": "
                      << summary.patched_sprite_entries << "/"
                      << summary.total_archive_entries << " sys0.gpk sprites and "
                      << summary.patched_standalone_files << " standalone files.\n";
            return 0;
        }
    }

    if (!fs::exists(gpk_backup) || !fs::exists(gtb_backup)) {
        if (!fs::exists(gpk_live) || !fs::exists(gtb_live)) {
            std::cerr << "ERROR: Neither dwq_backup\\sys0.{gpk,gtb} nor dwq\\sys0.{gpk,gtb} found in "
                      << opt.game_dir.u8string() << "\n";
            return 1;
        }
        if (!opt.test_mode && !opt.extract_only) {
            fs::create_directories(dwq_backup_dir);
            fs::copy_file(gpk_live, gpk_backup, fs::copy_options::overwrite_existing);
            fs::copy_file(gtb_live, gtb_backup, fs::copy_options::overwrite_existing);
        }
    }

    const fs::path src_gpk = fs::exists(gpk_backup) ? gpk_backup : gpk_live;
    const fs::path src_gtb = fs::exists(gtb_backup) ? gtb_backup : gtb_live;

    MenuArchivePatcher patcher;
    patcher.load(src_gpk, src_gtb);

    if (opt.extract_only) {
        fs::path out_dir = opt.extract_dir.empty() ? (opt.rendered_dir / "extracted")
                                                   : opt.extract_dir;
        fs::create_directories(out_dir);
        for (size_t i = 0; i < patcher.entry_count(); ++i) {
            char buf[kExtractFilenameBufferSize];
            std::snprintf(buf, sizeof(buf), "%03zu_%s.png", i,
                          patcher.entry_name(i).c_str());
            const auto& raw = patcher.entry_raw_bytes(i);
            if (raw.size() >= kDwqHeaderSize &&
                raw[kDwqPackTypeCharOffset] == kPackTypePng) {
                uint32_t dsize = read_u32_le(raw.data() + kDwqDataSizeOffset);
                if (kDwqHeaderSize + dsize <= raw.size()) {
                    std::vector<uint8_t> png_raw(raw.begin() + kDwqHeaderSize,
                                                 raw.begin() + kDwqHeaderSize + dsize);
                    write_file_bytes(out_dir / buf, png_raw);
                    continue;
                }
            }
            auto rgba = patcher.decode_entry_rgba(i);
            write_png_rgba(out_dir / buf, rgba);
        }
        std::cout << "Extracted " << patcher.entry_count() << " entries to "
                  << out_dir.u8string() << "\n";
        return 0;
    }

    std::unordered_map<size_t, std::string> target_indices;
    if (fs::exists(opt.manifest_path)) {
        auto json_text = read_file_string(opt.manifest_path);
        auto jv = boost::json::parse(json_text);
        const boost::json::array* arr_ptr = nullptr;
        if (jv.is_array()) {
            arr_ptr = &jv.as_array();
        } else if (jv.is_object() && jv.as_object().contains("entries") &&
                   jv.as_object().at("entries").is_array()) {
            arr_ptr = &jv.as_object().at("entries").as_array();
        }
        if (arr_ptr) {
            for (const auto& item : *arr_ptr) {
                if (!item.is_object()) continue;
                const auto& obj = item.as_object();
                bool translate = obj.contains("translate") && obj.at("translate").is_bool() &&
                                 obj.at("translate").as_bool();
                if (!translate) continue;
                size_t idx = static_cast<size_t>(obj.at("index").to_number<int64_t>());
                std::string name = std::string(obj.at("name").as_string());
                target_indices[idx] = name;
            }
        }
    }

    size_t replaced_count = 0;
    for (size_t i = 0; i < patcher.entry_count(); ++i) {
        if (!target_indices.empty() && target_indices.find(i) == target_indices.end()) {
            continue;
        }
        char fname_buf[kSpriteFilenameBufferSize];
        std::snprintf(fname_buf, sizeof(fname_buf), "%03zu_%s.png", i,
                      patcher.entry_name(i).c_str());
        fs::path cand = opt.rendered_dir / fname_buf;
        if (!fs::exists(cand)) {
            cand = opt.rendered_dir / (patcher.entry_name(i) + ".png");
        }
        if (!fs::exists(cand)) {
            if (!target_indices.empty()) {
                std::cerr << "ERROR: Missing rendered PNG for manifest entry "
                          << fname_buf << " in " << opt.rendered_dir.u8string() << "\n";
                return 1;
            }
            continue;
        }

        const auto& raw = patcher.entry_raw_bytes(i);
        if (raw.size() >= kDwqHeaderSize &&
            raw[kDwqPackTypeCharOffset] == kPackTypePng) {
            auto png_bytes = read_file_bytes(cand);
            patcher.replace_entry_png_bytes(i, png_bytes, true);
        } else {
            auto img = read_png_rgba(cand);
            patcher.replace_entry_rgba(i, img, true);
        }
        ++replaced_count;
    }

    if (opt.test_mode) {
        std::cout << "[test_mode] Verified " << replaced_count << " menu sprite replacements.\n";
        return 0;
    }

    patcher.save(gpk_live, gtb_live);
    std::cout << "Patched " << replaced_count << " menu sprites into " << gpk_live.u8string()
              << " and " << gtb_live.u8string() << "\n";

    const fs::path init2_dir = opt.game_dir / "init2";
    const fs::path init2_backup_dir = opt.game_dir / "init2_backup";

    const auto patch_fxf_file = [&](const std::string& filename,
                                    std::string (*patch_fn)(const std::string&)) {
        fs::path live_path = init2_dir / filename;
        fs::path backup_path = init2_backup_dir / filename;
        if (!fs::exists(backup_path) && fs::exists(live_path)) {
            fs::create_directories(init2_backup_dir);
            fs::copy_file(live_path, backup_path, fs::copy_options::overwrite_existing);
        }
        fs::path src_path = fs::exists(backup_path) ? backup_path : live_path;
        if (!fs::exists(src_path)) {
            return;
        }
        auto raw = read_file_bytes(src_path);
        auto dec = decrypt_fxf(raw);
        auto patched = patch_fn(dec);
        auto enc = encrypt_fxf(patched);
        write_file_bytes(live_path, enc);
        std::cout << "Patched " << live_path.u8string() << "\n";
    };

    patch_fxf_file("mainControl.fxf", &patch_main_control_fxf);
    patch_fxf_file("backlog.fxf", &patch_backlog_fxf);

    if (fs::exists(opt.game_dir / "spt" / "charaname.xtx") ||
        fs::exists(opt.game_dir / "xtx_backup" / "charaname.xtx") ||
        fs::exists(opt.game_dir / "xtx_backup" / "spt" / "charaname.xtx")) {
        exm::xtx::run_translate_xtx(opt.game_dir.u8string());
    }

    if (!opt.write_patch_bin_path.empty()) {
        MenuArchivePatcher::write_patch_bin_from_game_dir(
            opt.game_dir, opt.write_patch_bin_path);
        std::cout << "Wrote MIZMENU1 patch container to "
                  << opt.write_patch_bin_path.u8string() << "\n";
    }

    return 0;
}

}  // namespace exm::menu
