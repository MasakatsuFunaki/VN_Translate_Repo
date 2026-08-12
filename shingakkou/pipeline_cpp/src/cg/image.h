// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Raster support for the narrative-CG step: an 8-bit RGB/RGBA image with
// decode, crop, resize, PNG/BMP/JPEG encode and base64.
//
// The alpha channel is load-bearing here: 822 of the 1001 assets in this game
// are PNG colour type 6, and 87 of the 90 patched outputs are RGBA.  Flatten
// them and every UI button repacks as an opaque black box.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "common/util.h"

namespace shin::cg {

struct Image {
    int width = 0;
    int height = 0;
    int channels = 3;  // 3 == RGB, 4 == RGBA.  Never anything else.
    Bytes px;          // row-major, `channels` bytes/pixel

    bool empty() const { return width <= 0 || height <= 0; }
    std::uint8_t* at(int x, int y) {
        return px.data() + (static_cast<std::size_t>(y) * width + x) * channels;
    }
    const std::uint8_t* at(int x, int y) const {
        return px.data() + (static_cast<std::size_t>(y) * width + x) * channels;
    }
};

// Decode to RGB or RGBA, nothing else.  `channels` is decided from the
// CONTAINER HEADER (PNG IHDR colour type, BMP bit count) rather than from
// whatever the decoder infers, so an image that declares alpha keeps it even
// when every pixel happens to be opaque.
std::optional<Image> open_image(const std::string& path);
std::optional<Image> decode_image(const Bytes& raw);

// The crop box is HALF-OPEN: [x1,x2) x [y1,y2).
Image crop(const Image& img, int x1, int y1, int x2, int y2);

// Bilinear.  Only ever used to downscale a vision upload wider than 1280px,
// and nothing in this corpus is that wide.
Image resize(const Image& img, int new_w, int new_h);

Bytes encode_png(const Image& img);  // colour type 2 for 3ch, 6 for 4ch
Bytes encode_bmp(const Image& img);  // 24-bit; caller drops alpha first
Bytes encode_jpg(const Image& img, int quality);

// RGBA -> RGB: DISCARDS alpha, does not composite onto a matte.
Image drop_alpha(const Image& img);

std::string base64_encode(const Bytes& data);

}  // namespace shin::cg
