// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// PIMPL-insulated 32-bit BGRA sprite editing and typography canvas:
//   * Background restoration: solid fill (`WipeRectSolid`), horizontal border
//     row-median sampling (`WipeRectRowMedian`), clean donor blitting
//     (`BlitCleanRegion`), and premultiplied-alpha Successive Over-Relaxation
//     Laplace inpainting (`InpaintMaskedLaplaceSor`, `InpaintRectLaplaceSor`).
//   * Typography: 3x supersampled (`kSupersampleScale = 3`) Win32 GDI rendering
//     with deterministic font-family fallback resolution, kerning/tracking,
//     width measurement (`MeasureTextWidthAt1x`), font auto-fitting
//     (`AutoFitFontSize`), horizontal squeeze + uniform scale inside safe
//     bounds, multi-radius stroke outline, blurred drop shadow, vertical
//     gradient ink fill, box-filtered 3x -> 1x premultiplied downsample, and
//     90-degree counter-clockwise vertical sidebar layout
//     (`DrawTextRotated90CCW`).
//   * Compositing: `CompositePremulOverStraight` and
//     `CompositeStraightOverStraight`, both leaving `alpha == 0` exterior
//     pixels byte-identical in the destination image.
//
// Insulation: <windows.h> and <wingdi.h> and all HDC/HBITMAP/HFONT handles stay
// strictly inside sprite_canvas.cpp behind SpriteCanvas::Impl.
//
// Thread-compatible: separate SpriteCanvas instances may be used on separate
// threads.
#pragma once

#include "menu/bgra_image.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace snu::menu {

inline constexpr int kSupersampleScale = 3;
inline constexpr int kDefaultFontWeightNormal = 400;
inline constexpr int kDefaultFontWeightSemiBold = 600;
inline constexpr int kDefaultFontWeightBold = 700;
inline constexpr int kDefaultRowMedianMarginPx = 6;
inline constexpr int kDefaultAutoFitMarginPx = 2;

enum class HorizontalAlign {
    kLeft,
    kCenter,
    kRight,
};

enum class VerticalAlign {
    kTop,
    kCenter,
    kBottom,
};

struct TextStyle {
    std::wstring font_family = L"Segoe UI";
    int weight = kDefaultFontWeightNormal;
    bool italic = false;
    bool cleartype_natural = false;
    float size_px = 16.0f;
    float min_font_size_px = 8.0f;
    float tracking_px = 0.0f;
    float line_spacing = 1.0f;

    BgraColor fill{0xFF, 0xFF, 0xFF, 0xFF};
    bool gradient = false;
    BgraColor fill_bottom{0xFF, 0xFF, 0xFF, 0xFF};

    float stroke_px = 0.0f;
    BgraColor stroke_color{0x00, 0x00, 0x00, 0xFF};

    bool shadow = false;
    float shadow_dx = 1.0f;
    float shadow_dy = 1.0f;
    float shadow_blur_px = 1.5f;
    BgraColor shadow_color{0x00, 0x00, 0x00, 0x90};

    HorizontalAlign halign = HorizontalAlign::kCenter;
    VerticalAlign valign = VerticalAlign::kCenter;

    bool wrap = false;
    float min_squeeze = 0.84f;
    float min_scale = 0.55f;
};

struct TextLayoutMetrics {
    bool fits = false;
    float applied_font_size_px = 0.0f;
    float applied_scale = 1.0f;
    float applied_squeeze = 1.0f;
    int line_count = 0;
    int measured_width_px = 0;
    int measured_height_px = 0;
    RectI ink_bounds{};
};

// Composites premultiplied-BGRA `src_premul` over straight-BGRA `dst_straight`
// at `(dst_x, dst_y)`. Pixels where `src_premul` has `alpha == 0` are never
// modified in `dst_straight`.
void CompositePremulOverStraight(BgraImage& dst_straight, const BgraImage& src_premul,
                                 int dst_x = 0, int dst_y = 0);

// Composites straight-BGRA `src` over straight-BGRA `dst` using Porter-Duff
// over at `(dst_x, dst_y)`. Pixels where `src` has `alpha == 0` are left
// byte-identical in `dst`.
void CompositeStraightOverStraight(BgraImage& dst, const BgraImage& src, int dst_x = 0,
                                   int dst_y = 0);

// Resolves the first installed Win32 font family from `candidates`, falling
// back through `{"Segoe UI", "Franklin Gothic Medium", "Trebuchet MS",
// "Georgia", "Palatino Linotype"}` deterministically.
std::wstring ResolveInstalledFontFamily(const std::vector<std::wstring>& candidates = {});

class SpriteCanvas {
public:
    SpriteCanvas(int width, int height);
    explicit SpriteCanvas(BgraImage image);
    SpriteCanvas(const SpriteCanvas& other);
    SpriteCanvas(SpriteCanvas&& other) noexcept;
    SpriteCanvas& operator=(const SpriteCanvas& other);
    SpriteCanvas& operator=(SpriteCanvas&& other) noexcept;
    ~SpriteCanvas();

    static SpriteCanvas LoadPng(const std::filesystem::path& path);

    void WipeRectSolid(const RectI& rect, BgraColor color);

    // Replaces each scanline inside `rect` with the per-channel median of
    // border pixels sampled within `sample_margin_px` immediately left and
    // right of `rect`, preserving horizontal plate/bevel gradients.
    void WipeRectRowMedian(const RectI& rect, int sample_margin_px = kDefaultRowMedianMarginPx);

    // Solves the discrete Laplace equation via premultiplied-alpha Successive
    // Over-Relaxation (`omega = 1.70`) over pixels inside `region` whose
    // corresponding entry in `mask` (`region.w * region.h` bytes, or full
    // `width() * height()` bytes) is non-zero, un-premultiplying back to
    // straight BGRA afterward.
    void InpaintMaskedLaplaceSor(const RectI& region, const std::vector<std::uint8_t>& mask);

    // Convenience overload treating the entire interior of `rect` as masked
    // unknown pixels seeded from its 1-pixel exterior border ring.
    void InpaintRectLaplaceSor(const RectI& rect);

    void BlitCleanRegion(const BgraImage& donor, const RectI& src_rect, int dst_x, int dst_y);

    TextLayoutMetrics DrawText(std::string_view text_utf8, const RectI& safe_bounds,
                               const TextStyle& style);

    // Renders horizontal `DrawText` onto a temporary `(safe_bounds.h, safe_bounds.w)`
    // canvas and rotates 90 degrees counter-clockwise `(x, y) -> (y, safe_bounds.h - 1 - x)`
    // onto `(safe_bounds.x, safe_bounds.y)` via `CompositeStraightOverStraight`.
    TextLayoutMetrics DrawTextRotated90CCW(std::string_view text_utf8, const RectI& safe_bounds,
                                           const TextStyle& style);

    int MeasureTextWidthAt1x(std::string_view text_utf8, const TextStyle& style) const;

    float AutoFitFontSize(std::string_view text_utf8, const RectI& target_box,
                          const TextStyle& base_style,
                          int margin_px = kDefaultAutoFitMarginPx) const;

    TextLayoutMetrics MeasureText(std::string_view text_utf8, const RectI& safe_bounds,
                                  const TextStyle& style) const;

    int width() const noexcept;
    int height() const noexcept;
    BgraColor PixelAt(int x, int y) const;
    const BgraImage& image() const noexcept;
    BgraImage CopyImage() const;

    void SavePng(const std::filesystem::path& path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snu::menu
