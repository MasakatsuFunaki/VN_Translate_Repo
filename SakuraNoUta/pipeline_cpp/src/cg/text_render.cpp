// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "text_render.h"

#include <algorithm>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "common/util.h"

namespace snu::cg {

namespace {

FT_Library ft_library() {
    static FT_Library lib = [] {
        FT_Library l = nullptr;
        FT_Init_FreeType(&l);
        return l;
    }();
    return lib;
}

}  // namespace

struct Font::Impl {
    FT_Face face = nullptr;
    ~Impl() {
        if (face) FT_Done_Face(face);
    }

    int ascender_px() const {
        return static_cast<int>(face->size->metrics.ascender >> 6);
    }

    // Walk glyphs, calling fn(pen_x, glyph) per rendered glyph.  Returns
    // final pen_x (advance width).
    template <typename Fn>
    long walk(const std::string& utf8, Fn&& fn) const {
        long pen_x = 0;
        FT_UInt prev_index = 0;
        const bool has_kerning = FT_HAS_KERNING(face);
        std::size_t i = 0;
        while (i < utf8.size()) {
            char32_t cp = utf8_next(utf8, i);
            FT_UInt gi = FT_Get_Char_Index(face, static_cast<FT_ULong>(cp));
            if (has_kerning && prev_index && gi) {
                FT_Vector delta;
                FT_Get_Kerning(face, prev_index, gi, FT_KERNING_DEFAULT, &delta);
                pen_x += delta.x >> 6;
            }
            if (FT_Load_Glyph(face, gi, FT_LOAD_RENDER) == 0) {
                fn(pen_x, face->glyph);
                pen_x += face->glyph->advance.x >> 6;
            }
            prev_index = gi;
        }
        return pen_x;
    }
};

std::unique_ptr<Font> Font::load(const std::string& path, int pixel_size) {
    FT_Face face = nullptr;
    if (FT_New_Face(ft_library(), path.c_str(), 0, &face) != 0)
        return nullptr;
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size)) != 0) {
        FT_Done_Face(face);
        return nullptr;
    }
    auto font = std::unique_ptr<Font>(new Font());
    font->impl_ = std::make_unique<Impl>();
    font->impl_->face = face;
    font->pixel_size_ = pixel_size;
    return font;
}

Font::~Font() = default;

TextBBox Font::measure(const std::string& utf8) const {
    const int asc = impl_->ascender_px();
    TextBBox bb;
    bool any = false;
    long end_x = impl_->walk(utf8, [&](long pen_x, FT_GlyphSlot g) {
        if (g->bitmap.width == 0 || g->bitmap.rows == 0) return;
        int gx0 = static_cast<int>(pen_x + g->bitmap_left);
        int gy0 = asc - g->bitmap_top;
        int gx1 = gx0 + static_cast<int>(g->bitmap.width);
        int gy1 = gy0 + static_cast<int>(g->bitmap.rows);
        if (!any) {
            bb = {gx0, gy0, gx1, gy1};
            any = true;
        } else {
            bb.x0 = std::min(bb.x0, gx0);
            bb.y0 = std::min(bb.y0, gy0);
            bb.x1 = std::max(bb.x1, gx1);
            bb.y1 = std::max(bb.y1, gy1);
        }
    });
    if (!any) return {0, 0, static_cast<int>(end_x), 0};
    return bb;
}

void Font::draw(Image& img, int x, int y, const std::string& utf8,
                std::array<std::uint8_t, 3> color) const {
    const int asc = impl_->ascender_px();
    impl_->walk(utf8, [&](long pen_x, FT_GlyphSlot g) {
        const FT_Bitmap& bm = g->bitmap;
        if (bm.pixel_mode != FT_PIXEL_MODE_GRAY && bm.pixel_mode != FT_PIXEL_MODE_MONO)
            return;
        for (unsigned row = 0; row < bm.rows; ++row) {
            int py = y + asc - g->bitmap_top + static_cast<int>(row);
            if (py < 0 || py >= img.height) continue;
            for (unsigned col = 0; col < bm.width; ++col) {
                int px_x = x + static_cast<int>(pen_x) + g->bitmap_left +
                           static_cast<int>(col);
                if (px_x < 0 || px_x >= img.width) continue;
                unsigned a;
                if (bm.pixel_mode == FT_PIXEL_MODE_MONO)
                    a = (bm.buffer[row * bm.pitch + (col >> 3)] >> (7 - (col & 7))) & 1
                            ? 255 : 0;
                else
                    a = bm.buffer[row * static_cast<unsigned>(bm.pitch) + col];
                if (a == 0) continue;
                std::uint8_t* d = img.px(px_x, py);
                for (int c = 0; c < 3; ++c)
                    d[c] = static_cast<std::uint8_t>(
                        (color[c] * a + d[c] * (255 - a) + 127) / 255);
            }
        }
    });
}

}  // namespace snu::cg
