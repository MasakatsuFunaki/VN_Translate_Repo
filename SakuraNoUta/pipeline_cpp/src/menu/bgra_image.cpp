// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/bgra_image.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace snu::menu {
namespace {

constexpr double kChannelMaxD = 255.0;
constexpr double kInvChannelMaxD = 1.0 / kChannelMaxD;
constexpr double kMinPremulAlpha = 1.0e-9;

constexpr int kBitsPerByte = 8;
constexpr int kDwordBytes = 4;
constexpr std::size_t kRgbaChannelRed = 0;
constexpr std::size_t kRgbaChannelGreen = 1;
constexpr std::size_t kRgbaChannelBlue = 2;
constexpr std::size_t kRgbaChannelAlpha = 3;
constexpr std::size_t kBmpFileHeaderBytes = 14;
constexpr std::size_t kBmpInfoHeaderBytes = 40;
constexpr std::size_t kBmpTotalHeaderBytes = kBmpFileHeaderBytes + kBmpInfoHeaderBytes;
constexpr std::size_t kBmpPixelOffsetPos = 10;
constexpr std::size_t kBmpDibSizePos = 14;
constexpr std::size_t kBmpWidthPos = 18;
constexpr std::size_t kBmpHeightPos = 22;
constexpr std::size_t kBmpPlanesPos = 26;
constexpr std::size_t kBmpBppPos = 28;
constexpr std::size_t kBmpCompressionPos = 30;
constexpr std::uint16_t kBmpPlanesCount = 1;
constexpr std::uint32_t kBmpCompressionRgb = 0;
constexpr std::uint32_t kBmpCompressionBitfields = 3;
constexpr std::uint32_t kBmpBitfieldsMinDibBytes = 56;
constexpr std::size_t kBmpMaskRedPos = 54;
constexpr std::size_t kBmpMaskGreenPos = 58;
constexpr std::size_t kBmpMaskBluePos = 62;
constexpr std::size_t kBmpMaskAlphaPos = 66;
constexpr std::uint32_t kBgraMaskRed = 0x00FF0000u;
constexpr std::uint32_t kBgraMaskGreen = 0x0000FF00u;
constexpr std::uint32_t kBgraMaskBlue = 0x000000FFu;
constexpr std::uint32_t kBgraMaskAlpha = 0xFF000000u;
constexpr std::size_t kBmpRowAlignBytes = 4;
constexpr std::uint32_t kBmpDefaultPpm72Dpi = 2835;
constexpr std::size_t kMinImageProbeBytes = 8;

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

std::uint16_t ReadLe16(const Bytes& raw, std::size_t off) noexcept {
    std::uint16_t v = 0;
    std::memcpy(&v, raw.data() + off, sizeof(v));
    return v;
}

std::uint32_t ReadLe32(const Bytes& raw, std::size_t off) noexcept {
    std::uint32_t v = 0;
    std::memcpy(&v, raw.data() + off, sizeof(v));
    return v;
}

void WriteLe16(Bytes& dst, std::uint16_t v) {
    dst.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    dst.push_back(static_cast<std::uint8_t>((v >> kBitsPerByte) & 0xFFu));
}

void WriteLe32(Bytes& dst, std::uint32_t v) {
    for (int i = 0; i < kDwordBytes; ++i) {
        dst.push_back(static_cast<std::uint8_t>((v >> (kBitsPerByte * i)) & 0xFFu));
    }
}

void AppendPngChunk(void* context, void* data, int size) {
    if (size <= 0) {
        return;
    }
    auto* out = static_cast<Bytes*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

BgraImage DecodeBmpDirect(const Bytes& raw) {
    if (raw.size() < kBmpTotalHeaderBytes) {
        throw std::runtime_error("DecodeBmpOrPng: truncated BMP header");
    }

    const std::uint32_t pixel_offset = ReadLe32(raw, kBmpPixelOffsetPos);
    const std::uint32_t dib_size = ReadLe32(raw, kBmpDibSizePos);
    const auto bi_width = static_cast<std::int32_t>(ReadLe32(raw, kBmpWidthPos));
    const auto bi_height = static_cast<std::int32_t>(ReadLe32(raw, kBmpHeightPos));
    const std::uint16_t planes = ReadLe16(raw, kBmpPlanesPos);
    const std::uint16_t bpp = ReadLe16(raw, kBmpBppPos);
    const std::uint32_t compression = ReadLe32(raw, kBmpCompressionPos);

    if (dib_size < kBmpInfoHeaderBytes || planes != kBmpPlanesCount || bi_width <= 0 ||
        bi_height == 0 || bi_height == std::numeric_limits<std::int32_t>::min()) {
        throw std::runtime_error("DecodeBmpOrPng: invalid BMP header fields");
    }
    if (bpp != kBmpBpp24 && bpp != kBmpBpp32) {
        throw std::runtime_error("DecodeBmpOrPng: unsupported BMP bit depth");
    }

    bool preserve_alpha_channel = (bpp == kBmpBpp32);
    if (compression == kBmpCompressionBitfields) {
        if (bpp != kBmpBpp32 || dib_size < kBmpBitfieldsMinDibBytes ||
            raw.size() < kBmpFileHeaderBytes + kBmpBitfieldsMinDibBytes) {
            throw std::runtime_error("DecodeBmpOrPng: unsupported BMP bitfields header");
        }
        const std::uint32_t r_mask = ReadLe32(raw, kBmpMaskRedPos);
        const std::uint32_t g_mask = ReadLe32(raw, kBmpMaskGreenPos);
        const std::uint32_t b_mask = ReadLe32(raw, kBmpMaskBluePos);
        const std::uint32_t a_mask = ReadLe32(raw, kBmpMaskAlphaPos);
        if (r_mask != kBgraMaskRed || g_mask != kBgraMaskGreen || b_mask != kBgraMaskBlue ||
            (a_mask != kBgraMaskAlpha && a_mask != 0u)) {
            throw std::runtime_error("DecodeBmpOrPng: unsupported BMP channel masks");
        }
        preserve_alpha_channel = (a_mask == kBgraMaskAlpha);
    } else if (compression != kBmpCompressionRgb) {
        throw std::runtime_error("DecodeBmpOrPng: unsupported BMP compression");
    }

    const bool top_down = (bi_height < 0);
    const int height = top_down ? -bi_height : bi_height;
    const auto bytes_per_px = static_cast<std::size_t>(bpp / kBitsPerByte);
    const std::size_t raw_row = static_cast<std::size_t>(bi_width) * bytes_per_px;
    const std::size_t stride =
        ((raw_row + kBmpRowAlignBytes - 1u) / kBmpRowAlignBytes) * kBmpRowAlignBytes;

    if (pixel_offset > raw.size() ||
        stride > (raw.size() - pixel_offset) / static_cast<std::size_t>(height)) {
        throw std::runtime_error("DecodeBmpOrPng: truncated BMP pixel array");
    }

    BgraImage out(bi_width, height);
    std::uint8_t* dst_base = out.data();
    const std::size_t dst_row_bytes =
        static_cast<std::size_t>(bi_width) * static_cast<std::size_t>(kBgraBytesPerPixel);

    for (int y = 0; y < height; ++y) {
        const int src_row = top_down ? y : (height - 1 - y);
        const std::uint8_t* src =
            raw.data() + pixel_offset + static_cast<std::size_t>(src_row) * stride;
        std::uint8_t* dst = dst_base + static_cast<std::size_t>(y) * dst_row_bytes;

        if (bpp == kBmpBpp32 && preserve_alpha_channel) {
            std::memcpy(dst, src, dst_row_bytes);
            continue;
        }
        for (int x = 0; x < bi_width; ++x) {
            const std::size_t s_off = static_cast<std::size_t>(x) * bytes_per_px;
            const std::size_t d_off =
                static_cast<std::size_t>(x) * static_cast<std::size_t>(kBgraBytesPerPixel);
            dst[d_off + kChannelBlue] = src[s_off + kChannelBlue];
            dst[d_off + kChannelGreen] = src[s_off + kChannelGreen];
            dst[d_off + kChannelRed] = src[s_off + kChannelRed];
            dst[d_off + kChannelAlpha] = kOpaqueAlpha;
        }
    }
    return out;
}

}  // namespace

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

BgraImage::BgraImage(int width, int height, Bytes pixels) {
    const std::size_t expected = CheckedPixelByteCount(width, height);
    if (pixels.size() != expected) {
        throw std::invalid_argument(
            "BgraImage pixel buffer size does not match width * height * 4");
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
        pixels_[off + kChannelBlue] = color.b;
        pixels_[off + kChannelGreen] = color.g;
        pixels_[off + kChannelRed] = color.r;
        pixels_[off + kChannelAlpha] = color.a;
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
            pixels_[off + kChannelBlue] = color.b;
            pixels_[off + kChannelGreen] = color.g;
            pixels_[off + kChannelRed] = color.r;
            pixels_[off + kChannelAlpha] = color.a;
            off += static_cast<std::size_t>(kBgraBytesPerPixel);
        }
    }
}

void BgraImage::SetPixel(int x, int y, BgraColor c) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        throw std::out_of_range("BgraImage::SetPixel coordinates out of range");
    }
    const std::size_t off = PixelByteOffset(width_, x, y);
    pixels_[off + kChannelBlue] = c.b;
    pixels_[off + kChannelGreen] = c.g;
    pixels_[off + kChannelRed] = c.r;
    pixels_[off + kChannelAlpha] = c.a;
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
        const std::uint8_t* src_ptr =
            src.pixels_.data() + PixelByteOffset(src.width_, sx, sy + row);
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
        pixels_[off + kChannelBlue],
        pixels_[off + kChannelGreen],
        pixels_[off + kChannelRed],
        pixels_[off + kChannelAlpha],
    };
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

Bytes BgraImage::EncodeBmp(int bpp) const {
    if (empty()) {
        throw std::runtime_error("BgraImage::EncodeBmp cannot encode an empty image");
    }
    if (bpp != kBmpBpp24 && bpp != kBmpBpp32) {
        throw std::runtime_error("BgraImage::EncodeBmp requires 24 or 32 bpp");
    }

    const auto bytes_per_px = static_cast<std::size_t>(bpp / kBitsPerByte);
    const std::size_t raw_row = static_cast<std::size_t>(width_) * bytes_per_px;
    const std::size_t stride =
        ((raw_row + kBmpRowAlignBytes - 1u) / kBmpRowAlignBytes) * kBmpRowAlignBytes;
    const std::size_t image_bytes = stride * static_cast<std::size_t>(height_);
    const std::size_t total_bytes = kBmpTotalHeaderBytes + image_bytes;
    if (total_bytes > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("BgraImage::EncodeBmp exceeds 32-bit BMP size limit");
    }

    Bytes bmp;
    bmp.reserve(total_bytes);
    bmp.push_back('B');
    bmp.push_back('M');
    WriteLe32(bmp, static_cast<std::uint32_t>(total_bytes));
    WriteLe16(bmp, 0u);
    WriteLe16(bmp, 0u);
    WriteLe32(bmp, static_cast<std::uint32_t>(kBmpTotalHeaderBytes));
    WriteLe32(bmp, static_cast<std::uint32_t>(kBmpInfoHeaderBytes));
    WriteLe32(bmp, static_cast<std::uint32_t>(width_));
    WriteLe32(bmp, static_cast<std::uint32_t>(height_));
    WriteLe16(bmp, kBmpPlanesCount);
    WriteLe16(bmp, static_cast<std::uint16_t>(bpp));
    WriteLe32(bmp, kBmpCompressionRgb);
    WriteLe32(bmp, static_cast<std::uint32_t>(image_bytes));
    WriteLe32(bmp, kBmpDefaultPpm72Dpi);
    WriteLe32(bmp, kBmpDefaultPpm72Dpi);
    WriteLe32(bmp, 0u);
    WriteLe32(bmp, 0u);

    const std::size_t src_row_bytes =
        static_cast<std::size_t>(width_) * static_cast<std::size_t>(kBgraBytesPerPixel);
    const std::size_t pad_bytes = stride - raw_row;

    for (int y = height_ - 1; y >= 0; --y) {
        const std::uint8_t* row = pixels_.data() + static_cast<std::size_t>(y) * src_row_bytes;
        if (bpp == kBmpBpp32) {
            bmp.insert(bmp.end(), row, row + src_row_bytes);
            continue;
        }
        for (int x = 0; x < width_; ++x) {
            const std::size_t off =
                static_cast<std::size_t>(x) * static_cast<std::size_t>(kBgraBytesPerPixel);
            bmp.push_back(row[off + kChannelBlue]);
            bmp.push_back(row[off + kChannelGreen]);
            bmp.push_back(row[off + kChannelRed]);
        }
        bmp.insert(bmp.end(), pad_bytes, 0u);
    }
    return bmp;
}

BgraImage BgraImage::DecodeBmpOrPng(const Bytes& raw) {
    if (raw.size() < kMinImageProbeBytes) {
        throw std::runtime_error("DecodeBmpOrPng: input buffer too small");
    }
    if (raw[0] == 'B' && raw[1] == 'M') {
        return DecodeBmpDirect(raw);
    }

    int w = 0;
    int h = 0;
    int comp = 0;
    stbi_uc* rgba = stbi_load_from_memory(raw.data(), static_cast<int>(raw.size()), &w, &h, &comp,
                                          kBgraBytesPerPixel);
    if (rgba == nullptr || w <= 0 || h <= 0) {
        if (rgba != nullptr) {
            stbi_image_free(rgba);
        }
        throw std::runtime_error("DecodeBmpOrPng: failed to decode image");
    }

    const std::size_t pixel_count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    Bytes bgra(pixel_count * static_cast<std::size_t>(kBgraBytesPerPixel));
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const std::size_t off = i * static_cast<std::size_t>(kBgraBytesPerPixel);
        bgra[off + kChannelBlue] = rgba[off + kRgbaChannelBlue];
        bgra[off + kChannelGreen] = rgba[off + kRgbaChannelGreen];
        bgra[off + kChannelRed] = rgba[off + kRgbaChannelRed];
        bgra[off + kChannelAlpha] = rgba[off + kRgbaChannelAlpha];
    }
    stbi_image_free(rgba);
    return BgraImage(w, h, std::move(bgra));
}

Bytes BgraImage::EncodePng() const {
    if (empty()) {
        throw std::runtime_error("BgraImage::EncodePng cannot encode an empty image");
    }
    const std::size_t pixel_count =
        static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    Bytes rgba(pixel_count * static_cast<std::size_t>(kBgraBytesPerPixel));
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const std::size_t off = i * static_cast<std::size_t>(kBgraBytesPerPixel);
        rgba[off + kRgbaChannelRed] = pixels_[off + kChannelRed];
        rgba[off + kRgbaChannelGreen] = pixels_[off + kChannelGreen];
        rgba[off + kRgbaChannelBlue] = pixels_[off + kChannelBlue];
        rgba[off + kRgbaChannelAlpha] = pixels_[off + kChannelAlpha];
    }

    Bytes out;
    const int ok = stbi_write_png_to_func(&AppendPngChunk, &out, width_, height_,
                                          kBgraBytesPerPixel, rgba.data(),
                                          width_ * kBgraBytesPerPixel);
    if (ok == 0 || out.empty()) {
        throw std::runtime_error("BgraImage::EncodePng failed");
    }
    return out;
}

void BgraImage::SavePng(const std::filesystem::path& path) const {
    const Bytes png = EncodePng();
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        throw std::runtime_error("BgraImage::SavePng cannot open output file");
    }
    ofs.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!ofs) {
        throw std::runtime_error("BgraImage::SavePng short write");
    }
}

BgraImage BgraImage::LoadPng(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        throw std::runtime_error("BgraImage::LoadPng cannot open input file");
    }
    const auto sz = ifs.tellg();
    if (sz <= 0) {
        throw std::runtime_error("BgraImage::LoadPng empty input file");
    }
    Bytes raw(static_cast<std::size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (!ifs) {
        throw std::runtime_error("BgraImage::LoadPng read failed");
    }
    return DecodeBmpOrPng(raw);
}

bool operator==(const BgraImage& lhs, const BgraImage& rhs) noexcept {
    return lhs.width() == rhs.width() && lhs.height() == rhs.height() &&
           lhs.pixels() == rhs.pixels();
}

bool operator!=(const BgraImage& lhs, const BgraImage& rhs) noexcept {
    return !(lhs == rhs);
}

}  // namespace snu::menu
