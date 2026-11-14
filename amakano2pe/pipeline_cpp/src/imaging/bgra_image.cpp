// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "imaging/bgra_image.h"

#include <algorithm>
#include <stdexcept>

namespace ama::imaging {

namespace {

constexpr double kMaxChannelValue = 255.0;
constexpr double kRoundingBias = 0.5;

std::uint8_t ToChannel(double value) {
    const double clamped = std::clamp(value, 0.0, kMaxChannelValue);
    return static_cast<std::uint8_t>(clamped + kRoundingBias);
}

}  // namespace

bool operator==(const BgraColor& lhs, const BgraColor& rhs) {
    return lhs.b == rhs.b && lhs.g == rhs.g && lhs.r == rhs.r && lhs.a == rhs.a;
}

bool operator!=(const BgraColor& lhs, const BgraColor& rhs) {
    return !(lhs == rhs);
}

bool operator==(const RectI& lhs, const RectI& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
}

RectI ClampRect(const RectI& rect, int width, int height) {
    const int x0 = std::clamp(rect.x, 0, width);
    const int y0 = std::clamp(rect.y, 0, height);
    const int x1 = std::clamp(rect.x + rect.w, 0, width);
    const int y1 = std::clamp(rect.y + rect.h, 0, height);
    return RectI{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

BgraImage MakeBgraImage(int width, int height) {
    if (width < 0 || height < 0) {
        throw std::invalid_argument("negative image dimensions");
    }
    BgraImage image;
    image.width = width;
    image.height = height;
    image.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                            kBgraBytesPerPixel,
                        0);
    return image;
}

BgraImage MakeFilledBgraImage(int width, int height, BgraColor color) {
    BgraImage image = MakeBgraImage(width, height);
    for (std::size_t i = 0; i < image.pixels.size(); i += kBgraBytesPerPixel) {
        image.pixels[i + kChannelBlue] = color.b;
        image.pixels[i + kChannelGreen] = color.g;
        image.pixels[i + kChannelRed] = color.r;
        image.pixels[i + kChannelAlpha] = color.a;
    }
    return image;
}

std::size_t PixelOffset(const BgraImage& image, int x, int y) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
            static_cast<std::size_t>(x)) *
           kBgraBytesPerPixel;
}

BgraColor GetPixel(const BgraImage& image, int x, int y) {
    const std::size_t o = PixelOffset(image, x, y);
    return BgraColor{image.pixels[o + kChannelBlue], image.pixels[o + kChannelGreen],
                     image.pixels[o + kChannelRed], image.pixels[o + kChannelAlpha]};
}

void SetPixel(BgraImage& image, int x, int y, BgraColor color) {
    const std::size_t o = PixelOffset(image, x, y);
    image.pixels[o + kChannelBlue] = color.b;
    image.pixels[o + kChannelGreen] = color.g;
    image.pixels[o + kChannelRed] = color.r;
    image.pixels[o + kChannelAlpha] = color.a;
}

BgraImage CropImage(const BgraImage& image, const RectI& rect) {
    const RectI r = ClampRect(rect, image.width, image.height);
    BgraImage out = MakeBgraImage(r.w, r.h);
    const std::size_t row_bytes = static_cast<std::size_t>(r.w) * kBgraBytesPerPixel;
    for (int y = 0; y < r.h; ++y) {
        const auto src = image.pixels.begin() +
                         static_cast<std::ptrdiff_t>(PixelOffset(image, r.x, r.y + y));
        std::copy(src, src + static_cast<std::ptrdiff_t>(row_bytes),
                  out.pixels.begin() + static_cast<std::ptrdiff_t>(PixelOffset(out, 0, y)));
    }
    return out;
}

BgraImage ResizeImage(const BgraImage& image, int new_width, int new_height) {
    BgraImage out = MakeBgraImage(new_width, new_height);
    if (image.width == 0 || image.height == 0 || new_width == 0 || new_height == 0) {
        return out;
    }
    const double sx = static_cast<double>(image.width) / new_width;
    const double sy = static_cast<double>(image.height) / new_height;
    for (int oy = 0; oy < new_height; ++oy) {
        const double fy0 = oy * sy;
        const double fy1 = fy0 + sy;
        for (int ox = 0; ox < new_width; ++ox) {
            const double fx0 = ox * sx;
            const double fx1 = fx0 + sx;
            double acc_b = 0.0;
            double acc_g = 0.0;
            double acc_r = 0.0;
            double acc_a = 0.0;
            double area = 0.0;
            const int iy_end = std::min(image.height, static_cast<int>(fy1) + 1);
            const int ix_end = std::min(image.width, static_cast<int>(fx1) + 1);
            for (int iy = static_cast<int>(fy0); iy < iy_end; ++iy) {
                const double wy = std::min<double>(iy + 1, fy1) - std::max<double>(iy, fy0);
                if (wy <= 0.0) {
                    continue;
                }
                for (int ix = static_cast<int>(fx0); ix < ix_end; ++ix) {
                    const double wx = std::min<double>(ix + 1, fx1) - std::max<double>(ix, fx0);
                    if (wx <= 0.0) {
                        continue;
                    }
                    const double w = wx * wy;
                    const BgraColor c = GetPixel(image, ix, iy);
                    const double alpha = c.a / kMaxChannelValue;
                    acc_b += c.b * alpha * w;
                    acc_g += c.g * alpha * w;
                    acc_r += c.r * alpha * w;
                    acc_a += alpha * w;
                    area += w;
                }
            }
            if (area <= 0.0 || acc_a <= 0.0) {
                continue;
            }
            SetPixel(out, ox, oy,
                     BgraColor{ToChannel(acc_b / acc_a), ToChannel(acc_g / acc_a),
                               ToChannel(acc_r / acc_a),
                               ToChannel(acc_a / area * kMaxChannelValue)});
        }
    }
    return out;
}

bool IsFullyOpaque(const BgraImage& image) {
    for (std::size_t i = kChannelAlpha; i < image.pixels.size(); i += kBgraBytesPerPixel) {
        if (image.pixels[i] != kOpaqueAlpha) {
            return false;
        }
    }
    return true;
}

}  // namespace ama::imaging
