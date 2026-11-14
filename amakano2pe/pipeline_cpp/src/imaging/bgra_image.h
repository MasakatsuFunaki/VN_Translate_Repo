// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Plain 32-bit BGRA raster shared by the sprite codec, the WIC image IO and
// the sprite canvas.  It is the medium of exchange between those components,
// so it stays a small uninsulated value type.
//
// Layout: rows run top to bottom, each row `width * kBgraBytesPerPixel` bytes
// with no padding, channels B, G, R, A, straight (non-premultiplied) alpha.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ama::imaging {

inline constexpr int kBgraBytesPerPixel = 4;
inline constexpr int kChannelBlue = 0;
inline constexpr int kChannelGreen = 1;
inline constexpr int kChannelRed = 2;
inline constexpr int kChannelAlpha = 3;
inline constexpr std::uint8_t kOpaqueAlpha = 0xFF;

struct BgraColor {
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
    std::uint8_t a = 0;
};

bool operator==(const BgraColor& lhs, const BgraColor& rhs);
bool operator!=(const BgraColor& lhs, const BgraColor& rhs);

// Axis-aligned integer rectangle; `w` and `h` may be zero (empty).
struct RectI {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

bool operator==(const RectI& lhs, const RectI& rhs);

// Intersection of `rect` with `[0, width) x [0, height)`.
RectI ClampRect(const RectI& rect, int width, int height);

struct BgraImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

// A `width` x `height` image, every pixel fully transparent black.
// Throws std::invalid_argument on negative dimensions.
BgraImage MakeBgraImage(int width, int height);

// A `width` x `height` image filled with `color`.
BgraImage MakeFilledBgraImage(int width, int height, BgraColor color);

// Byte offset of pixel (x, y).  REQUIRES: 0 <= x < width, 0 <= y < height.
std::size_t PixelOffset(const BgraImage& image, int x, int y);

BgraColor GetPixel(const BgraImage& image, int x, int y);
void SetPixel(BgraImage& image, int x, int y, BgraColor color);

// Copy of `rect` (clamped to the image).
BgraImage CropImage(const BgraImage& image, const RectI& rect);

// Area-averaged resize in premultiplied space, so transparent pixels never
// bleed their colour into opaque neighbours.
BgraImage ResizeImage(const BgraImage& image, int new_width, int new_height);

// True when every alpha byte is kOpaqueAlpha.
bool IsFullyOpaque(const BgraImage& image);

}  // namespace ama::imaging
