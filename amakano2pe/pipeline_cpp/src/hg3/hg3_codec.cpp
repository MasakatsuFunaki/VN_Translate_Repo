// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "hg3/hg3_codec.h"

#include "imaging/wic_image_io.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ama::hg3 {

namespace {

constexpr std::size_t kFileHeaderBytes = 12;
constexpr std::size_t kMagicBytes = 4;
constexpr const char kHg3Magic[kMagicBytes] = {'H', 'G', '-', '3'};
constexpr std::uint32_t kExpectedHeaderSize = 0x0CU;
constexpr std::uint32_t kExpectedVersion = 0x0300U;
constexpr std::size_t kHeaderSizeOffset = 4;
constexpr std::size_t kVersionOffset = 8;

constexpr std::size_t kFrameHeaderBytes = 8;
constexpr std::size_t kFrameIdOffset = 4;

constexpr std::size_t kTagHeaderBytes = 16;
constexpr std::size_t kTagNameFieldBytes = 8;
constexpr std::size_t kTagNextRelOffset = 8;
constexpr std::size_t kTagDataLenOffset = 12;

constexpr std::size_t kStdInfoBytes = 40;
constexpr std::size_t kStdWidthOffset = 0;
constexpr std::size_t kStdHeightOffset = 4;
constexpr std::size_t kStdBppOffset = 8;
constexpr std::size_t kStdOffsetXOffset = 12;
constexpr std::size_t kStdOffsetYOffset = 16;
constexpr std::size_t kStdTotalWidthOffset = 20;
constexpr std::size_t kStdTotalHeightOffset = 24;
constexpr std::size_t kStdTransparentOffset = 28;
constexpr std::size_t kStdCanvasXOffset = 32;
constexpr std::size_t kStdCanvasYOffset = 36;

constexpr std::size_t kImg0000HeaderBytes = 24;
constexpr std::size_t kImg0000UnknownOffset = 0;
constexpr std::size_t kImg0000HeightOffset = 4;
constexpr std::size_t kImg0000DataCompLenOffset = 8;
constexpr std::size_t kImg0000DataOrigLenOffset = 12;
constexpr std::size_t kImg0000CmdCompLenOffset = 16;
constexpr std::size_t kImg0000CmdOrigLenOffset = 20;

constexpr std::size_t kImgAlHeaderBytes = 8;
constexpr std::size_t kImgAlCompLenOffset = 0;
constexpr std::size_t kImgAlOrigLenOffset = 4;

constexpr std::size_t kU32PayloadBytes = 4;
constexpr std::uint32_t kBpp24 = 24;
constexpr std::uint32_t kBpp32 = 32;
constexpr std::size_t kBytesPerPixel24 = 3;
constexpr std::size_t kBytesPerPixel32 = 4;
constexpr std::size_t kRowAlignment = 4;
constexpr std::size_t kInterleaveSections = 4;
constexpr int kBitsPerPair = 2;
constexpr std::uint8_t kTwoBitMask = 0x03U;
constexpr int kSectionShift0 = 6;
constexpr int kSectionShift1 = 4;
constexpr int kSectionShift2 = 2;
constexpr int kSectionShift3 = 0;
constexpr std::uint8_t kTopBitMask = 0x80U;
constexpr std::uint8_t kOpaqueAlpha = 0xFFU;

constexpr int kByteShift0 = 0;
constexpr int kByteShift1 = 8;
constexpr int kByteShift2 = 16;
constexpr int kByteShift3 = 24;
constexpr int kBitsPerByte = 8;
constexpr std::size_t kBitIndexShift = 3;
constexpr std::size_t kBitOffsetMask = 7;
constexpr std::uint32_t kByteMask = 0xFFU;
constexpr std::size_t kInflateChunkBytes = 65536;
constexpr int kMaxGammaZeros = 60;
constexpr std::uint32_t kBootSkinWidth = 352;
constexpr std::uint32_t kBootSkinHeight = 255;

bool IsBootMenuSkinStdInfo(const Hg3StdInfo& info) noexcept {
    return info.width == kBootSkinWidth && info.height == kBootSkinHeight &&
           info.total_width == kBootSkinWidth && info.total_height == kBootSkinHeight &&
           info.transparent_flag == 0U;
}

std::uint32_t ReadU32Le(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << kByteShift0) |
           (static_cast<std::uint32_t>(p[1]) << kByteShift1) |
           (static_cast<std::uint32_t>(p[2]) << kByteShift2) |
           (static_cast<std::uint32_t>(p[3]) << kByteShift3);
}

std::int32_t ReadI32Le(const std::uint8_t* p) {
    return static_cast<std::int32_t>(ReadU32Le(p));
}

void WriteU32Le(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>((v >> kByteShift0) & kByteMask);
    p[1] = static_cast<std::uint8_t>((v >> kByteShift1) & kByteMask);
    p[2] = static_cast<std::uint8_t>((v >> kByteShift2) & kByteMask);
    p[3] = static_cast<std::uint8_t>((v >> kByteShift3) & kByteMask);
}

void WriteI32Le(std::uint8_t* p, std::int32_t v) {
    WriteU32Le(p, static_cast<std::uint32_t>(v));
}

std::string ParseTagName(const std::uint8_t* raw_name) {
    std::size_t len = 0;
    while (len < kTagNameFieldBytes && raw_name[len] != 0) {
        ++len;
    }
    return std::string(reinterpret_cast<const char*>(raw_name), len);
}

std::vector<std::uint8_t> ZlibDecompress(const std::uint8_t* src, std::size_t comp_len,
                                         std::size_t expected_orig_len) {
    if (comp_len == 0) {
        return std::vector<std::uint8_t>(expected_orig_len, 0);
    }

    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) {
        throw std::runtime_error("inflateInit failed in Hg3Codec");
    }
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src));
    zs.avail_in = static_cast<uInt>(comp_len);

    std::vector<std::uint8_t> out;
    if (expected_orig_len > 0) {
        out.reserve(expected_orig_len);
    }
    std::vector<std::uint8_t> chunk(kInflateChunkBytes);
    int rc = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(chunk.data());
        zs.avail_out = static_cast<uInt>(chunk.size());
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            inflateEnd(&zs);
            throw std::runtime_error("inflate failed in Hg3Codec");
        }
        const std::size_t produced = chunk.size() - static_cast<std::size_t>(zs.avail_out);
        out.insert(out.end(), chunk.begin(),
                   chunk.begin() + static_cast<std::ptrdiff_t>(produced));
        if (rc == Z_BUF_ERROR && zs.avail_out != 0) {
            break;
        }
    } while (rc != Z_STREAM_END);

    inflateEnd(&zs);
    if (expected_orig_len > 0 && out.size() < expected_orig_len) {
        out.resize(expected_orig_len, 0);
    }
    return out;
}

std::vector<std::uint8_t> ZlibCompress(const std::uint8_t* src, std::size_t orig_len) {
    uLongf bound = compressBound(static_cast<uLong>(orig_len));
    std::vector<std::uint8_t> comp(static_cast<std::size_t>(bound));
    const Bytef* input_ptr =
        orig_len > 0 ? reinterpret_cast<const Bytef*>(src) : reinterpret_cast<const Bytef*>("");
    const int rc = compress2(reinterpret_cast<Bytef*>(comp.data()), &bound, input_ptr,
                             static_cast<uLong>(orig_len), Z_BEST_COMPRESSION);
    if (rc != Z_OK) {
        throw std::runtime_error("compress2 failed in Hg3Codec");
    }
    comp.resize(static_cast<std::size_t>(bound));
    return comp;
}

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t byte_len)
        : data_(data), bit_len_(byte_len * kBitsPerByte) {}

    std::uint32_t ReadBit() {
        if (bit_pos_ >= bit_len_) {
            throw std::runtime_error("HG-3 command bitstream overrun");
        }
        const std::uint32_t bit =
            (data_[bit_pos_ >> kBitIndexShift] >> (bit_pos_ & kBitOffsetMask)) & 1U;
        ++bit_pos_;
        return bit;
    }

    std::size_t ReadGamma() {
        int zeros = 0;
        while (ReadBit() == 0U) {
            ++zeros;
            if (zeros > kMaxGammaZeros) {
                throw std::runtime_error("invalid Elias-gamma code in HG-3 stream");
            }
        }
        std::size_t value = 1;
        for (int i = 0; i < zeros; ++i) {
            value = (value << 1U) | static_cast<std::size_t>(ReadBit());
        }
        return value;
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t bit_len_ = 0;
    std::size_t bit_pos_ = 0;
};

class BitWriter {
public:
    void WriteBit(std::uint32_t bit) {
        const std::size_t byte_index = bit_pos_ >> kBitIndexShift;
        if (byte_index >= bytes_.size()) {
            bytes_.push_back(0);
        }
        if ((bit & 1U) != 0U) {
            bytes_[byte_index] = static_cast<std::uint8_t>(
                bytes_[byte_index] | static_cast<std::uint8_t>(1U << (bit_pos_ & kBitOffsetMask)));
        }
        ++bit_pos_;
    }

    void WriteGamma(std::size_t value) {
        if (value == 0) {
            throw std::invalid_argument("Elias-gamma value must be >= 1");
        }
        int zeros = 0;
        std::size_t tmp = value;
        while ((tmp >> 1U) != 0U) {
            tmp >>= 1U;
            ++zeros;
        }
        for (int i = 0; i < zeros; ++i) {
            WriteBit(0U);
        }
        WriteBit(1U);
        for (int bit = zeros - 1; bit >= 0; --bit) {
            WriteBit(static_cast<std::uint32_t>((value >> bit) & 1U));
        }
    }

    const std::vector<std::uint8_t>& Bytes() const noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t bit_pos_ = 0;
};

std::uint8_t InverseZigZag(std::uint8_t z) noexcept {
    const std::uint8_t mask =
        static_cast<std::uint8_t>(-static_cast<std::int8_t>(z & 1U));
    return static_cast<std::uint8_t>((z >> 1U) ^ mask);
}

std::uint8_t ForwardZigZag(std::uint8_t b) noexcept {
    if ((b & kTopBitMask) != 0U) {
        return static_cast<std::uint8_t>((static_cast<std::uint8_t>(~b) << 1U) | 1U);
    }
    return static_cast<std::uint8_t>(b << 1U);
}

Hg3StdInfo ParseStdInfo(const std::uint8_t* payload, std::size_t len) {
    if (len < kStdInfoBytes) {
        throw std::runtime_error("HG-3 stdinfo chunk is smaller than 40 bytes");
    }
    Hg3StdInfo info{};
    info.width = ReadU32Le(payload + kStdWidthOffset);
    info.height = ReadU32Le(payload + kStdHeightOffset);
    info.bpp = ReadU32Le(payload + kStdBppOffset);
    info.offset_x = ReadI32Le(payload + kStdOffsetXOffset);
    info.offset_y = ReadI32Le(payload + kStdOffsetYOffset);
    info.total_width = ReadU32Le(payload + kStdTotalWidthOffset);
    info.total_height = ReadU32Le(payload + kStdTotalHeightOffset);
    info.transparent_flag = ReadU32Le(payload + kStdTransparentOffset);
    info.canvas_x = ReadI32Le(payload + kStdCanvasXOffset);
    info.canvas_y = ReadI32Le(payload + kStdCanvasYOffset);
    return info;
}

std::vector<std::uint8_t> SerializeStdInfo(const Hg3StdInfo& info) {
    std::vector<std::uint8_t> buf(kStdInfoBytes, 0);
    WriteU32Le(buf.data() + kStdWidthOffset, info.width);
    WriteU32Le(buf.data() + kStdHeightOffset, info.height);
    WriteU32Le(buf.data() + kStdBppOffset, info.bpp);
    WriteI32Le(buf.data() + kStdOffsetXOffset, info.offset_x);
    WriteI32Le(buf.data() + kStdOffsetYOffset, info.offset_y);
    WriteU32Le(buf.data() + kStdTotalWidthOffset, info.total_width);
    WriteU32Le(buf.data() + kStdTotalHeightOffset, info.total_height);
    WriteU32Le(buf.data() + kStdTransparentOffset, info.transparent_flag);
    WriteI32Le(buf.data() + kStdCanvasXOffset, info.canvas_x);
    WriteI32Le(buf.data() + kStdCanvasYOffset, info.canvas_y);
    return buf;
}

ama::imaging::BgraImage DecodeImg0000(const Hg3StdInfo& stdinfo, const std::uint8_t* payload,
                                      std::size_t len, bool top_down_wire,
                                      std::uint32_t* out_unknown0) {
    if (len < kImg0000HeaderBytes) {
        throw std::runtime_error("HG-3 img0000 chunk is smaller than 24 bytes");
    }
    const std::uint32_t unknown0 = ReadU32Le(payload + kImg0000UnknownOffset);
    if (out_unknown0 != nullptr) {
        *out_unknown0 = unknown0;
    }
    const std::uint32_t data_comp_len = ReadU32Le(payload + kImg0000DataCompLenOffset);
    const std::uint32_t data_orig_len = ReadU32Le(payload + kImg0000DataOrigLenOffset);
    const std::uint32_t cmd_comp_len = ReadU32Le(payload + kImg0000CmdCompLenOffset);
    const std::uint32_t cmd_orig_len = ReadU32Le(payload + kImg0000CmdOrigLenOffset);

    if (kImg0000HeaderBytes + static_cast<std::size_t>(data_comp_len) +
            static_cast<std::size_t>(cmd_comp_len) >
        len) {
        throw std::runtime_error("HG-3 img0000 compressed streams exceed tag payload");
    }
    if (stdinfo.width == 0 || stdinfo.height == 0) {
        return {};
    }

    const std::vector<std::uint8_t> data_stream =
        ZlibDecompress(payload + kImg0000HeaderBytes, data_comp_len, data_orig_len);
    const std::vector<std::uint8_t> cmd_stream = ZlibDecompress(
        payload + kImg0000HeaderBytes + data_comp_len, cmd_comp_len, cmd_orig_len);

    BitReader reader(cmd_stream.data(), cmd_stream.size());
    bool copy_flag = reader.ReadBit() != 0U;
    const std::size_t total_size = reader.ReadGamma();
    if (total_size == 0 || (total_size % kInterleaveSections) != 0) {
        throw std::runtime_error("invalid HG-3 img0000 unpacked buffer size");
    }

    std::vector<std::uint8_t> rle_buf(total_size, 0);
    std::size_t dst_pos = 0;
    std::size_t src_pos = 0;
    while (dst_pos < total_size) {
        const std::size_t run = reader.ReadGamma();
        if (dst_pos + run > total_size) {
            throw std::runtime_error("HG-3 img0000 RLE run exceeds buffer size");
        }
        if (copy_flag) {
            if (src_pos + run > data_stream.size()) {
                throw std::runtime_error("HG-3 img0000 RLE data stream underflow");
            }
            std::memcpy(rle_buf.data() + dst_pos, data_stream.data() + src_pos, run);
            src_pos += run;
        }
        dst_pos += run;
        copy_flag = !copy_flag;
    }

    const std::size_t sect = total_size / kInterleaveSections;
    const std::uint8_t* s0 = rle_buf.data();
    const std::uint8_t* s1 = rle_buf.data() + sect;
    const std::uint8_t* s2 = rle_buf.data() + sect * 2;
    const std::uint8_t* s3 = rle_buf.data() + sect * 3;

    std::vector<std::uint8_t> pixel_buf(total_size, 0);
    for (std::size_t i = 0; i < sect; ++i) {
        for (std::size_t k = 0; k < kInterleaveSections; ++k) {
            const int sh = static_cast<int>(k) * kBitsPerPair;
            const std::uint8_t z = static_cast<std::uint8_t>(
                (((s0[i] >> sh) & kTwoBitMask) << kSectionShift0) |
                (((s1[i] >> sh) & kTwoBitMask) << kSectionShift1) |
                (((s2[i] >> sh) & kTwoBitMask) << kSectionShift2) |
                (((s3[i] >> sh) & kTwoBitMask) << kSectionShift3));
            pixel_buf[i * kInterleaveSections + k] = InverseZigZag(z);
        }
    }

    const std::size_t bpp_bytes =
        (stdinfo.bpp == kBpp24) ? kBytesPerPixel24 : kBytesPerPixel32;
    const std::size_t unaligned_stride = static_cast<std::size_t>(stdinfo.width) * bpp_bytes;
    const std::size_t aligned_stride =
        (unaligned_stride + (kRowAlignment - 1)) & ~(kRowAlignment - 1);
    const std::size_t row_stride =
        (aligned_stride * static_cast<std::size_t>(stdinfo.height) == total_size)
            ? aligned_stride
            : unaligned_stride;
    if (row_stride * static_cast<std::size_t>(stdinfo.height) > total_size) {
        throw std::runtime_error("HG-3 img0000 decoded buffer smaller than image dimensions");
    }

    for (std::size_t x = bpp_bytes; x < row_stride; ++x) {
        pixel_buf[x] = static_cast<std::uint8_t>(pixel_buf[x] + pixel_buf[x - bpp_bytes]);
    }
    for (std::size_t y = 1; y < static_cast<std::size_t>(stdinfo.height); ++y) {
        std::uint8_t* cur = pixel_buf.data() + y * row_stride;
        const std::uint8_t* prev = pixel_buf.data() + (y - 1) * row_stride;
        for (std::size_t x = 0; x < row_stride; ++x) {
            cur[x] = static_cast<std::uint8_t>(cur[x] + prev[x]);
        }
    }

    ama::imaging::BgraImage img = ama::imaging::MakeBgraImage(
        static_cast<int>(stdinfo.width), static_cast<int>(stdinfo.height));
    for (int y = 0; y < img.height; ++y) {
        const std::size_t wire_y =
            top_down_wire ? static_cast<std::size_t>(y)
                          : static_cast<std::size_t>(img.height - 1 - y);
        const std::uint8_t* src_row = pixel_buf.data() + wire_y * row_stride;
        std::uint8_t* dst_row =
            img.pixels.data() +
            static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) * kBytesPerPixel32;
        if (bpp_bytes == kBytesPerPixel32) {
            std::memcpy(dst_row, src_row,
                        static_cast<std::size_t>(img.width) * kBytesPerPixel32);
        } else {
            for (int x = 0; x < img.width; ++x) {
                dst_row[static_cast<std::size_t>(x) * kBytesPerPixel32 + 0] =
                    src_row[static_cast<std::size_t>(x) * kBytesPerPixel24 + 0];
                dst_row[static_cast<std::size_t>(x) * kBytesPerPixel32 + 1] =
                    src_row[static_cast<std::size_t>(x) * kBytesPerPixel24 + 1];
                dst_row[static_cast<std::size_t>(x) * kBytesPerPixel32 + 2] =
                    src_row[static_cast<std::size_t>(x) * kBytesPerPixel24 + 2];
                dst_row[static_cast<std::size_t>(x) * kBytesPerPixel32 + 3] = kOpaqueAlpha;
            }
        }
    }
    return img;
}

std::vector<std::uint8_t> EncodeImg0000(const Hg3Frame& frame) {
    const std::uint32_t width = frame.stdinfo.width;
    const std::uint32_t height = frame.stdinfo.height;
    if (width == 0 || height == 0) {
        throw std::invalid_argument("cannot encode empty HG-3 frame as img0000");
    }
    if (frame.pixels.width != static_cast<int>(width) ||
        frame.pixels.height != static_cast<int>(height)) {
        throw std::invalid_argument("Hg3Frame pixel dimensions do not match stdinfo");
    }

    const std::size_t bpp_bytes =
        (frame.stdinfo.bpp == kBpp24) ? kBytesPerPixel24 : kBytesPerPixel32;
    const std::size_t unaligned_stride = static_cast<std::size_t>(width) * bpp_bytes;
    const std::size_t row_stride =
        (unaligned_stride + (kRowAlignment - 1)) & ~(kRowAlignment - 1);
    const std::size_t total_size = row_stride * static_cast<std::size_t>(height);

    const bool top_down_wire = frame.top_down_img0000 || IsBootMenuSkinStdInfo(frame.stdinfo);
    std::vector<std::uint8_t> raw_buf(total_size, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t top_down_y =
            top_down_wire ? static_cast<std::size_t>(y)
                          : static_cast<std::size_t>(height - 1U - y);
        const std::uint8_t* src_row =
            frame.pixels.pixels.data() +
            top_down_y * static_cast<std::size_t>(width) * kBytesPerPixel32;
        std::uint8_t* dst_row = raw_buf.data() + static_cast<std::size_t>(y) * row_stride;
        if (bpp_bytes == kBytesPerPixel32) {
            std::memcpy(dst_row, src_row, static_cast<std::size_t>(width) * kBytesPerPixel32);
        } else {
            for (std::uint32_t x = 0; x < width; ++x) {
                dst_row[static_cast<std::size_t>(x) * kBytesPerPixel24 + 0] =
                    src_row[static_cast<std::size_t>(x) * kBytesPerPixel32 + 0];
                dst_row[static_cast<std::size_t>(x) * kBytesPerPixel24 + 1] =
                    src_row[static_cast<std::size_t>(x) * kBytesPerPixel32 + 1];
                dst_row[static_cast<std::size_t>(x) * kBytesPerPixel24 + 2] =
                    src_row[static_cast<std::size_t>(x) * kBytesPerPixel32 + 2];
            }
        }
    }

    std::vector<std::uint8_t> delta_buf(total_size, 0);
    for (std::size_t x = 0; x < bpp_bytes && x < row_stride; ++x) {
        delta_buf[x] = raw_buf[x];
    }
    for (std::size_t x = bpp_bytes; x < row_stride; ++x) {
        delta_buf[x] = static_cast<std::uint8_t>(raw_buf[x] - raw_buf[x - bpp_bytes]);
    }
    for (std::size_t y = 1; y < static_cast<std::size_t>(height); ++y) {
        const std::uint8_t* cur = raw_buf.data() + y * row_stride;
        const std::uint8_t* prev = raw_buf.data() + (y - 1) * row_stride;
        std::uint8_t* dst = delta_buf.data() + y * row_stride;
        for (std::size_t x = 0; x < row_stride; ++x) {
            dst[x] = static_cast<std::uint8_t>(cur[x] - prev[x]);
        }
    }

    const std::size_t sect = total_size / kInterleaveSections;
    std::vector<std::uint8_t> rle_buf(total_size, 0);
    std::uint8_t* s0 = rle_buf.data();
    std::uint8_t* s1 = rle_buf.data() + sect;
    std::uint8_t* s2 = rle_buf.data() + sect * 2;
    std::uint8_t* s3 = rle_buf.data() + sect * 3;

    for (std::size_t i = 0; i < sect; ++i) {
        for (std::size_t k = 0; k < kInterleaveSections; ++k) {
            const int sh = static_cast<int>(k) * kBitsPerPair;
            const std::uint8_t z = ForwardZigZag(delta_buf[i * kInterleaveSections + k]);
            s0[i] = static_cast<std::uint8_t>(
                s0[i] | static_cast<std::uint8_t>(((z >> kSectionShift0) & kTwoBitMask) << sh));
            s1[i] = static_cast<std::uint8_t>(
                s1[i] | static_cast<std::uint8_t>(((z >> kSectionShift1) & kTwoBitMask) << sh));
            s2[i] = static_cast<std::uint8_t>(
                s2[i] | static_cast<std::uint8_t>(((z >> kSectionShift2) & kTwoBitMask) << sh));
            s3[i] = static_cast<std::uint8_t>(
                s3[i] | static_cast<std::uint8_t>(((z >> kSectionShift3) & kTwoBitMask) << sh));
        }
    }

    BitWriter cmd_writer;
    std::vector<std::uint8_t> data_raw;
    data_raw.reserve(total_size / 2);

    bool copy_flag = rle_buf[0] != 0U;
    cmd_writer.WriteBit(copy_flag ? 1U : 0U);
    cmd_writer.WriteGamma(total_size);

    std::size_t pos = 0;
    while (pos < total_size) {
        std::size_t run = 0;
        if (copy_flag) {
            while (pos + run < total_size && rle_buf[pos + run] != 0U) {
                ++run;
            }
            data_raw.insert(data_raw.end(), rle_buf.begin() + static_cast<std::ptrdiff_t>(pos),
                            rle_buf.begin() + static_cast<std::ptrdiff_t>(pos + run));
        } else {
            while (pos + run < total_size && rle_buf[pos + run] == 0U) {
                ++run;
            }
        }
        cmd_writer.WriteGamma(run);
        pos += run;
        copy_flag = !copy_flag;
    }

    const std::vector<std::uint8_t>& cmd_raw = cmd_writer.Bytes();
    const std::vector<std::uint8_t> data_comp = ZlibCompress(data_raw.data(), data_raw.size());
    const std::vector<std::uint8_t> cmd_comp = ZlibCompress(cmd_raw.data(), cmd_raw.size());

    std::vector<std::uint8_t> out(kImg0000HeaderBytes + data_comp.size() + cmd_comp.size(), 0);
    WriteU32Le(out.data() + kImg0000UnknownOffset, frame.img0000_unknown);
    WriteU32Le(out.data() + kImg0000HeightOffset, height);
    WriteU32Le(out.data() + kImg0000DataCompLenOffset,
               static_cast<std::uint32_t>(data_comp.size()));
    WriteU32Le(out.data() + kImg0000DataOrigLenOffset,
               static_cast<std::uint32_t>(data_raw.size()));
    WriteU32Le(out.data() + kImg0000CmdCompLenOffset,
               static_cast<std::uint32_t>(cmd_comp.size()));
    WriteU32Le(out.data() + kImg0000CmdOrigLenOffset,
               static_cast<std::uint32_t>(cmd_raw.size()));

    if (!data_comp.empty()) {
        std::memcpy(out.data() + kImg0000HeaderBytes, data_comp.data(), data_comp.size());
    }
    if (!cmd_comp.empty()) {
        std::memcpy(out.data() + kImg0000HeaderBytes + data_comp.size(), cmd_comp.data(),
                    cmd_comp.size());
    }
    return out;
}

void ApplyImgAlAlpha(ama::imaging::BgraImage& img, const std::uint8_t* payload, std::size_t len) {
    if (len < kImgAlHeaderBytes) {
        throw std::runtime_error("HG-3 img_al chunk is smaller than 8 bytes");
    }
    const std::uint32_t comp_len = ReadU32Le(payload + kImgAlCompLenOffset);
    const std::uint32_t orig_len = ReadU32Le(payload + kImgAlOrigLenOffset);
    if (kImgAlHeaderBytes + static_cast<std::size_t>(comp_len) > len) {
        throw std::runtime_error("HG-3 img_al compressed data exceeds chunk size");
    }

    const std::size_t pixel_count =
        static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height);
    const std::size_t expected = orig_len != 0 ? static_cast<std::size_t>(orig_len) : pixel_count;
    const std::vector<std::uint8_t> alpha =
        ZlibDecompress(payload + kImgAlHeaderBytes, comp_len, expected);
    if (alpha.size() < pixel_count) {
        throw std::runtime_error("HG-3 img_al decompressed buffer smaller than pixel count");
    }

    std::size_t idx = 0;
    for (int y = 0; y < img.height; ++y) {
        std::uint8_t* row =
            img.pixels.data() +
            static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) * kBytesPerPixel32;
        for (int x = 0; x < img.width; ++x) {
            row[static_cast<std::size_t>(x) * kBytesPerPixel32 + 3] = alpha[idx++];
        }
    }
}

void AppendTag(std::vector<std::uint8_t>& dst, const std::string& name,
               const std::vector<std::uint8_t>& payload, bool is_last_tag) {
    const std::size_t tag_start = dst.size();
    dst.resize(tag_start + kTagHeaderBytes + payload.size(), 0);
    const std::size_t copy_name_len = std::min(name.size(), kTagNameFieldBytes);
    std::memcpy(dst.data() + tag_start, name.data(), copy_name_len);
    const std::uint32_t next_rel =
        is_last_tag ? 0U : static_cast<std::uint32_t>(kTagHeaderBytes + payload.size());
    WriteU32Le(dst.data() + tag_start + kTagNextRelOffset, next_rel);
    WriteU32Le(dst.data() + tag_start + kTagDataLenOffset,
               static_cast<std::uint32_t>(payload.size()));
    if (!payload.empty()) {
        std::memcpy(dst.data() + tag_start + kTagHeaderBytes, payload.data(), payload.size());
    }
}

}  // namespace

struct Hg3Codec::Impl {};

Hg3Frame* Hg3Container::FindFrame(std::uint32_t id) noexcept {
    for (auto& f : frames) {
        if (f.frame_id == id) {
            return &f;
        }
    }
    return nullptr;
}

const Hg3Frame* Hg3Container::FindFrame(std::uint32_t id) const noexcept {
    for (const auto& f : frames) {
        if (f.frame_id == id) {
            return &f;
        }
    }
    return nullptr;
}

Hg3Codec::Hg3Codec() : impl_(std::make_unique<Impl>()) {}
Hg3Codec::~Hg3Codec() = default;
Hg3Codec::Hg3Codec(Hg3Codec&&) noexcept = default;
Hg3Codec& Hg3Codec::operator=(Hg3Codec&&) noexcept = default;

Hg3Container Hg3Codec::Decode(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < kFileHeaderBytes) {
        throw std::runtime_error("HG-3 buffer is smaller than 12-byte header");
    }
    if (std::memcmp(data, kHg3Magic, kMagicBytes) != 0) {
        throw std::runtime_error("invalid HG-3 magic signature");
    }
    const std::uint32_t hdr_size = ReadU32Le(data + kHeaderSizeOffset);
    const std::uint32_t version = ReadU32Le(data + kVersionOffset);
    if (hdr_size != kExpectedHeaderSize || version != kExpectedVersion) {
        throw std::runtime_error("unsupported HG-3 header size or version");
    }

    Hg3Container container;
    std::size_t frame_off = kFileHeaderBytes;
    while (frame_off + kFrameHeaderBytes <= size) {
        const std::uint32_t next_frame_rel = ReadU32Le(data + frame_off);
        const std::uint32_t frame_id = ReadU32Le(data + frame_off + kFrameIdOffset);
        const std::size_t frame_end =
            (next_frame_rel != 0U && frame_off + next_frame_rel <= size)
                ? (frame_off + next_frame_rel)
                : size;

        Hg3Frame frame;
        frame.frame_id = frame_id;
        bool has_stdinfo = false;
        const std::uint8_t* pending_img_al = nullptr;
        std::size_t pending_img_al_len = 0;

        std::size_t tag_off = frame_off + kFrameHeaderBytes;
        while (tag_off + kTagHeaderBytes <= frame_end) {
            const std::string tag_name = ParseTagName(data + tag_off);
            if (tag_name.empty()) {
                break;
            }
            const std::uint32_t next_tag_rel = ReadU32Le(data + tag_off + kTagNextRelOffset);
            const std::uint32_t data_len = ReadU32Le(data + tag_off + kTagDataLenOffset);
            const std::size_t payload_off = tag_off + kTagHeaderBytes;
            if (payload_off + data_len > size) {
                throw std::runtime_error("HG-3 tag payload exceeds buffer size");
            }
            const std::uint8_t* payload = data + payload_off;

            if (tag_name == "stdinfo") {
                frame.stdinfo = ParseStdInfo(payload, data_len);
                frame.top_down_img0000 = IsBootMenuSkinStdInfo(frame.stdinfo);
                has_stdinfo = true;
            } else if (tag_name.rfind("img000", 0) == 0) {
                if (!has_stdinfo) {
                    throw std::runtime_error("HG-3 img0000 encountered before stdinfo");
                }
                frame.pixels = DecodeImg0000(frame.stdinfo, payload, data_len,
                                             frame.top_down_img0000, &frame.img0000_unknown);
            } else if (tag_name == "img_jpg") {
                if (!has_stdinfo) {
                    throw std::runtime_error("HG-3 img_jpg encountered before stdinfo");
                }
                const std::vector<std::uint8_t> encoded_jpg(payload, payload + data_len);
                frame.pixels = ama::imaging::DecodeImage(encoded_jpg);
                frame.was_jpeg_encoded = true;
            } else if (tag_name == "img_al") {
                pending_img_al = payload;
                pending_img_al_len = data_len;
            } else if (tag_name == "cptype") {
                if (data_len >= kU32PayloadBytes) {
                    frame.cptype = ReadU32Le(payload);
                }
            } else if (tag_name == "imgmode") {
                if (data_len >= kU32PayloadBytes) {
                    frame.imgmode = ReadU32Le(payload);
                }
            } else {
                Hg3ExtraTag extra;
                extra.name = tag_name;
                extra.payload.assign(payload, payload + data_len);
                frame.extra_tags.push_back(std::move(extra));
            }

            if (next_tag_rel == 0U) {
                break;
            }
            tag_off += next_tag_rel;
        }

        if (pending_img_al != nullptr && !frame.pixels.pixels.empty()) {
            ApplyImgAlAlpha(frame.pixels, pending_img_al, pending_img_al_len);
        }
        frame.has_stdinfo = has_stdinfo;
        container.frames.push_back(std::move(frame));

        if (next_frame_rel == 0U) {
            break;
        }
        frame_off += next_frame_rel;
    }

    return container;
}

Hg3Container Hg3Codec::Decode(const std::vector<std::uint8_t>& bytes) {
    return Decode(bytes.data(), bytes.size());
}

Hg3Container Hg3Codec::DecodeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("cannot open HG-3 file: " + path.u8string());
    }
    const std::streamsize len = in.tellg();
    if (len < 0) {
        throw std::runtime_error("failed to stat HG-3 file: " + path.u8string());
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(len));
    in.seekg(0, std::ios::beg);
    if (len > 0) {
        in.read(reinterpret_cast<char*>(buf.data()), len);
    }
    return Decode(buf);
}

std::vector<std::uint8_t> Hg3Codec::Encode(const Hg3Container& container) {
    std::vector<std::uint8_t> out(kFileHeaderBytes, 0);
    std::memcpy(out.data(), kHg3Magic, kMagicBytes);
    WriteU32Le(out.data() + kHeaderSizeOffset, kExpectedHeaderSize);
    WriteU32Le(out.data() + kVersionOffset, kExpectedVersion);

    for (std::size_t idx = 0; idx < container.frames.size(); ++idx) {
        const Hg3Frame& frame = container.frames[idx];
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> tags;
        if (frame.has_stdinfo) {
            tags.emplace_back("stdinfo", SerializeStdInfo(frame.stdinfo));
            if (frame.stdinfo.width > 0 && frame.stdinfo.height > 0 &&
                !frame.pixels.pixels.empty()) {
                tags.emplace_back("img0000", EncodeImg0000(frame));
            }
        }
        if (frame.cptype.has_value()) {
            std::vector<std::uint8_t> cp_buf(kU32PayloadBytes, 0);
            WriteU32Le(cp_buf.data(), *frame.cptype);
            tags.emplace_back("cptype", std::move(cp_buf));
        }
        if (frame.imgmode.has_value()) {
            std::vector<std::uint8_t> mode_buf(kU32PayloadBytes, 0);
            WriteU32Le(mode_buf.data(), *frame.imgmode);
            tags.emplace_back("imgmode", std::move(mode_buf));
        }
        for (const auto& extra : frame.extra_tags) {
            tags.emplace_back(extra.name, extra.payload);
        }

        std::vector<std::uint8_t> frame_tags_buf;
        for (std::size_t t = 0; t < tags.size(); ++t) {
            const bool is_last_tag = (t + 1 == tags.size());
            AppendTag(frame_tags_buf, tags[t].first, tags[t].second, is_last_tag);
        }

        const bool is_last_frame = (idx + 1 == container.frames.size());
        const std::uint32_t next_frame_rel =
            is_last_frame
                ? 0U
                : static_cast<std::uint32_t>(kFrameHeaderBytes + frame_tags_buf.size());

        const std::size_t frame_start = out.size();
        out.resize(frame_start + kFrameHeaderBytes + frame_tags_buf.size(), 0);
        WriteU32Le(out.data() + frame_start, next_frame_rel);
        WriteU32Le(out.data() + frame_start + kFrameIdOffset, frame.frame_id);
        if (!frame_tags_buf.empty()) {
            std::memcpy(out.data() + frame_start + kFrameHeaderBytes, frame_tags_buf.data(),
                        frame_tags_buf.size());
        }
    }

    return out;
}

void Hg3Codec::EncodeToFile(const Hg3Container& container, const std::filesystem::path& path) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    const std::vector<std::uint8_t> bytes = Encode(container);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot open HG-3 file for writing: " + path.u8string());
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw std::runtime_error("failed writing HG-3 file: " + path.u8string());
    }
}

bool Hg3Codec::VerifyFrameMetadataParity(const Hg3Container& expected,
                                         const Hg3Container& actual) noexcept {
    if (expected.frames.size() != actual.frames.size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected.frames.size(); ++i) {
        const Hg3Frame& e = expected.frames[i];
        const Hg3Frame& a = actual.frames[i];
        if (e.frame_id != a.frame_id || e.has_stdinfo != a.has_stdinfo ||
            e.stdinfo != a.stdinfo || e.cptype != a.cptype || e.imgmode != a.imgmode ||
            e.pixels.width != a.pixels.width || e.pixels.height != a.pixels.height) {
            return false;
        }
    }
    return true;
}

}  // namespace ama::hg3
