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
#define STBI_ONLY_BMP
#define STBI_ONLY_PNG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace snt::cg {

std::optional<Image> decode_image(const Bytes& raw) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* data = stbi_load_from_memory(raw.data(), static_cast<int>(raw.size()),
                                          &w, &h, &comp, 3);
    if (!data) return std::nullopt;
    Image img;
    img.width = w;
    img.height = h;
    img.rgb.assign(data, data + static_cast<std::size_t>(w) * h * 3);
    stbi_image_free(data);
    return img;
}

Image crop(const Image& img, int x1, int y1, int x2, int y2) {
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(img.width, x2); y2 = std::min(img.height, y2);
    Image out;
    out.width = std::max(0, x2 - x1);
    out.height = std::max(0, y2 - y1);
    out.rgb.resize(static_cast<std::size_t>(out.width) * out.height * 3);
    for (int y = 0; y < out.height; ++y)
        std::memcpy(out.rgb.data() + static_cast<std::size_t>(y) * out.width * 3,
                    img.px(x1, y1 + y), static_cast<std::size_t>(out.width) * 3);
    return out;
}

Image resize(const Image& img, int new_w, int new_h) {
    Image out;
    out.width = new_w;
    out.height = new_h;
    out.rgb.resize(static_cast<std::size_t>(new_w) * new_h * 3);
    for (int y = 0; y < new_h; ++y) {
        double sy = (static_cast<double>(y) + 0.5) * img.height / new_h - 0.5;
        int y0 = static_cast<int>(sy);
        y0 = std::max(0, std::min(img.height - 1, y0));
        int y1 = std::min(img.height - 1, y0 + 1);
        double fy = sy - y0;
        if (fy < 0) fy = 0;
        for (int x = 0; x < new_w; ++x) {
            double sx = (static_cast<double>(x) + 0.5) * img.width / new_w - 0.5;
            int x0 = static_cast<int>(sx);
            x0 = std::max(0, std::min(img.width - 1, x0));
            int x1 = std::min(img.width - 1, x0 + 1);
            double fx = sx - x0;
            if (fx < 0) fx = 0;
            std::uint8_t* d = out.px(x, y);
            for (int c = 0; c < 3; ++c) {
                double v = (1 - fy) * ((1 - fx) * img.px(x0, y0)[c] + fx * img.px(x1, y0)[c]) +
                           fy * ((1 - fx) * img.px(x0, y1)[c] + fx * img.px(x1, y1)[c]);
                d[c] = static_cast<std::uint8_t>(v + 0.5);
            }
        }
    }
    return out;
}

namespace {
void append_bytes(void* ctx, void* data, int size) {
    auto* out = static_cast<Bytes*>(ctx);
    const auto* p = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}
}  // namespace

Bytes encode_png(const Image& img) {
    Bytes out;
    stbi_write_png_to_func(append_bytes, &out, img.width, img.height, 3,
                           img.rgb.data(), img.width * 3);
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

}  // namespace snt::cg
