// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// SpriteCanvas: an editable 32-bit BGRA sprite frame with the operations the
// menu renderers need to replace baked-in Japanese text with English:
//
//   * background restoration -- erase a text region by row-median sampling
//     or by inward border inpainting (onion peel), draw rounded pills, and
//     copy clean pixels from a donor frame;
//   * typography -- GDI text rasterised at 3x and box-filtered down, with
//     weight, italic, tracking, multi-line layout, auto-shrink/squeeze to a
//     safe rectangle, stroke outline, soft drop shadow and vertical gradient
//     fill, composited over the existing pixels so the frame's alpha channel
//     is preserved (text only ever adds coverage);
//   * PNG load/save and contact sheets through the imaging components.
//
// Insulation: <windows.h>/<wingdi.h> and every GDI handle stay in
// sprite_canvas.cpp behind the pimpl.  The public types are standard C++.
//
// Thread-compatible: distinct canvases may be used from distinct threads.
#pragma once

#include "imaging/bgra_image.h"
#include "imaging/contact_sheet.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ama::menu {

using imaging::BgraColor;
using imaging::BgraImage;
using imaging::RectI;

enum class HorizontalAlign { kLeft, kCenter, kRight };
enum class VerticalAlign { kTop, kCenter, kBottom };

// Everything that decides how a run of text looks.  All lengths are in
// destination (1x) pixels; the canvas scales them internally.
struct TextStyle {
    std::wstring font_family = L"Segoe UI";
    int weight = 400;               // GDI FW_* weight (400 normal, 600 semibold, 700 bold)
    bool italic = false;
    float size_px = 16.0F;          // em height (GDI negative lfHeight)
    float tracking_px = 0.0F;       // extra advance added after every glyph
    float line_spacing = 1.0F;      // multiple of the font's natural line height

    BgraColor fill{0xFF, 0xFF, 0xFF, 0xFF};
    bool gradient = false;          // vertical gradient from `fill` (top) ...
    BgraColor fill_bottom{0xFF, 0xFF, 0xFF, 0xFF};  // ... to `fill_bottom`

    float stroke_px = 0.0F;         // outline width; 0 disables
    BgraColor stroke{0x00, 0x00, 0x00, 0xFF};

    bool shadow = false;
    float shadow_dx = 1.0F;
    float shadow_dy = 1.0F;
    float shadow_blur_px = 1.5F;
    BgraColor shadow_color{0x00, 0x00, 0x00, 0x80};

    HorizontalAlign halign = HorizontalAlign::kCenter;
    VerticalAlign valign = VerticalAlign::kCenter;

    bool wrap = false;              // break at spaces to fit the width; '\n' always breaks
    float min_squeeze = 0.80F;      // narrowest horizontal compression before shrinking
    float min_scale = 0.60F;        // smallest uniform size factor allowed
};

// What DrawText / MeasureText actually did.
struct TextLayoutResult {
    bool fits = false;          // every line inside the safe rect, nothing clipped
    float scale = 1.0F;         // uniform size factor applied
    float squeeze = 1.0F;       // horizontal compression applied
    int line_count = 0;
    RectI ink_bounds;           // painted pixels (fill, stroke and shadow)
};

class SpriteCanvas {
public:
    // A transparent `width` x `height` canvas.
    SpriteCanvas(int width, int height);
    explicit SpriteCanvas(BgraImage image);
    SpriteCanvas(const SpriteCanvas& other);
    SpriteCanvas(SpriteCanvas&& other) noexcept;
    SpriteCanvas& operator=(const SpriteCanvas& other);
    SpriteCanvas& operator=(SpriteCanvas&& other) noexcept;
    ~SpriteCanvas();

    static SpriteCanvas LoadPng(const std::filesystem::path& path);

    // ---- manipulators: background restoration --------------------------

    void Fill(const RectI& region, BgraColor color);

    // Replaces each row of `region` with the per-channel median of the
    // pixels on that row within `sample_margin_px` to the left and right of
    // the region (outside it).  Suited to flat or horizontally uniform
    // plates and vertical gradients.
    void EraseWithRowMedian(const RectI& region, int sample_margin_px);

    // Fills `region` inward from its one-pixel border ring by repeated
    // averaging of already-known neighbours (onion peel), then relaxes the
    // interior (Laplace smoothing).  Preserves smooth gradients and soft
    // drop-shadow alpha across the region.
    void EraseWithBorderInpaint(const RectI& region);

    // Same, restricted to pixels where `mask` (same size as the canvas, one
    // byte per pixel, non-zero = unknown) is set; all other pixels are known.
    void EraseMaskedWithInpaint(const std::vector<std::uint8_t>& mask);

    // Anti-aliased rounded rectangle; `radius_px` >= h/2 gives a pill.
    void DrawRoundedPill(const RectI& rect, float radius_px, BgraColor fill,
                         float border_px, BgraColor border);

    // Copies `source_rect` of `source` verbatim (alpha included) to
    // (dst_x, dst_y).  Use with a clean donor frame of the same strip.
    void BlitCleanRegion(const BgraImage& source, const RectI& source_rect, int dst_x,
                         int dst_y);

    // Source-over composite of `source_rect` of `source` at (dst_x, dst_y).
    void CompositeSubRect(const BgraImage& source, const RectI& source_rect, int dst_x,
                          int dst_y);

    // ---- manipulators: typography ---------------------------------------

    // Lays out UTF-8 `text` inside `safe_bounds` and paints it.  The size is
    // reduced first by horizontal squeeze (down to style.min_squeeze), then
    // by uniform scale (down to style.min_scale) until it fits.  When it
    // still does not fit at the limits it is painted at the limits and the
    // result reports fits == false.
    TextLayoutResult DrawText(const std::string& text, const RectI& safe_bounds,
                              const TextStyle& style);

    // ---- accessors ------------------------------------------------------

    // The layout DrawText would produce, without painting.
    TextLayoutResult MeasureText(const std::string& text, const RectI& safe_bounds,
                                 const TextStyle& style) const;

    int width() const;
    int height() const;
    BgraColor PixelAt(int x, int y) const;
    const BgraImage& image() const;

    void SavePng(const std::filesystem::path& path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Writes paged contact sheets of `cells`; see imaging::SaveContactSheetPages.
std::vector<std::filesystem::path> SaveContactSheetGrid(
    const std::vector<imaging::ContactSheetCell>& cells, const std::filesystem::path& directory,
    const std::string& stem, const imaging::ContactSheetLayout& layout);

}  // namespace ama::menu
