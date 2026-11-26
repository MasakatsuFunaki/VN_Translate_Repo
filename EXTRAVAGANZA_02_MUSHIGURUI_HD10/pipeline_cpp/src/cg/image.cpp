// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "image.h"

#include <algorithm>
#include <cstring>
#include <limits>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_JPEG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace mgi::cg {

namespace {

constexpr std::size_t kBmpFileHeaderSize = 14u;
constexpr std::size_t kBmpInfoHeaderSize = 40u;
constexpr std::size_t kBmpHeaderSize = kBmpFileHeaderSize + kBmpInfoHeaderSize;
constexpr std::size_t kBmpOffBitsOffset = 10u;
constexpr std::size_t kBmpDibSizeOffset = 14u;
constexpr std::size_t kBmpWidthOffset = 18u;
constexpr std::size_t kBmpHeightOffset = 22u;
constexpr std::size_t kBmpBitCountOffset = 28u;
constexpr std::size_t kBmpCompressionOffset = 30u;
constexpr std::uint32_t kBmpCompressionRgb = 0u;
constexpr std::uint16_t kBmpPlanes = 1u;
constexpr std::uint16_t kBmp32BitCount = 32u;
constexpr int kRgbaChannels = 4;
constexpr unsigned int kByteShift1 = 8u;
constexpr unsigned int kByteShift2 = 16u;
constexpr unsigned int kByteShift3 = 24u;

std::uint16_t rd_u16le(const Bytes& d, std::size_t off) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(d[off]) |
        (static_cast<std::uint16_t>(d[off + 1u]) << kByteShift1));
}

std::uint32_t rd_u32le(const Bytes& d, std::size_t off) noexcept {
    return static_cast<std::uint32_t>(d[off]) |
           (static_cast<std::uint32_t>(d[off + 1u]) << kByteShift1) |
           (static_cast<std::uint32_t>(d[off + 2u]) << kByteShift2) |
           (static_cast<std::uint32_t>(d[off + 3u]) << kByteShift3);
}

std::int32_t rd_i32le(const Bytes& d, std::size_t off) noexcept {
    return static_cast<std::int32_t>(rd_u32le(d, off));
}

void put_u16le(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> kByteShift1));
}

void put_u32le(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> kByteShift1));
    out.push_back(static_cast<std::uint8_t>(v >> kByteShift2));
    out.push_back(static_cast<std::uint8_t>(v >> kByteShift3));
}

void put_i32le(Bytes& out, std::int32_t v) {
    put_u32le(out, static_cast<std::uint32_t>(v));
}

bool is_bmp32_bi_rgb(const Bytes& d) noexcept {
    if (d.size() < kBmpHeaderSize || d[0] != 'B' || d[1] != 'M') {
        return false;
    }
    const std::uint32_t dib_size = rd_u32le(d, kBmpDibSizeOffset);
    const std::uint16_t bpp = rd_u16le(d, kBmpBitCountOffset);
    const std::uint32_t compression = rd_u32le(d, kBmpCompressionOffset);
    return dib_size >= kBmpInfoHeaderSize && bpp == kBmp32BitCount &&
           compression == kBmpCompressionRgb;
}

// Decode uncompressed 32-bpp BI_RGB BMP directly so all 4 straight BGRA
// channels (including all-zero alpha) survive without stb's BGRX heuristic.
std::optional<Image> try_decode_bmp32_bi_rgb(const Bytes& d) {
    if (!is_bmp32_bi_rgb(d)) {
        return std::nullopt;
    }
    const std::uint32_t offbits = rd_u32le(d, kBmpOffBitsOffset);
    const std::int32_t w = rd_i32le(d, kBmpWidthOffset);
    const std::int32_t raw_h = rd_i32le(d, kBmpHeightOffset);
    const bool top_down = raw_h < 0;
    const std::int64_t h64 = top_down ? -static_cast<std::int64_t>(raw_h)
                                      : static_cast<std::int64_t>(raw_h);
    if (w <= 0 || h64 <= 0 || h64 > std::numeric_limits<int>::max() ||
        offbits < kBmpHeaderSize || offbits > d.size()) {
        return std::nullopt;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(w) * kRgbaChannels;
    if (static_cast<std::size_t>(h64) > (d.size() - offbits) / row_bytes) {
        return std::nullopt;
    }
    const std::size_t px_bytes = row_bytes * static_cast<std::size_t>(h64);

    Image img;
    img.width = w;
    img.height = static_cast<int>(h64);
    img.channels = kRgbaChannels;
    img.px.resize(px_bytes);
    for (int y = 0; y < img.height; ++y) {
        const int src_y = top_down ? y : (img.height - 1 - y);
        const std::uint8_t* src_row =
            d.data() + offbits + static_cast<std::size_t>(src_y) * row_bytes;
        std::uint8_t* dst_row = img.px.data() + static_cast<std::size_t>(y) * row_bytes;
        for (int x = 0; x < img.width; ++x) {
            const std::size_t p = static_cast<std::size_t>(x) * kRgbaChannels;
            dst_row[p + 0u] = src_row[p + 2u];
            dst_row[p + 1u] = src_row[p + 1u];
            dst_row[p + 2u] = src_row[p + 0u];
            dst_row[p + 3u] = src_row[p + 3u];
        }
    }
    return img;
}

// Channel count to decode to, read from the CONTAINER HEADER.  Returns 0 for
// anything we refuse to open, which the caller turns into "not an image".
int container_channels(const Bytes& d) {
    static const std::uint8_t PNG_MAGIC[8] = {0x89, 'P', 'N', 'G',
                                              0x0D, 0x0A, 0x1A, 0x0A};
    if (d.size() >= 26 && std::memcmp(d.data(), PNG_MAGIC, 8) == 0) {
        const std::uint8_t colour_type = d[0x19];  // IHDR + 9
        switch (colour_type) {
        case 2: return 3;              // truecolour
        case 6: return 4;              // truecolour + alpha
        case 0: case 3: case 4: return 4;  // grey / palette / grey+alpha -> RGBA
        default: return 0;
        }
    }
    if (d.size() >= 30 && d[0] == 'B' && d[1] == 'M') {
        const int bpp = d[0x1C] | (d[0x1D] << 8);
        switch (bpp) {
        case 1: case 4: case 8: return 4;  // palettised -> RGBA
        case 16: case 24: return 3;        // 16/24-bpp -> RGB
        case 32: return 4;                 // 32-bpp BGRA -> straight RGBA
        default: return 0;
        }
    }
    if (d.size() >= 2 && d[0] == 0xFF && d[1] == 0xD8) return 3;  // JPEG -> RGB
    return 0;
}

void append_bytes(void* ctx, void* data, int size) {
    auto* out = static_cast<Bytes*>(ctx);
    const auto* p = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}

}  // namespace

std::optional<Image> decode_image(const Bytes& raw) {
    const int channels = container_channels(raw);
    if (channels == 0) return std::nullopt;

    if (is_bmp32_bi_rgb(raw)) {
        return try_decode_bmp32_bi_rgb(raw);
    }

    int w = 0, h = 0, comp = 0;
    stbi_uc* data = stbi_load_from_memory(raw.data(), static_cast<int>(raw.size()),
                                          &w, &h, &comp, channels);
    if (!data) return std::nullopt;

    Image img;
    img.width = w;
    img.height = h;
    img.channels = channels;
    img.px.assign(data, data + static_cast<std::size_t>(w) * h * channels);
    stbi_image_free(data);
    return img;
}

std::optional<Image> open_image(const std::string& path) {
    Bytes raw;
    try {
        raw = read_file(path);
    } catch (...) {
        return std::nullopt;  // unreadable file is "not an image", not a fault
    }
    return decode_image(raw);
}

Image crop(const Image& img, int x1, int y1, int x2, int y2) {
    x1 = std::max(0, x1);
    y1 = std::max(0, y1);
    x2 = std::min(img.width, x2);
    y2 = std::min(img.height, y2);

    Image out;
    out.channels = img.channels;
    out.width = std::max(0, x2 - x1);
    out.height = std::max(0, y2 - y1);
    out.px.resize(static_cast<std::size_t>(out.width) * out.height * out.channels);
    const std::size_t row = static_cast<std::size_t>(out.width) * out.channels;
    for (int y = 0; y < out.height; ++y)
        std::memcpy(out.px.data() + static_cast<std::size_t>(y) * row,
                    img.at(x1, y1 + y), row);
    return out;
}

Image resize(const Image& img, int new_w, int new_h) {
    Image out;
    out.channels = img.channels;
    out.width = new_w;
    out.height = new_h;
    out.px.resize(static_cast<std::size_t>(new_w) * new_h * out.channels);
    for (int y = 0; y < new_h; ++y) {
        double sy = (y + 0.5) * img.height / new_h - 0.5;
        int y0 = std::max(0, std::min(img.height - 1, static_cast<int>(sy)));
        int y1 = std::min(img.height - 1, y0 + 1);
        double fy = std::max(0.0, sy - y0);
        for (int x = 0; x < new_w; ++x) {
            double sx = (x + 0.5) * img.width / new_w - 0.5;
            int x0 = std::max(0, std::min(img.width - 1, static_cast<int>(sx)));
            int x1 = std::min(img.width - 1, x0 + 1);
            double fx = std::max(0.0, sx - x0);
            std::uint8_t* d = out.at(x, y);
            for (int c = 0; c < out.channels; ++c) {
                double v = (1 - fy) * ((1 - fx) * img.at(x0, y0)[c] + fx * img.at(x1, y0)[c]) +
                           fy * ((1 - fx) * img.at(x0, y1)[c] + fx * img.at(x1, y1)[c]);
                d[c] = static_cast<std::uint8_t>(v + 0.5);
            }
        }
    }
    return out;
}

Bytes encode_png(const Image& img) {
    Bytes out;
    stbi_write_png_to_func(append_bytes, &out, img.width, img.height, img.channels,
                           img.px.data(), img.width * img.channels);
    return out;
}

Bytes encode_bmp(const Image& img) {
    Bytes out;
    if (img.channels == kRgbaChannels) {
        const std::size_t px_bytes =
            static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height) *
            kRgbaChannels;
        const std::uint32_t file_size = static_cast<std::uint32_t>(kBmpHeaderSize + px_bytes);
        out.reserve(file_size);
        out.push_back('B');
        out.push_back('M');
        put_u32le(out, file_size);
        put_u16le(out, 0u);
        put_u16le(out, 0u);
        put_u32le(out, static_cast<std::uint32_t>(kBmpHeaderSize));
        put_u32le(out, static_cast<std::uint32_t>(kBmpInfoHeaderSize));
        put_i32le(out, img.width);
        put_i32le(out, img.height);  // bottom-up 32-bpp BGRA
        put_u16le(out, kBmpPlanes);
        put_u16le(out, kBmp32BitCount);
        put_u32le(out, kBmpCompressionRgb);
        put_u32le(out, static_cast<std::uint32_t>(px_bytes));
        put_i32le(out, 0);
        put_i32le(out, 0);
        put_u32le(out, 0u);
        put_u32le(out, 0u);

        for (int y = img.height - 1; y >= 0; --y) {
            for (int x = 0; x < img.width; ++x) {
                const std::uint8_t* p = img.at(x, y);
                out.push_back(p[2]);  // B
                out.push_back(p[1]);  // G
                out.push_back(p[0]);  // R
                out.push_back(p[3]);  // A
            }
        }
        return out;
    }

    stbi_write_bmp_to_func(append_bytes, &out, img.width, img.height, 3,
                           img.px.data());
    return out;
}

Bytes encode_jpg(const Image& img, int quality) {
    Bytes out;
    const Image rgb = img.channels == 4 ? drop_alpha(img) : img;
    stbi_write_jpg_to_func(append_bytes, &out, rgb.width, rgb.height, 3,
                           rgb.px.data(), quality);
    return out;
}

Image drop_alpha(const Image& img) {
    if (img.channels == 3) return img;
    Image out;
    out.width = img.width;
    out.height = img.height;
    out.channels = 3;
    const std::size_t n = static_cast<std::size_t>(img.width) * img.height;
    out.px.resize(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        out.px[i * 3 + 0] = img.px[i * 4 + 0];
        out.px[i * 3 + 1] = img.px[i * 4 + 1];
        out.px[i * 3 + 2] = img.px[i * 4 + 2];
    }
    return out;
}

std::string base64_encode(const Bytes& data) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
        i += 3;
    }
    if (i + 1 == data.size()) {
        std::uint32_t v = data[i] << 16;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

}  // namespace mgi::cg
