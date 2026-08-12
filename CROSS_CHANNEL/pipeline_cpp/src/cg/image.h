// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Minimal RGB8 image type for the narrative-CG scanner: BMP/PNG decode, crop,
// resize, PNG + BMP encode.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "common/util.h"

namespace crc::cg {

using Rgb = std::array<std::uint8_t, 3>;

struct Image {
    int width = 0;
    int height = 0;
    Bytes rgb;  // row-major, 3 bytes/pixel

    bool empty() const { return width <= 0 || height <= 0; }
    std::uint8_t* px(int x, int y) {
        return rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
    }
    const std::uint8_t* px(int x, int y) const {
        return rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
    }
};

// Decode a BMP / PNG blob via stb_image, converting to RGB.
std::optional<Image> decode_image(const Bytes& raw);

Image crop(const Image& img, int x1, int y1, int x2, int y2);

// Bilinear downscale.  Resampling quality is not critical here: the resized
// image is only ever vision-API input, never a written artefact.
Image resize(const Image& img, int new_w, int new_h);

// Vision-API upload.
Bytes encode_png(const Image& img);

// The canonical uncompressed BMP an RGB image saves as: 24-bit BGR,
// bottom-up, 54-byte header, rows padded to 4 bytes.
Bytes encode_bmp(const Image& img);

std::string base64_encode(const Bytes& data);

// NOTE: x2/y2 are INCLUSIVE -- the erase rect must cover the last column and
// row of the region, or a one-pixel seam of the old Japanese survives.
void fill_rect(Image& img, int x1, int y1, int x2, int y2, Rgb color);

// Median colour of the 2-pixel border of a region.  An even-length sample
// averages the two middle values and truncates the result; corner pixels are
// sampled twice, so a coloured corner pulls the estimate slightly toward it.
Rgb sample_bg(const Image& img, int x1, int y1, int x2, int y2);

}  // namespace crc::cg
