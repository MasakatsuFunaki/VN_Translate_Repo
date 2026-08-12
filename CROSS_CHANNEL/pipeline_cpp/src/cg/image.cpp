// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "image.h"

#include <algorithm>
#include <cstring>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_BMP
#define STBI_ONLY_PNG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace crc::cg {

std::optional<Image> decode_image(const Bytes& raw) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* data = stbi_load_from_memory(raw.data(), static_cast<int>(raw.size()), &w, &h, &comp, 3);
    if (!data) return std::nullopt;
    Image img;
    img.width = w;
    img.height = h;
    img.rgb.assign(data, data + static_cast<std::size_t>(w) * h * 3);
    stbi_image_free(data);
    return img;
}

Image crop(const Image& img, int x1, int y1, int x2, int y2) {
    x1 = std::max(0, x1);
    y1 = std::max(0, y1);
    x2 = std::min(img.width, x2);
    y2 = std::min(img.height, y2);
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
    stbi_write_png_to_func(append_bytes, &out, img.width, img.height, 3, img.rgb.data(),
                           img.width * 3);
    return out;
}

Bytes encode_bmp(const Image& img) {
    // Hand-rolled rather than stbi_write_bmp_to_func: stb leaves biSizeImage
    // and the pixels-per-metre fields zero.  The BMPs already sitting in
    // script_output/narrative_patched carry the real image size and 3780
    // (96 dpi x 39.3701, rounded), so the header written here has to match
    // them byte for byte or a rerun rewrites every file for no reason.
    const std::size_t stride = (static_cast<std::size_t>(img.width) * 3 + 3) & ~std::size_t{3};
    const std::size_t image_bytes = stride * static_cast<std::size_t>(img.height);
    const std::uint32_t offset = 14 + 40;

    Bytes out;
    out.reserve(offset + image_bytes);
    auto u16 = [&out](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v & 0xFF));
        out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    };
    auto u32 = [&out](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    };

    out.push_back('B');
    out.push_back('M');
    u32(offset + static_cast<std::uint32_t>(image_bytes));
    u32(0);
    u32(offset);
    u32(40);  // BITMAPINFOHEADER
    u32(static_cast<std::uint32_t>(img.width));
    u32(static_cast<std::uint32_t>(img.height));
    u16(1);   // planes
    u16(24);  // bits per pixel
    u32(0);   // BI_RGB
    u32(static_cast<std::uint32_t>(image_bytes));
    u32(3780);  // X pixels per metre
    u32(3780);  // Y pixels per metre
    u32(0);     // colours used
    u32(0);     // colours important

    for (int y = img.height - 1; y >= 0; --y) {  // bottom-up
        for (int x = 0; x < img.width; ++x) {
            const std::uint8_t* p = img.px(x, y);
            out.push_back(p[2]);  // B
            out.push_back(p[1]);  // G
            out.push_back(p[0]);  // R
        }
        for (std::size_t pad = static_cast<std::size_t>(img.width) * 3; pad < stride; ++pad)
            out.push_back(0);
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

void fill_rect(Image& img, int x1, int y1, int x2, int y2, Rgb color) {
    x1 = std::max(0, x1);
    y1 = std::max(0, y1);
    x2 = std::min(img.width - 1, x2);   // x2/y2 are INCLUSIVE (see image.h)
    y2 = std::min(img.height - 1, y2);
    for (int y = y1; y <= y2; ++y)
        for (int x = x1; x <= x2; ++x) std::memcpy(img.px(x, y), color.data(), 3);
}

Rgb sample_bg(const Image& img, int x1, int y1, int x2, int y2) {
    const Image arr = crop(img, x1, y1, x2, y2);
    const int h = arr.height, w = arr.width;
    std::vector<std::array<std::uint8_t, 3>> edges;

    if (h > 4) {
        for (int y : {0, 1})
            for (int x = 0; x < w; ++x)
                edges.push_back({arr.px(x, y)[0], arr.px(x, y)[1], arr.px(x, y)[2]});
        for (int y : {h - 2, h - 1})
            for (int x = 0; x < w; ++x)
                edges.push_back({arr.px(x, y)[0], arr.px(x, y)[1], arr.px(x, y)[2]});
    }
    if (w > 4) {
        // Corner pixels get counted twice here.  That is deliberate: the
        // sample size and therefore the median shift if you dedupe them.
        for (int y = 0; y < h; ++y)
            for (int x : {0, 1})
                edges.push_back({arr.px(x, y)[0], arr.px(x, y)[1], arr.px(x, y)[2]});
        for (int y = 0; y < h; ++y)
            for (int x : {w - 2, w - 1})
                edges.push_back({arr.px(x, y)[0], arr.px(x, y)[1], arr.px(x, y)[2]});
    }
    if (edges.empty()) return Rgb{255, 255, 255};

    Rgb med{};
    for (int c = 0; c < 3; ++c) {
        std::vector<int> vals;
        vals.reserve(edges.size());
        for (const auto& e : edges) vals.push_back(e[c]);
        std::sort(vals.begin(), vals.end());
        const std::size_t n = vals.size();
        const double m = (n % 2) ? static_cast<double>(vals[n / 2])
                                 : (static_cast<double>(vals[n / 2 - 1]) +
                                    static_cast<double>(vals[n / 2])) / 2.0;
        med[static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(static_cast<int>(m));
    }
    return med;
}

}  // namespace crc::cg
