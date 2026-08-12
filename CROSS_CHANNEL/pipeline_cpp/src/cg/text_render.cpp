// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "text_render.h"

#include <algorithm>
#include <limits>
#include <mutex>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace crc::cg {

namespace {

FT_Library& ft_library() {
    static FT_Library lib = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        if (FT_Init_FreeType(&lib) != 0) lib = nullptr;
    });
    return lib;
}

// Codepoints of a UTF-8 string, for glyph lookup.
std::vector<char32_t> codepoints(const std::string& s) {
    std::vector<char32_t> out;
    std::size_t i = 0;
    while (i < s.size()) out.push_back(utf8_next(s, i));
    return out;
}

}  // namespace

struct Font::Impl {
    FT_Face face = nullptr;
    int px = 0;
    ~Impl() {
        if (face) FT_Done_Face(face);
    }
};

Font::Font() = default;
bool Font::valid() const { return impl && impl->face != nullptr; }
int Font::pixel_size() const { return impl ? impl->px : 0; }

Font load_font(const std::string& ttf_path, int px) {
    Font f;
    FT_Library lib = ft_library();
    if (!lib) return f;
    auto impl = std::make_shared<Font::Impl>();
    if (FT_New_Face(lib, ttf_path.c_str(), 0, &impl->face) != 0) return f;
    if (FT_Set_Pixel_Sizes(impl->face, 0, static_cast<FT_UInt>(px)) != 0) return f;
    impl->px = px;
    f.impl = std::move(impl);
    return f;
}

BBox text_bbox(const Font& font, const std::string& utf8) {
    BBox bb;
    if (!font.valid() || utf8.empty()) return bb;
    FT_Face face = font.impl->face;
    const int ascender = static_cast<int>(face->size->metrics.ascender >> 6);

    int pen = 0;
    int x_min = std::numeric_limits<int>::max(), x_max = std::numeric_limits<int>::min();
    int y_min = std::numeric_limits<int>::max(), y_max = std::numeric_limits<int>::min();
    bool any = false;

    for (char32_t cp : codepoints(utf8)) {
        if (FT_Load_Char(face, static_cast<FT_ULong>(cp), FT_LOAD_RENDER) != 0) continue;
        const FT_GlyphSlot g = face->glyph;
        const int left = pen + g->bitmap_left;
        const int top = ascender - g->bitmap_top;
        if (g->bitmap.width > 0 && g->bitmap.rows > 0) {
            x_min = std::min(x_min, left);
            x_max = std::max(x_max, left + static_cast<int>(g->bitmap.width));
            y_min = std::min(y_min, top);
            y_max = std::max(y_max, top + static_cast<int>(g->bitmap.rows));
            any = true;
        }
        pen += static_cast<int>(g->advance.x >> 6);
    }
    if (!any) return bb;
    bb.x0 = x_min;
    bb.y0 = y_min;
    bb.x1 = x_max;
    bb.y1 = y_max;
    return bb;
}

void draw_text(Image& img, int x, int y, const Font& font, const std::string& utf8, Rgb color) {
    if (!font.valid()) return;
    FT_Face face = font.impl->face;
    const int ascender = static_cast<int>(face->size->metrics.ascender >> 6);

    int pen = 0;
    for (char32_t cp : codepoints(utf8)) {
        if (FT_Load_Char(face, static_cast<FT_ULong>(cp), FT_LOAD_RENDER) != 0) continue;
        const FT_GlyphSlot g = face->glyph;
        const int gx = x + pen + g->bitmap_left;
        const int gy = y + ascender - g->bitmap_top;
        for (unsigned row = 0; row < g->bitmap.rows; ++row) {
            const int py = gy + static_cast<int>(row);
            if (py < 0 || py >= img.height) continue;
            for (unsigned col = 0; col < g->bitmap.width; ++col) {
                const int pxx = gx + static_cast<int>(col);
                if (pxx < 0 || pxx >= img.width) continue;
                const unsigned a = g->bitmap.buffer[row * static_cast<unsigned>(g->bitmap.pitch) + col];
                if (a == 0) continue;
                std::uint8_t* d = img.px(pxx, py);
                for (int c = 0; c < 3; ++c)
                    d[c] = static_cast<std::uint8_t>((d[c] * (255 - a) + color[static_cast<std::size_t>(c)] * a) / 255);
            }
        }
        pen += static_cast<int>(g->advance.x >> 6);
    }
}

Font fit_font(const std::string& ttf_path, const std::string& fallback_path,
              const std::string& text, int max_w, int max_h, int start_size) {
    for (int sz = start_size; sz > 7; --sz) {
        Font f = load_font(ttf_path, sz);
        if (!f.valid()) f = load_font(fallback_path, sz);
        if (!f.valid()) break;
        const BBox bb = text_bbox(f, text);
        if (bb.x1 - bb.x0 <= max_w && bb.y1 - bb.y0 <= max_h) return f;
    }
    // Nothing in [8, start_size] fits.  Rather than give up, render at the
    // smallest size and let the text overflow its box -- an oversized label is
    // still readable, a missing one is not.
    Font f = load_font(ttf_path, 8);
    if (!f.valid()) f = load_font(fallback_path, 8);
    return f;
}

std::vector<std::string> wrap_lines(const Font& font, const std::string& text, int max_w) {
    // Split on RUNS of Unicode whitespace (U+3000 included), dropping empty
    // fields, so a double space never yields a zero-width "word".
    std::vector<std::string> words;
    {
        std::string cur;
        std::size_t i = 0;
        while (i < text.size()) {
            const std::size_t start = i;
            const char32_t cp = utf8_next(text, i);
            if (is_unicode_space(cp)) {
                if (!cur.empty()) {
                    words.push_back(cur);
                    cur.clear();
                }
            } else {
                cur.append(text, start, i - start);
            }
        }
        if (!cur.empty()) words.push_back(cur);
    }

    std::vector<std::string> lines;
    std::string cur;
    for (const auto& word : words) {
        const std::string test = trim(cur + " " + word);
        const BBox bb = text_bbox(font, test);
        if (bb.x1 - bb.x0 > max_w && !cur.empty()) {
            lines.push_back(cur);
            cur = word;
        } else {
            cur = test;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

}  // namespace crc::cg
