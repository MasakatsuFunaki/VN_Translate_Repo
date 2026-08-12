// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "image.h"

#include <algorithm>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_JPEG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace shin::cg {

namespace {

// Channel count to decode a container to, taken from its header.  Returns 0
// for a file this step refuses to open at all.
int pil_channels(const Bytes& d) {
    static const std::uint8_t PNG_MAGIC[8] = {0x89, 'P', 'N', 'G',
                                              0x0D, 0x0A, 0x1A, 0x0A};
    if (d.size() >= 26 && std::memcmp(d.data(), PNG_MAGIC, 8) == 0) {
        const std::uint8_t colour_type = d[0x19];  // IHDR + 9
        switch (colour_type) {
        case 2: return 3;              // truecolour       -> 'RGB'
        case 6: return 4;              // truecolour+alpha -> 'RGBA'
        case 0: case 3: case 4: return 4;  // 'L' / 'P' / 'LA' -> convert('RGBA')
        default: return 0;
        }
    }
    if (d.size() >= 30 && d[0] == 'B' && d[1] == 'M') {
        const int bpp = d[0x1C] | (d[0x1D] << 8);
        switch (bpp) {
        case 1: case 4: case 8: return 4;   // '1' / 'P' -> convert('RGBA')
        case 16: case 24: case 32: return 3;  // 'RGB' (32bpp BGRX drops alpha)
        default: return 0;
        }
    }
    if (d.size() >= 2 && d[0] == 0xFF && d[1] == 0xD8) return 3;  // JPEG -> 'RGB'
    return 0;
}

void append_bytes(void* ctx, void* data, int size) {
    auto* out = static_cast<Bytes*>(ctx);
    const auto* p = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}

}  // namespace

std::optional<Image> decode_image(const Bytes& raw) {
    const int channels = pil_channels(raw);
    if (channels == 0) return std::nullopt;

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
        return std::nullopt;  // unreadable file is "no image", not an error
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
    // The 24-bit BMP the game reads has no alpha channel; the caller is
    // expected to have dropped it already.
    const Image rgb = img.channels == 4 ? drop_alpha(img) : img;
    stbi_write_bmp_to_func(append_bytes, &out, rgb.width, rgb.height, 3,
                           rgb.px.data());
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

}  // namespace shin::cg
