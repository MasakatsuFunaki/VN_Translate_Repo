// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "text_render.h"

#include <algorithm>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_STROKER_H

#include "common/util.h"

namespace shin::cg {

namespace {

FT_Library ft_library() {
    static FT_Library lib = [] {
        FT_Library l = nullptr;
        FT_Init_FreeType(&l);
        return l;
    }();
    return lib;
}

// Fixed-point a*b/255.  NOT the naive rounded divide -- the two disagree by a
// unit on a lot of anti-aliased edge pixels, which shows up as a visible fringe
// around every glyph.
inline unsigned muldiv255(unsigned a, unsigned b) {
    const unsigned t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

// One rasterised glyph, positioned relative to the pen origin.
struct GlyphBitmap {
    int left = 0;   // x offset from pen
    int top = 0;    // y offset from the baseline, upward
    unsigned width = 0, rows = 0;
    std::vector<std::uint8_t> gray;  // 8-bit coverage, row-major, pitch == width
};

}  // namespace

struct Font::Impl {
    FT_Face face = nullptr;
    ~Impl() {
        if (face) FT_Done_Face(face);
    }

    int ascender_px() const {
        return static_cast<int>(face->size->metrics.ascender >> 6);
    }

    void copy_bitmap(const FT_Bitmap& bm, GlyphBitmap& out) const {
        out.width = bm.width;
        out.rows = bm.rows;
        out.gray.assign(static_cast<std::size_t>(bm.width) * bm.rows, 0);
        for (unsigned r = 0; r < bm.rows; ++r)
            for (unsigned c = 0; c < bm.width; ++c) {
                std::uint8_t v;
                if (bm.pixel_mode == FT_PIXEL_MODE_MONO)
                    v = (bm.buffer[r * bm.pitch + (c >> 3)] >> (7 - (c & 7))) & 1 ? 255 : 0;
                else
                    v = bm.buffer[r * static_cast<unsigned>(bm.pitch) + c];
                out.gray[static_cast<std::size_t>(r) * bm.width + c] = v;
            }
    }

    // Lay the string out, returning one entry per rendered glyph.  When
    // stroke_width > 0 the glyph outline is expanded with FT_Stroker, exactly
    // as Pillow does for stroke_width.
    std::vector<GlyphBitmap> layout(const std::string& utf8, int stroke_width) const {
        std::vector<GlyphBitmap> out;
        long pen_x = 0;
        FT_UInt prev_index = 0;
        const bool has_kerning = FT_HAS_KERNING(face) != 0;

        FT_Stroker stroker = nullptr;
        if (stroke_width > 0 && FT_Stroker_New(ft_library(), &stroker) == 0)
            FT_Stroker_Set(stroker, static_cast<FT_Fixed>(stroke_width * 64),
                           FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);

        std::size_t i = 0;
        while (i < utf8.size()) {
            const char32_t cp = utf8_next(utf8, i);
            const FT_UInt gi = FT_Get_Char_Index(face, static_cast<FT_ULong>(cp));
            if (has_kerning && prev_index && gi) {
                FT_Vector delta;
                FT_Get_Kerning(face, prev_index, gi, FT_KERNING_DEFAULT, &delta);
                pen_x += delta.x >> 6;
            }
            if (stroker) {
                if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT) == 0) {
                    FT_Glyph glyph = nullptr;
                    if (FT_Get_Glyph(face->glyph, &glyph) == 0) {
                        if (FT_Glyph_StrokeBorder(&glyph, stroker, 0, 1) == 0 &&
                            FT_Glyph_To_Bitmap(&glyph, FT_RENDER_MODE_NORMAL, nullptr, 1) == 0) {
                            auto* bg = reinterpret_cast<FT_BitmapGlyph>(glyph);
                            GlyphBitmap g;
                            g.left = static_cast<int>(pen_x) + bg->left;
                            g.top = bg->top;
                            copy_bitmap(bg->bitmap, g);
                            if (g.width && g.rows) out.push_back(std::move(g));
                        }
                        FT_Done_Glyph(glyph);
                    }
                    pen_x += face->glyph->advance.x >> 6;
                }
            } else if (FT_Load_Glyph(face, gi, FT_LOAD_RENDER) == 0) {
                const FT_GlyphSlot slot = face->glyph;
                if (slot->bitmap.width && slot->bitmap.rows) {
                    GlyphBitmap g;
                    g.left = static_cast<int>(pen_x) + slot->bitmap_left;
                    g.top = slot->bitmap_top;
                    copy_bitmap(slot->bitmap, g);
                    out.push_back(std::move(g));
                }
                pen_x += slot->advance.x >> 6;
            }
            prev_index = gi;
        }
        if (stroker) FT_Stroker_Done(stroker);
        advance_ = pen_x;
        return out;
    }

    mutable long advance_ = 0;
};

std::unique_ptr<Font> Font::load(const std::string& path, int pixel_size) {
    if (pixel_size <= 0) return nullptr;
    FT_Face face = nullptr;
    if (FT_New_Face(ft_library(), path.c_str(), 0, &face) != 0) return nullptr;
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

TextBBox Font::measure(const std::string& utf8, int stroke_width) const {
    const int asc = impl_->ascender_px();
    const auto glyphs = impl_->layout(utf8, stroke_width);
    if (glyphs.empty())
        return {0, 0, static_cast<int>(impl_->advance_), 0};

    TextBBox bb{};
    bool any = false;
    for (const auto& g : glyphs) {
        const int gx0 = g.left;
        const int gy0 = asc - g.top;
        const int gx1 = gx0 + static_cast<int>(g.width);
        const int gy1 = gy0 + static_cast<int>(g.rows);
        if (!any) {
            bb = {gx0, gy0, gx1, gy1};
            any = true;
        } else {
            bb.x0 = std::min(bb.x0, gx0);
            bb.y0 = std::min(bb.y0, gy0);
            bb.x1 = std::max(bb.x1, gx1);
            bb.y1 = std::max(bb.y1, gy1);
        }
    }
    return bb;
}

void Font::draw(Image& img, int x, int y, const std::string& utf8,
                const std::array<std::uint8_t, 4>& rgba, int stroke_width,
                const std::array<std::uint8_t, 4>& stroke_rgba) const {
    const int asc = impl_->ascender_px();

    const auto blit = [&](const std::vector<GlyphBitmap>& glyphs,
                          const std::array<std::uint8_t, 4>& ink) {
        for (const auto& g : glyphs) {
            for (unsigned r = 0; r < g.rows; ++r) {
                const int py = y + asc - g.top + static_cast<int>(r);
                if (py < 0 || py >= img.height) continue;
                for (unsigned c = 0; c < g.width; ++c) {
                    const int px_x = x + g.left + static_cast<int>(c);
                    if (px_x < 0 || px_x >= img.width) continue;
                    const unsigned a = g.gray[static_cast<std::size_t>(r) * g.width + c];
                    if (a == 0) continue;
                    std::uint8_t* d = img.at(px_x, py);
                    for (int ch = 0; ch < img.channels; ++ch)
                        d[ch] = static_cast<std::uint8_t>(muldiv255(d[ch], 255 - a) +
                                                          muldiv255(ink[ch], a));
                }
            }
        }
    };

    // Pillow paints the stroke first, then the fill on top.
    if (stroke_width > 0) blit(impl_->layout(utf8, stroke_width), stroke_rgba);
    blit(impl_->layout(utf8, 0), rgba);
}

void fill_rect(Image& img, int x1, int y1, int x2, int y2,
               const std::array<std::uint8_t, 4>& rgba) {
    const int ylo = std::max(0, y1), yhi = std::min(img.height - 1, y2);
    const int xlo = std::max(0, x1), xhi = std::min(img.width - 1, x2);
    for (int y = ylo; y <= yhi; ++y)
        for (int x = xlo; x <= xhi; ++x) {
            std::uint8_t* d = img.at(x, y);
            for (int c = 0; c < img.channels; ++c) d[c] = rgba[c];
        }
}

}  // namespace shin::cg
