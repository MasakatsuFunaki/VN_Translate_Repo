// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "cg/image.h"

#include "bgi/bgi_cbg.h"
#include "menu/bgra_image.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_BMP
#define STBI_ONLY_PNG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace snu::cg {

namespace {

constexpr std::size_t kRgbChannels = 3;
constexpr std::size_t kRgbaChannels = 4;
constexpr int kGrayAlphaChannels = 2;
constexpr int kRgbChannelsInt = 3;
constexpr int kRgbaChannelsInt = 4;
constexpr std::size_t kRgbRedOffset = 0;
constexpr std::size_t kRgbGreenOffset = 1;
constexpr std::size_t kRgbBlueOffset = 2;
constexpr std::size_t kRgbaAlphaOffset = 3;
constexpr std::size_t kBgraBlueOffset = 0;
constexpr std::size_t kBgraGreenOffset = 1;
constexpr std::size_t kBgraRedOffset = 2;
constexpr std::size_t kBgraAlphaOffset = 3;

constexpr std::size_t kBmpFileHeaderBytes = 14;
constexpr std::size_t kBmpInfoHeaderBytes = 40;
constexpr std::size_t kBmpTotalHeaderBytes =
    kBmpFileHeaderBytes + kBmpInfoHeaderBytes;
constexpr std::size_t kBmpDibSizeOffset = 14;
constexpr std::size_t kBmpBppOffset = 28;
constexpr std::size_t kBmpCompressionOffset = 30;
constexpr std::uint32_t kBmpCompressionBitfields = 3u;
constexpr std::uint32_t kBmpBitfieldsMinDibBytes = 56u;
constexpr std::size_t kBmpAlphaMaskOffset = 66;
constexpr int kBitsPerByte = 8;

std::uint16_t read_le16(const Bytes& raw, std::size_t off) noexcept {
    std::uint16_t v = 0;
    std::memcpy(&v, raw.data() + off, sizeof(v));
    return v;
}

std::uint32_t read_le32(const Bytes& raw, std::size_t off) noexcept {
    std::uint32_t v = 0;
    std::memcpy(&v, raw.data() + off, sizeof(v));
    return v;
}

bool is_valid_bmp_info_header(const Bytes& raw) noexcept {
    return raw.size() >= kBmpTotalHeaderBytes && raw[0] == 'B' &&
           raw[1] == 'M' && read_le32(raw, kBmpDibSizeOffset) >= kBmpInfoHeaderBytes;
}

bool bmp_carries_alpha_channel(const Bytes& raw, std::uint16_t bpp) noexcept {
    if (bpp != snu::menu::kBmpBpp32) {
        return false;
    }
    const std::uint32_t dib_size = read_le32(raw, kBmpDibSizeOffset);
    const std::uint32_t compression = read_le32(raw, kBmpCompressionOffset);
    if (compression == kBmpCompressionBitfields &&
        dib_size >= kBmpBitfieldsMinDibBytes &&
        raw.size() >= kBmpFileHeaderBytes + kBmpBitfieldsMinDibBytes) {
        return read_le32(raw, kBmpAlphaMaskOffset) != 0u;
    }
    return true;
}

Image from_bgra_image(const snu::menu::BgraImage& bgra, bool keep_alpha) {
    Image img;
    img.width = bgra.width();
    img.height = bgra.height();
    const std::size_t pixel_count =
        static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height);
    img.rgb.resize(pixel_count * kRgbChannels);
    if (keep_alpha) {
        img.alpha.resize(pixel_count);
    }
    const std::uint8_t* src = bgra.data();
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const std::uint8_t* s = src + i * kRgbaChannels;
        std::uint8_t* d = img.rgb.data() + i * kRgbChannels;
        d[kRgbRedOffset] = s[kBgraRedOffset];
        d[kRgbGreenOffset] = s[kBgraGreenOffset];
        d[kRgbBlueOffset] = s[kBgraBlueOffset];
        if (keep_alpha) {
            img.alpha[i] = s[kBgraAlphaOffset];
        }
    }
    return img;
}

void append_bytes(void* ctx, void* data, int size) {
    if (size <= 0) {
        return;
    }
    auto* out = static_cast<Bytes*>(ctx);
    const auto* p = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}

}  // namespace

std::optional<Image> decode_image(const Bytes& raw) {
    if (raw.empty()) {
        return std::nullopt;
    }
    if (is_valid_bmp_info_header(raw)) {
        const std::uint16_t bpp = read_le16(raw, kBmpBppOffset);
        if (bpp == snu::menu::kBmpBpp24 || bpp == snu::menu::kBmpBpp32) {
            try {
                const snu::menu::BgraImage bgra =
                    snu::menu::BgraImage::DecodeBmpOrPng(raw);
                return from_bgra_image(bgra, bmp_carries_alpha_channel(raw, bpp));
            } catch (...) {
                // Fall through to stb_image for non-BGRA bitfields or unusual DIBs.
            }
        }
    }

    int info_w = 0;
    int info_h = 0;
    int info_comp = 0;
    const bool has_info =
        stbi_info_from_memory(raw.data(), static_cast<int>(raw.size()),
                              &info_w, &info_h, &info_comp) != 0;
    const bool keep_alpha =
        has_info && (info_comp == kRgbaChannelsInt || info_comp == kGrayAlphaChannels);
    const int req_comp = keep_alpha ? kRgbaChannelsInt : kRgbChannelsInt;

    int w = 0;
    int h = 0;
    int comp = 0;
    stbi_uc* data = stbi_load_from_memory(
        raw.data(), static_cast<int>(raw.size()), &w, &h, &comp, req_comp);
    if (!data) {
        return std::nullopt;
    }

    Image img;
    img.width = w;
    img.height = h;
    const std::size_t pixel_count =
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (keep_alpha) {
        img.rgb.resize(pixel_count * kRgbChannels);
        img.alpha.resize(pixel_count);
        for (std::size_t i = 0; i < pixel_count; ++i) {
            const stbi_uc* s = data + i * kRgbaChannels;
            std::uint8_t* d = img.rgb.data() + i * kRgbChannels;
            d[kRgbRedOffset] = s[kRgbRedOffset];
            d[kRgbGreenOffset] = s[kRgbGreenOffset];
            d[kRgbBlueOffset] = s[kRgbBlueOffset];
            img.alpha[i] = s[kRgbaAlphaOffset];
        }
    } else {
        img.rgb.assign(data, data + pixel_count * kRgbChannels);
    }
    stbi_image_free(data);
    return img;
}

Image from_cbg_pixels(const Bytes& bgr, int width, int height, int bpp) {
    const int px = (bpp + (kBitsPerByte - 1)) / kBitsPerByte;
    const std::size_t pixel_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const bool has_alpha = (px >= kRgbaChannelsInt);

    Image img;
    img.width = width;
    img.height = height;
    img.rgb.resize(pixel_count * kRgbChannels);
    if (has_alpha) {
        img.alpha.resize(pixel_count);
    }
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const std::uint8_t* s = bgr.data() + i * static_cast<std::size_t>(px);
        std::uint8_t* d = img.rgb.data() + i * kRgbChannels;
        d[kRgbRedOffset] = px >= kRgbChannelsInt ? s[kBgraRedOffset] : s[0];
        d[kRgbGreenOffset] = px >= kRgbChannelsInt ? s[kBgraGreenOffset] : s[0];
        d[kRgbBlueOffset] = s[kBgraBlueOffset];
        if (has_alpha) {
            img.alpha[i] = s[kBgraAlphaOffset];
        }
    }
    return img;
}

Image crop(const Image& img, int x1, int y1, int x2, int y2) {
    if (img.empty()) {
        return {};
    }
    x1 = std::max(0, x1);
    y1 = std::max(0, y1);
    x2 = std::min(img.width, x2);
    y2 = std::min(img.height, y2);
    Image out;
    out.width = std::max(0, x2 - x1);
    out.height = std::max(0, y2 - y1);
    if (out.empty()) {
        return {};
    }
    const std::size_t out_row_rgb =
        static_cast<std::size_t>(out.width) * kRgbChannels;
    out.rgb.resize(out_row_rgb * static_cast<std::size_t>(out.height));
    const bool copy_alpha = img.has_alpha();
    if (copy_alpha) {
        out.alpha.resize(static_cast<std::size_t>(out.width) *
                         static_cast<std::size_t>(out.height));
    }
    for (int y = 0; y < out.height; ++y) {
        std::memcpy(out.rgb.data() + static_cast<std::size_t>(y) * out_row_rgb,
                    img.px(x1, y1 + y), out_row_rgb);
        if (copy_alpha) {
            std::memcpy(
                out.alpha.data() +
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width),
                img.alpha_px(x1, y1 + y), static_cast<std::size_t>(out.width));
        }
    }
    return out;
}

Image resize(const Image& img, int new_w, int new_h) {
    if (img.empty() || new_w <= 0 || new_h <= 0) {
        return {};
    }
    Image out;
    out.width = new_w;
    out.height = new_h;
    const std::size_t out_pixels =
        static_cast<std::size_t>(new_w) * static_cast<std::size_t>(new_h);
    out.rgb.resize(out_pixels * kRgbChannels);
    const bool scale_alpha = img.has_alpha();
    if (scale_alpha) {
        out.alpha.resize(out_pixels);
    }
    for (int y = 0; y < new_h; ++y) {
        const double sy =
            (static_cast<double>(y) + 0.5) * img.height / new_h - 0.5;
        const int y0 = std::clamp(static_cast<int>(sy), 0, img.height - 1);
        const int y1 = std::min(img.height - 1, y0 + 1);
        const double fy = std::max(0.0, sy - y0);
        for (int x = 0; x < new_w; ++x) {
            const double sx =
                (static_cast<double>(x) + 0.5) * img.width / new_w - 0.5;
            const int x0 = std::clamp(static_cast<int>(sx), 0, img.width - 1);
            const int x1 = std::min(img.width - 1, x0 + 1);
            const double fx = std::max(0.0, sx - x0);
            std::uint8_t* d = out.px(x, y);
            for (int c = 0; c < kRgbChannelsInt; ++c) {
                const double v =
                    (1.0 - fy) * ((1.0 - fx) * img.px(x0, y0)[c] +
                                  fx * img.px(x1, y0)[c]) +
                    fy * ((1.0 - fx) * img.px(x0, y1)[c] +
                          fx * img.px(x1, y1)[c]);
                d[c] = static_cast<std::uint8_t>(v + 0.5);
            }
            if (scale_alpha) {
                const double a =
                    (1.0 - fy) * ((1.0 - fx) * (*img.alpha_px(x0, y0)) +
                                  fx * (*img.alpha_px(x1, y0))) +
                    fy * ((1.0 - fx) * (*img.alpha_px(x0, y1)) +
                          fx * (*img.alpha_px(x1, y1)));
                *out.alpha_px(x, y) = static_cast<std::uint8_t>(a + 0.5);
            }
        }
    }
    return out;
}

Bytes encode_png(const Image& img) {
    Bytes out;
    stbi_write_png_to_func(append_bytes, &out, img.width, img.height,
                           kRgbChannelsInt, img.rgb.data(),
                           img.width * kRgbChannelsInt);
    return out;
}

Bytes encode_bmp(const Image& img) {
    const std::size_t pixel_count =
        static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height);
    if (img.has_alpha()) {
        Bytes bgra(pixel_count * kRgbaChannels);
        for (std::size_t i = 0; i < pixel_count; ++i) {
            const std::size_t s = i * kRgbChannels;
            const std::size_t d = i * kRgbaChannels;
            bgra[d + kBgraBlueOffset] = img.rgb[s + kRgbBlueOffset];
            bgra[d + kBgraGreenOffset] = img.rgb[s + kRgbGreenOffset];
            bgra[d + kBgraRedOffset] = img.rgb[s + kRgbRedOffset];
            bgra[d + kBgraAlphaOffset] = img.alpha[i];
        }
        return bgi::to_bmp(bgra, img.width, img.height, snu::menu::kBmpBpp32);
    }

    Bytes bgr(img.rgb.size());
    for (std::size_t i = 0; i + kRgbBlueOffset < img.rgb.size();
         i += kRgbChannels) {
        bgr[i + kBgraBlueOffset] = img.rgb[i + kRgbBlueOffset];
        bgr[i + kBgraGreenOffset] = img.rgb[i + kRgbGreenOffset];
        bgr[i + kBgraRedOffset] = img.rgb[i + kRgbRedOffset];
    }
    return bgi::to_bmp(bgr, img.width, img.height, snu::menu::kBmpBpp24);
}

std::string base64_encode(const Bytes& data) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const std::uint32_t v =
            (static_cast<std::uint32_t>(data[i]) << 16) |
            (static_cast<std::uint32_t>(data[i + 1]) << 8) |
            static_cast<std::uint32_t>(data[i + 2]);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
        i += 3;
    }
    if (i + 1 == data.size()) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        const std::uint32_t v =
            (static_cast<std::uint32_t>(data[i]) << 16) |
            (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

}  // namespace snu::cg
