// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Minimal RGB8 / straight-alpha RGBA8 image type for the narrative-CG pipeline:
// BMP/PNG/CBG decode, crop, resize, PNG/BMP encode.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "common/util.h"

namespace snu::cg {

struct Image {
    int width = 0;
    int height = 0;
    Bytes rgb;    // row-major, 3 bytes/pixel (R, G, B)
    Bytes alpha;  // optional row-major, 1 byte/pixel (empty => opaque 24-bpp)

    bool empty() const noexcept { return width <= 0 || height <= 0; }
    bool has_alpha() const noexcept {
        return !empty() &&
               alpha.size() == static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height);
    }
    std::uint8_t* px(int x, int y) {
        return rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
    }
    const std::uint8_t* px(int x, int y) const {
        return rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
    }
    std::uint8_t* alpha_px(int x, int y) {
        return alpha.data() + static_cast<std::size_t>(y) * width + x;
    }
    const std::uint8_t* alpha_px(int x, int y) const {
        return alpha.data() + static_cast<std::size_t>(y) * width + x;
    }
};

// Decode a BMP / PNG blob, preserving straight alpha when the source carries
// an alpha channel (32-bpp BMP or 4-channel PNG).
std::optional<Image> decode_image(const Bytes& raw);

// Convert CBG decoder output (row-major BGR or BGRA) to Image, preserving the
// alpha channel when `bpp == 32`.
Image from_cbg_pixels(const Bytes& bgr, int width, int height, int bpp);

Image crop(const Image& img, int x1, int y1, int x2, int y2);

// Bilinear resize -- only used to downscale vision-API uploads, where the
// filter choice is immaterial.
Image resize(const Image& img, int new_w, int new_h);

// Encode to PNG in memory (vision-API upload).
Bytes encode_png(const Image& img);

// Encode to bottom-up Windows BMP (32-bpp when `img.has_alpha()`, else 24-bpp).
Bytes encode_bmp(const Image& img);

std::string base64_encode(const Bytes& data);

}  // namespace snu::cg
