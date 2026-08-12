// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// FreeType-based text measurement + rendering.  The anchor is left/ascender:
// draw(x, y) places y at the top of the ascender and measure() reports the ink
// bbox relative to that same origin -- which is what the caller's "height of
// 'Ay' plus 4" line spacing assumes.
#pragma once

#include <array>
#include <memory>
#include <string>

#include "image.h"

namespace shin::cg {

struct TextBBox {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
};

class Font {
public:
    static std::unique_ptr<Font> load(const std::string& path, int pixel_size);
    ~Font();

    // Ink bbox of ONE line, stroke included, relative to the draw origin.
    TextBBox measure(const std::string& utf8, int stroke_width = 0) const;

    // Draw one line with an optional outline stroke.  The ink alpha is always
    // 255, so glyph pixels push the destination alpha toward opaque -- text
    // rendered into a transparent region must not stay transparent.
    void draw(Image& img, int x, int y, const std::string& utf8,
              const std::array<std::uint8_t, 4>& rgba, int stroke_width = 0,
              const std::array<std::uint8_t, 4>& stroke_rgba = {0, 0, 0, 255}) const;

    int pixel_size() const { return pixel_size_; }

private:
    Font() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int pixel_size_ = 0;
};

// INCLUSIVE OF BOTH CORNERS -- unlike crop(), which is half-open.  A half-open
// loop here leaves the right column and bottom row of every erased bbox
// carrying a 1px ghost of the Japanese glyph edges.
void fill_rect(Image& img, int x1, int y1, int x2, int y2,
               const std::array<std::uint8_t, 4>& rgba);

}  // namespace shin::cg
