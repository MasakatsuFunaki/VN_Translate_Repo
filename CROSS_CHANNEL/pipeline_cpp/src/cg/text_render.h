// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// FreeType text measurement and rasterisation for the narrative-CG renderer.
//
// Glyph placement here is deliberately not pinned to the pixel: CPK repacking
// is unimplemented, so the patched BMPs never reach the game and exist only
// for manual inspection.  The artefacts worth holding to an exact byte format
// are the JSON and the TSV, not these images.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "image.h"

namespace crc::cg {

// Opaque FT_Face holder (PIMPL: FreeType headers stay out of this header).
class Font {
public:
    Font();
    bool valid() const;
    int pixel_size() const;

    struct Impl;
    std::shared_ptr<Impl> impl;
};

struct BBox {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// FT_Set_Pixel_Sizes(face, 0, px) + FT_RENDER_MODE_NORMAL 8-bit AA.  Returns
// an invalid Font on failure.
Font load_font(const std::string& ttf_path, int px);

// The ink box measured from the pen origin, with y counted DOWN from the
// ascender line -- the same convention draw_text takes its `y` in.
BBox text_bbox(const Font& font, const std::string& utf8);

// Alpha-blends the glyph bitmaps over the destination.  `y` is the TOP of the
// ascender line ('la' anchor).
void draw_text(Image& img, int x, int y, const Font& font, const std::string& utf8, Rgb color);

// Largest size in [8, start_size] whose ink box fits (max_w, max_h); falls
// back to `fallback_path` at size 8 when nothing fits.
Font fit_font(const std::string& ttf_path, const std::string& fallback_path,
              const std::string& text, int max_w, int max_h, int start_size = 36);

// Greedy wrap.  Splits on RUNS of whitespace, U+3000 included, dropping empty
// fields -- so runs of spaces never produce zero-width "words".
std::vector<std::string> wrap_lines(const Font& font, const std::string& text, int max_w);

}  // namespace crc::cg
