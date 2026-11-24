// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Straight-alpha 32-bit BGRA raster shared by the BGI sysgrp codec, menu
// renderer, contact-sheet generator, and PIMPL SpriteCanvas.
//
// Layout: top-to-bottom scanlines, each row `width * kBgraBytesPerPixel` bytes
// with no row padding, channel order B, G, R, A in straight (unpremultiplied)
// alpha space.
//
// Thread-compatible.
#pragma once

#include "common/util.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace snu::menu {

inline constexpr int kBgraBytesPerPixel = 4;
inline constexpr int kBgrBytesPerPixel = 3;
inline constexpr int kChannelBlue = 0;
inline constexpr int kChannelGreen = 1;
inline constexpr int kChannelRed = 2;
inline constexpr int kChannelAlpha = 3;
inline constexpr std::uint8_t kTransparentAlpha = 0x00u;
inline constexpr std::uint8_t kOpaqueAlpha = 0xFFu;
inline constexpr int kBmpBpp24 = 24;
inline constexpr int kBmpBpp32 = 32;

struct BgraColor {
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
    std::uint8_t a = 0;
};

constexpr bool operator==(const BgraColor& lhs, const BgraColor& rhs) noexcept {
    return lhs.b == rhs.b && lhs.g == rhs.g && lhs.r == rhs.r && lhs.a == rhs.a;
}

constexpr bool operator!=(const BgraColor& lhs, const BgraColor& rhs) noexcept {
    return !(lhs == rhs);
}

// Axis-aligned integer rectangle in half-open coordinates `[x, x + w) x [y, y + h)`.
struct RectI {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    constexpr bool empty() const noexcept { return w <= 0 || h <= 0; }
};

constexpr bool operator==(const RectI& lhs, const RectI& rhs) noexcept {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
}

constexpr bool operator!=(const RectI& lhs, const RectI& rhs) noexcept {
    return !(lhs == rhs);
}

// Intersection of `rect` with `[0, width) x [0, height)`. Returns `{0, 0, 0, 0}`
// when the intersection is empty.
RectI ClampRect(const RectI& rect, int width, int height) noexcept;

// Smallest axis-aligned rectangle enclosing all non-empty inputs.
RectI UnionRect(const RectI& a, const RectI& b) noexcept;

class BgraImage {
public:
    BgraImage() = default;
    BgraImage(int width, int height, BgraColor fill = {});
    BgraImage(int width, int height, Bytes pixels);

    void Fill(BgraColor color) noexcept;
    void FillRect(const RectI& rect, BgraColor color) noexcept;

    // Bounds-checked pixel mutation; throws std::out_of_range if `(x, y)` lies
    // outside `[0, width()) x [0, height())`.
    void SetPixel(int x, int y, BgraColor c);

    // Copies `src_rect` (clamped to `src` and destination bounds) verbatim,
    // including straight-alpha values, onto `(dst_x, dst_y)`.
    void BlitFrom(const BgraImage& src, const RectI& src_rect, int dst_x, int dst_y);

    std::uint8_t* data() noexcept { return pixels_.data(); }

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    bool empty() const noexcept { return width_ <= 0 || height_ <= 0; }
    std::size_t byte_size() const noexcept { return pixels_.size(); }
    const std::uint8_t* data() const noexcept { return pixels_.data(); }
    const Bytes& pixels() const noexcept { return pixels_; }

    // Bounds-checked pixel read; throws std::out_of_range if `(x, y)` lies
    // outside `[0, width()) x [0, height())`.
    BgraColor PixelAt(int x, int y) const;

    RectI NonZeroAlphaBounds(std::uint8_t alpha_threshold = 1u) const noexcept;

    // Returns a cropped copy of `rect` clamped to `[0, width()) x [0, height())`.
    BgraImage Crop(const RectI& rect) const;

    // Area-averaged resize in premultiplied-alpha space.
    BgraImage ResizeAreaPremultiplied(int new_width, int new_height) const;

    // Encodes bottom-up Windows `"BM"` BMP (`bpp` must be 24 or 32).
    // Throws std::runtime_error if the image is empty or `bpp` is unsupported.
    Bytes EncodeBmp(int bpp = kBmpBpp32) const;

    // Decodes 24/32-bpp BMP directly (preserving exact straight BGRA alpha) or
    // PNG via stb_image. Throws std::runtime_error on malformed input.
    static BgraImage DecodeBmpOrPng(const Bytes& raw);

    // Encodes 32-bit RGBA PNG in memory via stb_image_write.
    Bytes EncodePng() const;

    void SavePng(const std::filesystem::path& path) const;
    static BgraImage LoadPng(const std::filesystem::path& path);

private:
    int width_ = 0;
    int height_ = 0;
    Bytes pixels_;
};

bool operator==(const BgraImage& lhs, const BgraImage& rhs) noexcept;
bool operator!=(const BgraImage& lhs, const BgraImage& rhs) noexcept;

}  // namespace snu::menu
