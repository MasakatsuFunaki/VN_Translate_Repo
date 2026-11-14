// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "imaging/bgra_image.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace adi::imaging {
namespace {

constexpr double kChannelMaxD = 255.0;
constexpr double kInvChannelMaxD = 1.0 / kChannelMaxD;
constexpr double kMinPremulAlpha = 1.0e-9;

std::size_t CheckedPixelByteCount(int width, int height) {
    if (width < 0 || height < 0) {
        throw std::invalid_argument("BgraImage dimensions must be non-negative");
    }
    if (width == 0 || height == 0) {
        return 0u;
    }
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    const auto bpp = static_cast<std::size_t>(kBgraBytesPerPixel);
    if (w > std::numeric_limits<std::size_t>::max() / h) {
        throw std::overflow_error("BgraImage pixel count overflow");
    }
    const std::size_t count = w * h;
    if (count > std::numeric_limits<std::size_t>::max() / bpp) {
        throw std::overflow_error("BgraImage byte count overflow");
    }
    return count * bpp;
}

std::size_t PixelByteOffset(int width, int x, int y) noexcept {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
            static_cast<std::size_t>(x)) *
           static_cast<std::size_t>(kBgraBytesPerPixel);
}

std::uint8_t ClampChannelByte(double value) noexcept {
    const double clamped = std::clamp(value, 0.0, kChannelMaxD);
    return static_cast<std::uint8_t>(std::lround(clamped));
}

}  // namespace

bool operator==(const BgraColor& lhs, const BgraColor& rhs) noexcept {
    return lhs.b == rhs.b && lhs.g == rhs.g && lhs.r == rhs.r && lhs.a == rhs.a;
}

bool operator!=(const BgraColor& lhs, const BgraColor& rhs) noexcept {
    return !(lhs == rhs);
}

bool operator==(const RectI& lhs, const RectI& rhs) noexcept {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
}

bool operator!=(const RectI& lhs, const RectI& rhs) noexcept {
    return !(lhs == rhs);
}

RectI ClampRect(const RectI& rect, int width, int height) noexcept {
    if (rect.empty() || width <= 0 || height <= 0) {
        return RectI{};
    }
    const int x0 = std::max(0, rect.x);
    const int y0 = std::max(0, rect.y);
    const long long raw_x1 = static_cast<long long>(rect.x) + static_cast<long long>(rect.w);
    const long long raw_y1 = static_cast<long long>(rect.y) + static_cast<long long>(rect.h);
    const int x1 = static_cast<int>(std::min<long long>(width, raw_x1));
    const int y1 = static_cast<int>(std::min<long long>(height, raw_y1));
    if (x1 <= x0 || y1 <= y0) {
        return RectI{};
    }
    return RectI{x0, y0, x1 - x0, y1 - y0};
}

RectI UnionRect(const RectI& a, const RectI& b) noexcept {
    if (a.empty() && b.empty()) {
        return RectI{};
    }
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    const int x0 = std::min(a.x, b.x);
    const int y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.w, b.x + b.w);
    const int y1 = std::max(a.y + a.h, b.y + b.h);
    return RectI{x0, y0, x1 - x0, y1 - y0};
}

BgraImage::BgraImage(int width, int height, BgraColor fill) {
    const std::size_t total_bytes = CheckedPixelByteCount(width, height);
    if (total_bytes == 0u) {
        return;
    }
    width_ = width;
    height_ = height;
    pixels_.resize(total_bytes);
    Fill(fill);
}

BgraImage::BgraImage(int width, int height, std::vector<std::uint8_t> pixels) {
    const std::size_t expected = CheckedPixelByteCount(width, height);
    if (pixels.size() != expected) {
        throw std::invalid_argument("BgraImage pixel buffer size does not match width * height * 4");
    }
    if (expected == 0u) {
        return;
    }
    width_ = width;
    height_ = height;
    pixels_ = std::move(pixels);
}

void BgraImage::Fill(BgraColor color) noexcept {
    for (std::size_t off = 0; off < pixels_.size();
         off += static_cast<std::size_t>(kBgraBytesPerPixel)) {
        pixels_[off + static_cast<std::size_t>(kChannelBlue)] = color.b;
        pixels_[off + static_cast<std::size_t>(kChannelGreen)] = color.g;
        pixels_[off + static_cast<std::size_t>(kChannelRed)] = color.r;
        pixels_[off + static_cast<std::size_t>(kChannelAlpha)] = color.a;
    }
}

void BgraImage::FillRect(const RectI& rect, BgraColor color) noexcept {
    const RectI clamped = ClampRect(rect, width_, height_);
    if (clamped.empty()) {
        return;
    }
    for (int y = clamped.y; y < clamped.y + clamped.h; ++y) {
        std::size_t off = PixelByteOffset(width_, clamped.x, y);
        for (int x = 0; x < clamped.w; ++x) {
            pixels_[off + static_cast<std::size_t>(kChannelBlue)] = color.b;
            pixels_[off + static_cast<std::size_t>(kChannelGreen)] = color.g;
            pixels_[off + static_cast<std::size_t>(kChannelRed)] = color.r;
            pixels_[off + static_cast<std::size_t>(kChannelAlpha)] = color.a;
            off += static_cast<std::size_t>(kBgraBytesPerPixel);
        }
    }
}

void BgraImage::SetPixel(int x, int y, BgraColor color) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        throw std::out_of_range("BgraImage::SetPixel coordinates out of range");
    }
    const std::size_t off = PixelByteOffset(width_, x, y);
    pixels_[off + static_cast<std::size_t>(kChannelBlue)] = color.b;
    pixels_[off + static_cast<std::size_t>(kChannelGreen)] = color.g;
    pixels_[off + static_cast<std::size_t>(kChannelRed)] = color.r;
    pixels_[off + static_cast<std::size_t>(kChannelAlpha)] = color.a;
}

void BgraImage::BlitFrom(const BgraImage& src, const RectI& src_rect, int dst_x, int dst_y) {
    if (empty() || src.empty() || src_rect.empty()) {
        return;
    }
    int sx = src_rect.x;
    int sy = src_rect.y;
    int sw = src_rect.w;
    int sh = src_rect.h;
    int dx = dst_x;
    int dy = dst_y;

    if (sx < 0) {
        const int delta = -sx;
        sx = 0;
        dx += delta;
        sw -= delta;
    }
    if (sy < 0) {
        const int delta = -sy;
        sy = 0;
        dy += delta;
        sh -= delta;
    }
    if (dx < 0) {
        const int delta = -dx;
        dx = 0;
        sx += delta;
        sw -= delta;
    }
    if (dy < 0) {
        const int delta = -dy;
        dy = 0;
        sy += delta;
        sh -= delta;
    }
    if (sw <= 0 || sh <= 0 || sx >= src.width_ || sy >= src.height_ || dx >= width_ ||
        dy >= height_) {
        return;
    }
    sw = std::min({sw, src.width_ - sx, width_ - dx});
    sh = std::min({sh, src.height_ - sy, height_ - dy});
    if (sw <= 0 || sh <= 0) {
        return;
    }

    if (&src == this) {
        const BgraImage copy = src.Crop(RectI{sx, sy, sw, sh});
        BlitFrom(copy, RectI{0, 0, sw, sh}, dx, dy);
        return;
    }

    const std::size_t row_bytes =
        static_cast<std::size_t>(sw) * static_cast<std::size_t>(kBgraBytesPerPixel);
    for (int row = 0; row < sh; ++row) {
        const std::uint8_t* src_ptr = src.pixels_.data() + PixelByteOffset(src.width_, sx, sy + row);
        std::uint8_t* dst_ptr = pixels_.data() + PixelByteOffset(width_, dx, dy + row);
        std::memcpy(dst_ptr, src_ptr, row_bytes);
    }
}

BgraColor BgraImage::PixelAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        throw std::out_of_range("BgraImage::PixelAt coordinates out of range");
    }
    const std::size_t off = PixelByteOffset(width_, x, y);
    return BgraColor{
        pixels_[off + static_cast<std::size_t>(kChannelBlue)],
        pixels_[off + static_cast<std::size_t>(kChannelGreen)],
        pixels_[off + static_cast<std::size_t>(kChannelRed)],
        pixels_[off + static_cast<std::size_t>(kChannelAlpha)],
    };
}

bool BgraImage::IsFullyOpaque() const noexcept {
    for (std::size_t off = static_cast<std::size_t>(kChannelAlpha); off < pixels_.size();
         off += static_cast<std::size_t>(kBgraBytesPerPixel)) {
        if (pixels_[off] != kOpaqueAlpha) {
            return false;
        }
    }
    return true;
}

bool BgraImage::HasAnyTransparentPixel() const noexcept {
    return !IsFullyOpaque();
}

RectI BgraImage::NonZeroAlphaBounds(std::uint8_t alpha_threshold) const noexcept {
    if (empty()) {
        return RectI{};
    }
    int min_x = width_;
    int min_y = height_;
    int max_x = -1;
    int max_y = -1;
    for (int y = 0; y < height_; ++y) {
        std::size_t off = PixelByteOffset(width_, 0, y) + static_cast<std::size_t>(kChannelAlpha);
        for (int x = 0; x < width_; ++x) {
            if (pixels_[off] >= alpha_threshold) {
                min_x = std::min(min_x, x);
                min_y = std::min(min_y, y);
                max_x = std::max(max_x, x);
                max_y = std::max(max_y, y);
            }
            off += static_cast<std::size_t>(kBgraBytesPerPixel);
        }
    }
    if (max_x < min_x || max_y < min_y) {
        return RectI{};
    }
    return RectI{min_x, min_y, max_x - min_x + 1, max_y - min_y + 1};
}

BgraImage BgraImage::Crop(const RectI& rect) const {
    const RectI clamped = ClampRect(rect, width_, height_);
    if (clamped.empty()) {
        return BgraImage{};
    }
    BgraImage cropped(clamped.w, clamped.h);
    cropped.BlitFrom(*this, clamped, 0, 0);
    return cropped;
}

BgraImage BgraImage::ResizeAreaPremultiplied(int new_width, int new_height) const {
    if (new_width < 0 || new_height < 0) {
        throw std::invalid_argument("ResizeAreaPremultiplied dimensions must be non-negative");
    }
    if (empty() || new_width == 0 || new_height == 0) {
        return BgraImage{};
    }
    if (new_width == width_ && new_height == height_) {
        return *this;
    }

    BgraImage out(new_width, new_height);
    const double scale_x = static_cast<double>(width_) / static_cast<double>(new_width);
    const double scale_y = static_cast<double>(height_) / static_cast<double>(new_height);

    for (int dy = 0; dy < new_height; ++dy) {
        const double sy0 = static_cast<double>(dy) * scale_y;
        const double sy1 = static_cast<double>(dy + 1) * scale_y;
        const int iy0 = std::clamp(static_cast<int>(std::floor(sy0)), 0, height_ - 1);
        const int iy1 = std::clamp(static_cast<int>(std::ceil(sy1)) - 1, 0, height_ - 1);

        for (int dx = 0; dx < new_width; ++dx) {
            const double sx0 = static_cast<double>(dx) * scale_x;
            const double sx1 = static_cast<double>(dx + 1) * scale_x;
            const int ix0 = std::clamp(static_cast<int>(std::floor(sx0)), 0, width_ - 1);
            const int ix1 = std::clamp(static_cast<int>(std::ceil(sx1)) - 1, 0, width_ - 1);

            double weight_sum = 0.0;
            double pm_b = 0.0;
            double pm_g = 0.0;
            double pm_r = 0.0;
            double pm_a = 0.0;

            for (int sy = iy0; sy <= iy1; ++sy) {
                const double wy =
                    std::max(0.0, std::min(sy1, static_cast<double>(sy + 1)) -
                                      std::max(sy0, static_cast<double>(sy)));
                if (wy <= 0.0) {
                    continue;
                }
                for (int sx = ix0; sx <= ix1; ++sx) {
                    const double wx =
                        std::max(0.0, std::min(sx1, static_cast<double>(sx + 1)) -
                                          std::max(sx0, static_cast<double>(sx)));
                    const double w = wx * wy;
                    if (w <= 0.0) {
                        continue;
                    }
                    const BgraColor p = PixelAt(sx, sy);
                    const double a = static_cast<double>(p.a) * kInvChannelMaxD;
                    pm_b += w * static_cast<double>(p.b) * a;
                    pm_g += w * static_cast<double>(p.g) * a;
                    pm_r += w * static_cast<double>(p.r) * a;
                    pm_a += w * a;
                    weight_sum += w;
                }
            }

            if (weight_sum <= 0.0) {
                continue;
            }
            const double avg_a = pm_a / weight_sum;
            const std::uint8_t out_a = ClampChannelByte(avg_a * kChannelMaxD);
            if (out_a == 0u || avg_a <= kMinPremulAlpha) {
                out.SetPixel(dx, dy, BgraColor{0, 0, 0, 0});
                continue;
            }
            const double inv_pm_a = 1.0 / pm_a;
            out.SetPixel(dx, dy,
                         BgraColor{
                             ClampChannelByte(pm_b * inv_pm_a),
                             ClampChannelByte(pm_g * inv_pm_a),
                             ClampChannelByte(pm_r * inv_pm_a),
                             out_a,
                         });
        }
    }
    return out;
}

bool operator==(const BgraImage& lhs, const BgraImage& rhs) noexcept {
    return lhs.width() == rhs.width() && lhs.height() == rhs.height() &&
           lhs.pixels() == rhs.pixels();
}

bool operator!=(const BgraImage& lhs, const BgraImage& rhs) noexcept {
    return !(lhs == rhs);
}

PixelDiffMetrics ComputePixelDiff(const BgraImage& lhs, const BgraImage& rhs) {
    if (lhs.width() != rhs.width() || lhs.height() != rhs.height()) {
        throw std::invalid_argument("ComputePixelDiff requires equal image dimensions");
    }
    PixelDiffMetrics metrics{};
    const auto& lp = lhs.pixels();
    const auto& rp = rhs.pixels();
    for (std::size_t off = 0; off < lp.size();
         off += static_cast<std::size_t>(kBgraBytesPerPixel)) {
        bool pixel_differs = false;
        for (int ch = 0; ch < kBgraBytesPerPixel; ++ch) {
            const int diff = std::abs(static_cast<int>(lp[off + static_cast<std::size_t>(ch)]) -
                                      static_cast<int>(rp[off + static_cast<std::size_t>(ch)]));
            if (diff == 0) {
                continue;
            }
            pixel_differs = true;
            metrics.total_channel_abs_diff += static_cast<std::uint64_t>(diff);
            metrics.max_channel_abs_diff =
                std::max(metrics.max_channel_abs_diff, static_cast<std::uint8_t>(diff));
        }
        if (pixel_differs) {
            ++metrics.differing_pixels;
        }
    }
    return metrics;
}

}  // namespace adi::imaging
