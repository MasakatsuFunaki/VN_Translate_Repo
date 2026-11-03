// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "ui/ui_patcher.h"

#include "common/util.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace fav::ui {

namespace {

constexpr uint8_t kUtf8AsciiLimit = 0x80u;
constexpr uint8_t kUtf8TwoByteMask = 0xE0u;
constexpr uint8_t kUtf8TwoBytePrefix = 0xC0u;
constexpr uint8_t kUtf8TwoByteLeadPayloadMask = 0x1Fu;
constexpr uint8_t kUtf8ThreeByteMask = 0xF0u;
constexpr uint8_t kUtf8ThreeBytePrefix = 0xE0u;
constexpr uint8_t kUtf8ThreeByteLeadPayloadMask = 0x0Fu;
constexpr uint8_t kUtf8FourByteMask = 0xF8u;
constexpr uint8_t kUtf8FourBytePrefix = 0xF0u;
constexpr uint8_t kUtf8FourByteLeadPayloadMask = 0x07u;
constexpr uint8_t kUtf8ContPayloadMask = 0x3Fu;
constexpr unsigned int kUtf8SixBitShift = 6u;
constexpr unsigned int kUtf8TwelveBitShift = 12u;
constexpr unsigned int kUtf8EighteenBitShift = 18u;

constexpr double kColorChannelMax = 255.0;
constexpr double kAlphaEpsilon = 1e-6;
constexpr long kChannelClampMin = 0l;
constexpr long kChannelClampMax = 255l;
constexpr unsigned int kFreeType26Dot6Shift = 6u;

std::vector<uint32_t> utf8_to_codepoints(const std::string& s) {
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        if (c < kUtf8AsciiLimit) {
            out.push_back(c);
            ++i;
            continue;
        }
        if ((c & kUtf8TwoByteMask) == kUtf8TwoBytePrefix && i + 1 < s.size()) {
            uint32_t cp =
                static_cast<uint32_t>(c & kUtf8TwoByteLeadPayloadMask) << kUtf8SixBitShift;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(s[i + 1]) & kUtf8ContPayloadMask);
            out.push_back(cp);
            i += 2;
            continue;
        }
        if ((c & kUtf8ThreeByteMask) == kUtf8ThreeBytePrefix && i + 2 < s.size()) {
            uint32_t cp =
                static_cast<uint32_t>(c & kUtf8ThreeByteLeadPayloadMask) << kUtf8TwelveBitShift;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(s[i + 1]) & kUtf8ContPayloadMask)
                  << kUtf8SixBitShift;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(s[i + 2]) & kUtf8ContPayloadMask);
            out.push_back(cp);
            i += 3;
            continue;
        }
        if ((c & kUtf8FourByteMask) == kUtf8FourBytePrefix && i + 3 < s.size()) {
            uint32_t cp =
                static_cast<uint32_t>(c & kUtf8FourByteLeadPayloadMask) << kUtf8EighteenBitShift;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(s[i + 1]) & kUtf8ContPayloadMask)
                  << kUtf8TwelveBitShift;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(s[i + 2]) & kUtf8ContPayloadMask)
                  << kUtf8SixBitShift;
            cp |= static_cast<uint32_t>(static_cast<uint8_t>(s[i + 3]) & kUtf8ContPayloadMask);
            out.push_back(cp);
            i += 4;
            continue;
        }
        ++i;
    }
    return out;
}

}  // namespace

ImageCanvas::ImageCanvas(fvp::NvsgImage& image) noexcept : d_image(image) {}

int ImageCanvas::width() const noexcept {
    return static_cast<int>(d_image.width);
}

int ImageCanvas::height() const noexcept {
    return static_cast<int>(d_image.height);
}

fvp::NvsgImage& ImageCanvas::image() noexcept {
    return d_image;
}

const fvp::NvsgImage& ImageCanvas::image() const noexcept {
    return d_image;
}

void ImageCanvas::clear_transparent() {
    if (!d_image.pixels.empty()) {
        std::memset(d_image.pixels.data(), 0, d_image.pixels.size());
    }
}

void ImageCanvas::clear_top_rows(int row_count) {
    const int clamped_rows = std::clamp(row_count, 0, height());
    const size_t byte_count =
        static_cast<size_t>(width()) * static_cast<size_t>(clamped_rows) *
        d_image.bytes_per_pixel();
    if (byte_count > 0 && byte_count <= d_image.pixels.size()) {
        std::memset(d_image.pixels.data(), 0, byte_count);
    }
}

void ImageCanvas::fill_rect(Rect rect, Color color) {
    const size_t bpp = d_image.bytes_per_pixel();
    if (bpp == 0) {
        return;
    }
    const uint8_t packed[fvp::kBgra32BytesPerPixel] = {
        color.b, color.g, color.r, color.a};
    for (int y = rect.y; y < rect.bottom(); ++y) {
        for (int x = rect.x; x < rect.right(); ++x) {
            if (!d_image.in_bounds(x, y)) {
                continue;
            }
            std::memcpy(d_image.pixel_ptr(x, y), packed, bpp);
        }
    }
}

void ImageCanvas::blend_pixel(int x, int y, Color src, uint8_t coverage) {
    if (!d_image.in_bounds(x, y) || coverage == 0 || src.a == 0) {
        return;
    }
    const double a_src = (static_cast<double>(src.a) / kColorChannelMax) *
                         (static_cast<double>(coverage) / kColorChannelMax);
    if (a_src <= 0.0) {
        return;
    }

    uint8_t* p = d_image.pixel_ptr(x, y);
    if (d_image.format == fvp::kNvsgFormatBgr24) {
        p[0] = static_cast<uint8_t>(std::clamp(
            std::lround(src.b * a_src + p[0] * (1.0 - a_src)),
            kChannelClampMin, kChannelClampMax));
        p[1] = static_cast<uint8_t>(std::clamp(
            std::lround(src.g * a_src + p[1] * (1.0 - a_src)),
            kChannelClampMin, kChannelClampMax));
        p[2] = static_cast<uint8_t>(std::clamp(
            std::lround(src.r * a_src + p[2] * (1.0 - a_src)),
            kChannelClampMin, kChannelClampMax));
        return;
    }

    if (d_image.format == fvp::kNvsgFormatBgra32) {
        const double a_dst = static_cast<double>(p[3]) / kColorChannelMax;
        const double a_out = a_src + a_dst * (1.0 - a_src);
        if (a_out <= kAlphaEpsilon) {
            return;
        }
        p[0] = static_cast<uint8_t>(std::clamp(
            std::lround((src.b * a_src + p[0] * a_dst * (1.0 - a_src)) / a_out),
            kChannelClampMin, kChannelClampMax));
        p[1] = static_cast<uint8_t>(std::clamp(
            std::lround((src.g * a_src + p[1] * a_dst * (1.0 - a_src)) / a_out),
            kChannelClampMin, kChannelClampMax));
        p[2] = static_cast<uint8_t>(std::clamp(
            std::lround((src.r * a_src + p[2] * a_dst * (1.0 - a_src)) / a_out),
            kChannelClampMin, kChannelClampMax));
        p[3] = static_cast<uint8_t>(std::clamp(
            std::lround(a_out * kColorChannelMax),
            kChannelClampMin, kChannelClampMax));
    }
}

void ImageCanvas::copy_rect_from(const fvp::NvsgImage& src, Rect rect) {
    const size_t bpp = d_image.bytes_per_pixel();
    if (bpp == 0 || bpp != src.bytes_per_pixel()) {
        return;
    }
    for (int y = rect.y; y < rect.bottom(); ++y) {
        if (y < 0 || y >= height() || y >= static_cast<int>(src.height)) {
            continue;
        }
        for (int x = rect.x; x < rect.right(); ++x) {
            if (x < 0 || x >= width() || x >= static_cast<int>(src.width)) {
                continue;
            }
            std::memcpy(d_image.pixel_ptr(x, y), src.pixel_ptr(x, y), bpp);
        }
    }
}

void ImageCanvas::copy_vertical_offset(Rect rect, int src_dy) {
    const size_t bpp = d_image.bytes_per_pixel();
    if (bpp == 0) {
        return;
    }
    for (int y = rect.y; y < rect.bottom(); ++y) {
        const int src_y = y + src_dy;
        for (int x = rect.x; x < rect.right(); ++x) {
            if (!d_image.in_bounds(x, y) || !d_image.in_bounds(x, src_y)) {
                continue;
            }
            std::memcpy(d_image.pixel_ptr(x, y), d_image.pixel_ptr(x, src_y), bpp);
        }
    }
}

void ImageCanvas::interpolate_horizontal(Rect rect) {
    const size_t bpp = d_image.bytes_per_pixel();
    if (bpp == 0 || width() <= 0) {
        return;
    }
    const int lx = std::max(0, rect.x - 1);
    const int rx = std::min(width() - 1, rect.right());
    const double span = static_cast<double>(std::max(1, rx - lx));
    for (int y = rect.y; y < rect.bottom(); ++y) {
        if (y < 0 || y >= height()) {
            continue;
        }
        const uint8_t* lp = d_image.pixel_ptr(lx, y);
        const uint8_t* rp = d_image.pixel_ptr(rx, y);
        for (int x = rect.x; x < rect.right(); ++x) {
            if (x < 0 || x >= width()) {
                continue;
            }
            const double t = static_cast<double>(x - lx) / span;
            uint8_t* p = d_image.pixel_ptr(x, y);
            for (size_t c = 0; c < bpp; ++c) {
                p[c] = static_cast<uint8_t>(std::clamp(
                    std::lround(lp[c] * (1.0 - t) + rp[c] * t),
                    kChannelClampMin, kChannelClampMax));
            }
        }
    }
}

void ImageCanvas::interpolate_vertical(Rect rect) {
    const size_t bpp = d_image.bytes_per_pixel();
    if (bpp == 0 || height() <= 0) {
        return;
    }
    const int ty = std::max(0, rect.y - 1);
    const int by = std::min(height() - 1, rect.bottom());
    const double span = static_cast<double>(std::max(1, by - ty));
    for (int y = rect.y; y < rect.bottom(); ++y) {
        if (y < 0 || y >= height()) {
            continue;
        }
        const double t = static_cast<double>(y - ty) / span;
        for (int x = rect.x; x < rect.right(); ++x) {
            if (x < 0 || x >= width()) {
                continue;
            }
            const uint8_t* tp = d_image.pixel_ptr(x, ty);
            const uint8_t* bp = d_image.pixel_ptr(x, by);
            uint8_t* p = d_image.pixel_ptr(x, y);
            for (size_t c = 0; c < bpp; ++c) {
                p[c] = static_cast<uint8_t>(std::clamp(
                    std::lround(tp[c] * (1.0 - t) + bp[c] * t),
                    kChannelClampMin, kChannelClampMax));
            }
        }
    }
}

struct Font::Impl {
    FT_Library d_lib = nullptr;
    FT_Face d_regular = nullptr;
    FT_Face d_bold = nullptr;

    Impl() {
        if (FT_Init_FreeType(&d_lib) != 0) {
            throw std::runtime_error("FT_Init_FreeType failed");
        }
        const char* reg_candidates[] = {
            "C:\\Windows\\Fonts\\YuGothM.ttc",
            "C:\\Windows\\Fonts\\meiryo.ttc",
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "C:\\Windows\\Fonts\\arial.ttf",
        };
        for (const char* path : reg_candidates) {
            if (fs::exists(path) && FT_New_Face(d_lib, path, 0, &d_regular) == 0) {
                break;
            }
        }
        const char* bold_candidates[] = {
            "C:\\Windows\\Fonts\\YuGothB.ttc",
            "C:\\Windows\\Fonts\\meiryob.ttc",
            "C:\\Windows\\Fonts\\segoeuib.ttf",
            "C:\\Windows\\Fonts\\arialbd.ttf",
        };
        for (const char* path : bold_candidates) {
            if (fs::exists(path) && FT_New_Face(d_lib, path, 0, &d_bold) == 0) {
                break;
            }
        }
        if (!d_regular && !d_bold) {
            FT_Done_FreeType(d_lib);
            throw std::runtime_error("No usable system font found in C:\\Windows\\Fonts");
        }
        if (!d_regular) {
            d_regular = d_bold;
        }
        if (!d_bold) {
            d_bold = d_regular;
        }
    }

    ~Impl() {
        if (d_bold && d_bold != d_regular) {
            FT_Done_Face(d_bold);
        }
        if (d_regular) {
            FT_Done_Face(d_regular);
        }
        if (d_lib) {
            FT_Done_FreeType(d_lib);
        }
    }

    FT_Face face(bool use_bold, int px_size) const {
        FT_Face f = use_bold ? d_bold : d_regular;
        FT_Set_Pixel_Sizes(f, 0, static_cast<FT_UInt>(px_size));
        return f;
    }

    int cap_ascent(FT_Face f) const {
        if (FT_Load_Char(f, 'H', FT_LOAD_RENDER) == 0) {
            return f->glyph->bitmap_top;
        }
        return static_cast<int>(f->size->metrics.ascender >> kFreeType26Dot6Shift);
    }

    int measure(FT_Face f, const std::vector<uint32_t>& cps) const {
        int pen_x = 0;
        for (uint32_t cp : cps) {
            if (FT_Load_Char(f, cp, FT_LOAD_RENDER) != 0) {
                continue;
            }
            pen_x += static_cast<int>(f->glyph->advance.x >> kFreeType26Dot6Shift);
        }
        return pen_x;
    }

    void render_pass(ImageCanvas& canvas, FT_Face f,
                     const std::vector<uint32_t>& cps,
                     int start_x, int baseline_y, Color color) const {
        int pen_x = start_x;
        for (uint32_t cp : cps) {
            if (FT_Load_Char(f, cp, FT_LOAD_RENDER) != 0) {
                continue;
            }
            const FT_Bitmap& bm = f->glyph->bitmap;
            const int gx0 = pen_x + f->glyph->bitmap_left;
            const int gy0 = baseline_y - f->glyph->bitmap_top;
            for (unsigned int r = 0; r < bm.rows; ++r) {
                for (unsigned int c = 0; c < bm.width; ++c) {
                    const uint8_t cov = bm.buffer[r * bm.pitch + c];
                    if (cov > 0) {
                        canvas.blend_pixel(gx0 + static_cast<int>(c),
                                           gy0 + static_cast<int>(r),
                                           color, cov);
                    }
                }
            }
            pen_x += static_cast<int>(f->glyph->advance.x >> kFreeType26Dot6Shift);
        }
    }

    void render_with_outline(ImageCanvas& canvas, FT_Face f,
                             const std::vector<uint32_t>& cps,
                             int start_x, int baseline_y,
                             const TextStyle& style) const {
        if (style.outline_radius > 0 && style.outline.a > 0) {
            const int r = style.outline_radius;
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    if (r > 1 && dx * dx + dy * dy > r * r + 1) {
                        continue;
                    }
                    render_pass(canvas, f, cps, start_x + dx, baseline_y + dy,
                                style.outline);
                }
            }
        }
        render_pass(canvas, f, cps, start_x, baseline_y, style.foreground);
    }
};

Font::Font() : d_impl(std::make_unique<Impl>()) {}
Font::Font(Font&&) noexcept = default;
Font& Font::operator=(Font&&) noexcept = default;
Font::~Font() = default;

int Font::measure_width(const std::string& utf8_text, int px_size, bool bold) const {
    FT_Face f = d_impl->face(bold, px_size);
    return d_impl->measure(f, utf8_to_codepoints(utf8_text));
}

void Font::draw_left(ImageCanvas& canvas, Rect box,
                     const std::string& utf8_text,
                     const TextStyle& style) const {
    FT_Face f = d_impl->face(style.bold, style.pixel_size);
    const auto cps = utf8_to_codepoints(utf8_text);
    const int cap_h = d_impl->cap_ascent(f);
    const int baseline_y = box.y + (box.height + cap_h) / 2;
    d_impl->render_with_outline(canvas, f, cps, box.x, baseline_y, style);
}

void Font::draw_centered(ImageCanvas& canvas, Rect box,
                         const std::string& utf8_text,
                         const TextStyle& style) const {
    FT_Face f = d_impl->face(style.bold, style.pixel_size);
    const auto cps = utf8_to_codepoints(utf8_text);
    const int tw = d_impl->measure(f, cps);
    const int cap_h = d_impl->cap_ascent(f);
    const int start_x = box.x + (box.width - tw) / 2;
    const int baseline_y = box.y + (box.height + cap_h) / 2;
    d_impl->render_with_outline(canvas, f, cps, start_x, baseline_y, style);
}

void Font::draw_vertical(ImageCanvas& canvas, int center_x, int top_y, int step_y,
                         const std::string& utf8_text,
                         const TextStyle& style) const {
    FT_Face f = d_impl->face(style.bold, style.pixel_size);
    const auto cps = utf8_to_codepoints(utf8_text);
    const int cap_h = d_impl->cap_ascent(f);
    int cur_y = top_y;
    for (uint32_t cp : cps) {
        std::vector<uint32_t> single{cp};
        const int cw = d_impl->measure(f, single);
        const int sx = center_x - cw / 2;
        const int by = cur_y + cap_h;
        d_impl->render_with_outline(canvas, f, single, sx, by, style);
        cur_y += step_y;
    }
}

void Font::draw_text_left(fvp::NvsgImage& img, int x, int box_y, int box_h,
                          const std::string& utf8_text, int px_size, bool bold,
                          Color fg, Color outline, int outline_radius) const {
    ImageCanvas canvas(img);
    draw_left(canvas, Rect{x, box_y, 0, box_h}, utf8_text,
              TextStyle{px_size, bold, fg, outline, outline_radius});
}

void Font::draw_text_centered(fvp::NvsgImage& img, int box_x, int box_y,
                              int box_w, int box_h,
                              const std::string& utf8_text, int px_size, bool bold,
                              Color fg, Color outline, int outline_radius) const {
    ImageCanvas canvas(img);
    draw_centered(canvas, Rect{box_x, box_y, box_w, box_h}, utf8_text,
                  TextStyle{px_size, bold, fg, outline, outline_radius});
}

void Font::draw_text_vertical(fvp::NvsgImage& img, int center_x, int top_y,
                              int step_y, const std::string& utf8_text,
                              int px_size, bool bold, Color fg,
                              Color outline, int outline_radius) const {
    ImageCanvas canvas(img);
    draw_vertical(canvas, center_x, top_y, step_y, utf8_text,
                  TextStyle{px_size, bold, fg, outline, outline_radius});
}

void save_nvsg_png(const fvp::NvsgImage& img, const std::string& png_path) {
    fs::create_directories(fs::u8path(png_path).parent_path());
    const int w = static_cast<int>(img.width);
    const int h = static_cast<int>(img.height);
    const size_t pixel_count = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (img.format == fvp::kNvsgFormatBgr24) {
        std::vector<uint8_t> rgb(pixel_count * fvp::kBgr24BytesPerPixel);
        for (size_t i = 0; i < pixel_count; ++i) {
            const size_t base = i * fvp::kBgr24BytesPerPixel;
            rgb[base + 0] = img.pixels[base + 2];
            rgb[base + 1] = img.pixels[base + 1];
            rgb[base + 2] = img.pixels[base + 0];
        }
        stbi_write_png(png_path.c_str(), w, h,
                       static_cast<int>(fvp::kBgr24BytesPerPixel),
                       rgb.data(),
                       w * static_cast<int>(fvp::kBgr24BytesPerPixel));
        return;
    }
    if (img.format == fvp::kNvsgFormatBgra32) {
        std::vector<uint8_t> rgba(pixel_count * fvp::kBgra32BytesPerPixel);
        for (size_t i = 0; i < pixel_count; ++i) {
            const size_t base = i * fvp::kBgra32BytesPerPixel;
            rgba[base + 0] = img.pixels[base + 2];
            rgba[base + 1] = img.pixels[base + 1];
            rgba[base + 2] = img.pixels[base + 0];
            rgba[base + 3] = img.pixels[base + 3];
        }
        stbi_write_png(png_path.c_str(), w, h,
                       static_cast<int>(fvp::kBgra32BytesPerPixel),
                       rgba.data(),
                       w * static_cast<int>(fvp::kBgra32BytesPerPixel));
    }
}

namespace {

// Protocol class (GoF Strategy) for erasing baked Japanese typography and
// reconstructing clean background artwork before rendering English text.
class BackgroundRestoreStrategy {
public:
    // CREATORS
    virtual ~BackgroundRestoreStrategy();

    // ACCESSORS
    virtual void restore(ImageCanvas& canvas, Rect region) const = 0;
};

BackgroundRestoreStrategy::~BackgroundRestoreStrategy() = default;

class CrossSheetCopyStrategy final : public BackgroundRestoreStrategy {
public:
    // CREATORS
    explicit CrossSheetCopyStrategy(const fvp::NvsgImage& reference_sheet) noexcept
        : d_reference_sheet(reference_sheet) {}

    // ACCESSORS
    void restore(ImageCanvas& canvas, Rect region) const override {
        canvas.copy_rect_from(d_reference_sheet, region);
    }

private:
    // DATA
    const fvp::NvsgImage& d_reference_sheet;
};

class HorizontalGradientRestoreStrategy final : public BackgroundRestoreStrategy {
public:
    // ACCESSORS
    void restore(ImageCanvas& canvas, Rect region) const override {
        canvas.interpolate_horizontal(region);
    }
};

class VerticalGradientRestoreStrategy final : public BackgroundRestoreStrategy {
public:
    // ACCESSORS
    void restore(ImageCanvas& canvas, Rect region) const override {
        canvas.interpolate_vertical(region);
    }
};

// Erases baked anti-aliased glyphs inside semi-transparent uniform-alpha boxes
// (such as alpha == 77 utility buttons) by sampling nearest unoccluded neighbors.
class UniformAlphaNeighborRestoreStrategy final : public BackgroundRestoreStrategy {
public:
    // CREATORS
    UniformAlphaNeighborRestoreStrategy(const fvp::NvsgImage& original,
                                        uint8_t target_alpha) noexcept
        : d_original(original), d_target_alpha(target_alpha) {}

    // ACCESSORS
    void restore(ImageCanvas& canvas, Rect region) const override {
        constexpr int kBorderInsetX = 1;
        constexpr int kBorderInsetY = 2;
        constexpr int kNeighborMaxDy = 2;
        constexpr size_t kAlphaChannelIndex = 3;

        fvp::NvsgImage& dst = canvas.image();
        for (int y = region.y + kBorderInsetY; y < region.bottom() - kBorderInsetY; ++y) {
            for (int x = region.x + kBorderInsetX; x < region.right() - kBorderInsetX; ++x) {
                uint8_t* p = dst.pixel_ptr(x, y);
                if (p[kAlphaChannelIndex] <= d_target_alpha) {
                    continue;
                }
                copy_nearest_clean_pixel(p, region, x, y, kNeighborMaxDy);
            }
        }
    }

private:
    void copy_nearest_clean_pixel(uint8_t* dst_pixel, Rect region,
                                  int x, int y, int max_dy) const {
        constexpr int kBorderInsetX = 1;
        constexpr int kBorderInsetY = 2;
        constexpr size_t kAlphaChannelIndex = 3;

        for (int r = 1; r < region.width; ++r) {
            for (int dy = -max_dy; dy <= max_dy; ++dy) {
                const int ny = y + dy;
                if (ny < region.y + kBorderInsetY || ny >= region.bottom() - kBorderInsetY) {
                    continue;
                }
                for (int dx : {-r, r}) {
                    const int nx = x + dx;
                    if (nx < region.x + kBorderInsetX || nx >= region.right() - kBorderInsetX) {
                        continue;
                    }
                    const uint8_t* q = d_original.pixel_ptr(nx, ny);
                    if (q[kAlphaChannelIndex] == d_target_alpha) {
                        std::memcpy(dst_pixel, q, fvp::kBgra32BytesPerPixel);
                        return;
                    }
                }
            }
        }
    }

    // DATA
    const fvp::NvsgImage& d_original;
    uint8_t d_target_alpha;
};

// Reconstructs a clean unselected sakura button background template by pooling
// unoccluded background pixels (alpha == target_alpha) across multiple button origins.
class MultiOriginButtonTemplate final : public BackgroundRestoreStrategy {
public:
    // CREATORS
    MultiOriginButtonTemplate(const fvp::NvsgImage& original,
                              int width, int height,
                              uint8_t target_alpha,
                              const std::vector<std::pair<int, int>>& origins)
        : d_width(width),
          d_height(height),
          d_pixels(static_cast<size_t>(width) * static_cast<size_t>(height) *
                       fvp::kBgra32BytesPerPixel,
                   0) {
        constexpr size_t kAlphaChannelIndex = 3;
        for (int dy = 0; dy < d_height; ++dy) {
            for (int dx = 0; dx < d_width; ++dx) {
                uint8_t* dst =
                    d_pixels.data() +
                    (static_cast<size_t>(dy) * static_cast<size_t>(d_width) +
                     static_cast<size_t>(dx)) *
                        fvp::kBgra32BytesPerPixel;
                bool found = false;
                for (const auto& [ox, oy] : origins) {
                    const uint8_t* src = original.pixel_ptr(ox + dx, oy + dy);
                    const bool is_border =
                        (dy == 0 || dy == d_height - 1 || dx == 0 || dx == d_width - 1);
                    if (is_border || src[kAlphaChannelIndex] == target_alpha) {
                        std::memcpy(dst, src, fvp::kBgra32BytesPerPixel);
                        found = true;
                        break;
                    }
                }
                if (!found && dx > 0) {
                    std::memcpy(dst, dst - fvp::kBgra32BytesPerPixel,
                                fvp::kBgra32BytesPerPixel);
                }
            }
        }
    }

    // ACCESSORS
    void stamp_onto(ImageCanvas& canvas, int dst_x, int dst_y) const {
        fvp::NvsgImage& img = canvas.image();
        for (int dy = 0; dy < d_height; ++dy) {
            for (int dx = 0; dx < d_width; ++dx) {
                if (!img.in_bounds(dst_x + dx, dst_y + dy)) {
                    continue;
                }
                const uint8_t* src =
                    d_pixels.data() +
                    (static_cast<size_t>(dy) * static_cast<size_t>(d_width) +
                     static_cast<size_t>(dx)) *
                        fvp::kBgra32BytesPerPixel;
                std::memcpy(img.pixel_ptr(dst_x + dx, dst_y + dy),
                            src, fvp::kBgra32BytesPerPixel);
            }
        }
    }

    void restore(ImageCanvas& canvas, Rect region) const override {
        stamp_onto(canvas, region.x, region.y);
    }

private:
    // DATA
    int d_width;
    int d_height;
    std::vector<uint8_t> d_pixels;
};

// Receiver / context holding the source backup archive, shared Font renderer,
// optional PNG preview directory, and staged binary entry replacements.
class ArchivePatchSession {
public:
    // CREATORS
    ArchivePatchSession(std::string backup_bin_path, const Font& font,
                        std::string preview_dir)
        : d_backup_archive(std::move(backup_bin_path)),
          d_font(font),
          d_preview_dir(std::move(preview_dir)) {}

    // MANIPULATORS
    void commit_image(const std::string& name, const fvp::NvsgImage& img) {
        d_replacements[name] = fvp::encode_nvsg(img);
        if (!d_preview_dir.empty()) {
            save_nvsg_png(img, d_preview_dir + "\\" + name + ".png");
        }
    }

    // ACCESSORS
    const Font& font() const noexcept { return d_font; }

    fvp::NvsgImage load_image(const std::string& name) const {
        return d_backup_archive.load_image(name);
    }

    size_t replacement_count() const noexcept {
        return d_replacements.size();
    }

    void repack_to(const std::string& dst_bin_path) const {
        d_backup_archive.repack_to(dst_bin_path, d_replacements);
    }

private:
    // DATA
    fvp::GraphArchive d_backup_archive;
    const Font& d_font;
    std::string d_preview_dir;
    std::unordered_map<std::string, std::vector<uint8_t>> d_replacements;
};

// Protocol class (GoF Command) representing a self-contained UI texture patching
// operation executed against an ArchivePatchSession.
class UiPatchCommand {
public:
    // CREATORS
    virtual ~UiPatchCommand();

    // ACCESSORS
    virtual void execute(ArchivePatchSession& session) const = 0;
};

UiPatchCommand::~UiPatchCommand() = default;

// Composite Command (GoF Composite + Command / MacroCommand) executing an
// ordered sequence of child UiPatchCommand instances.
class CompositeUiPatchCommand final : public UiPatchCommand {
public:
    // MANIPULATORS
    void add(std::unique_ptr<UiPatchCommand> command) {
        d_commands.push_back(std::move(command));
    }

    // ACCESSORS
    void execute(ArchivePatchSession& session) const override {
        for (const auto& cmd : d_commands) {
            cmd->execute(session);
        }
    }

private:
    // DATA
    std::vector<std::unique_ptr<UiPatchCommand>> d_commands;
};

// Patches confirmation dialog frame (menu_bg), prompt messages (menu_*_mes),
// and Yes/No button states (menu_yes_1..2, menu_no_1..2).
class ConfirmationDialogPatchCommand final : public UiPatchCommand {
public:
    // ACCESSORS
    void execute(ArchivePatchSession& session) const override {
        const Font& font = session.font();

        auto menu_bg = session.load_image("menu_bg");
        patch_menu_bg(menu_bg, font);
        session.commit_image("menu_bg", menu_bg);

        const std::pair<const char*, const char*> kPrompts[] = {
            {"menu_exit_mes", "Exit the game?"},
            {"menu_del_mes", "Delete save data?"},
            {"menu_init_mes", "Reset settings to default?"},
            {"menu_ow_mes", "Overwrite save data?"},
            {"menu_qload_mes", "Quick load save data?"},
            {"menu_title_mes", "Return to title screen?"},
            {"menu_undo_mes", "Return to previous choice?"},
        };
        for (const auto& [name, label] : kPrompts) {
            auto img = session.load_image(name);
            patch_prompt_message(img, font, label);
            session.commit_image(name, img);
        }

        const std::tuple<const char*, const char*, bool> kButtons[] = {
            {"menu_yes_1", "Yes", false},
            {"menu_yes_2", "Yes", true},
            {"menu_no_1", "No", false},
            {"menu_no_2", "No", true},
        };
        for (const auto& [name, label, hovered] : kButtons) {
            auto img = session.load_image(name);
            patch_yes_no_button(img, font, label, hovered);
            session.commit_image(name, img);
        }
    }

private:
    static void patch_menu_bg(fvp::NvsgImage& img, const Font& font) {
        constexpr Rect kEraseRect{124, 96, 187, 20};
        constexpr int kCleanSourceDy = -20;
        constexpr Rect kLabelBox{126, 99, 0, 13};
        constexpr TextStyle kLabelStyle{
            13, true, Color::white(), {25, 15, 10, 210}, 1};

        ImageCanvas canvas(img);
        canvas.copy_vertical_offset(kEraseRect, kCleanSourceDy);
        font.draw_left(canvas, kLabelBox, "Show confirmation dialog", kLabelStyle);
    }

    static void patch_prompt_message(fvp::NvsgImage& img, const Font& font,
                                     const std::string& label) {
        constexpr TextStyle kPromptStyle{
            20, true, Color::white(), {30, 20, 15, 205}, 1};

        ImageCanvas canvas(img);
        canvas.clear_transparent();
        font.draw_centered(canvas, Rect{0, 0, canvas.width(), canvas.height()},
                           label, kPromptStyle);
    }

    static void patch_yes_no_button(fvp::NvsgImage& img, const Font& font,
                                    const std::string& label, bool hovered) {
        constexpr int kUnderlineStartRow = 30;
        constexpr int kTextTopOffset = 4;
        constexpr TextStyle kNormalStyle{
            19, true, Color::white(), {30, 20, 15, 195}, 1};
        constexpr TextStyle kHoveredStyle{
            19, true, Color::white(), {255, 230, 85, 220}, 2};

        ImageCanvas canvas(img);
        const int clear_h = std::min(kUnderlineStartRow, canvas.height());
        canvas.clear_top_rows(clear_h);
        font.draw_centered(canvas,
                           Rect{0, kTextTopOffset, canvas.width(), clear_h - kTextTopOffset},
                           label, hovered ? kHoveredStyle : kNormalStyle);
    }
};

// Patches the three full-screen 1280x720 options background sheets (opt_sheet1,
// opt_sheet2, opt_sheet3) using cross-sheet artwork restoration and gradient
// interpolation.
class OptionSheetPatchCommand final : public UiPatchCommand {
public:
    // ACCESSORS
    void execute(ArchivePatchSession& session) const override {
        const Font& font = session.font();
        auto s1 = session.load_image("opt_sheet1");
        auto s2 = session.load_image("opt_sheet2");
        auto s3 = session.load_image("opt_sheet3");

        const fvp::NvsgImage s1_orig = s1;
        const fvp::NvsgImage s2_orig = s2;
        const fvp::NvsgImage s3_orig = s3;

        patch_sheet1(s1, s2_orig, s3_orig, font);
        patch_sheet2(s2, s3_orig, font);
        patch_sheet3(s3, s1_orig, s2_orig, font);

        session.commit_image("opt_sheet1", s1);
        session.commit_image("opt_sheet2", s2);
        session.commit_image("opt_sheet3", s3);
    }

private:
    static constexpr int kRowCount = 6;
    static constexpr int kRowY[kRowCount] = {106, 181, 256, 331, 406, 481};
    static constexpr int kCol1X = 77;
    static constexpr int kCol2X = 472;
    static constexpr int kCol3X = 867;
    static constexpr int kHeaderBoxHeight = 18;
    static constexpr TextStyle kHeaderStyle{
        15, true, Color::white(), {25, 15, 10, 215}, 1};

    static void patch_sidebar_and_title(ImageCanvas& canvas, const Font& font,
                                        const std::string& title) {
        constexpr int kSidebarTopY = 196;
        constexpr int kSidebarBottomY = 360;
        constexpr int kBorderLineEdgeX = 11;
        constexpr int kBorderLineCleanSrcX = 9;
        constexpr int kInterpLeftRefX = 8;
        constexpr int kInterpRightRefX = 37;
        constexpr int kInterpStartX = 12;
        constexpr int kInterpEndX = 36;
        constexpr double kInterpSpan = 26.0;

        constexpr int kSidebarTextCenterX = 24;
        constexpr int kSidebarTextTopY = 202;
        constexpr int kSidebarTextStepY = 18;
        constexpr TextStyle kSidebarStyle{
            15, true, Color::white(), {25, 15, 10, 210}, 1};

        constexpr Rect kTitleEraseRect{450, 60, 380, 28};
        constexpr Rect kTitleDrawRect{450, 62, 380, 24};
        constexpr TextStyle kTitleStyle{
            19, true, Color::white(), {25, 15, 10, 220}, 1};

        fvp::NvsgImage& img = canvas.image();
        for (int y = kSidebarTopY; y <= kSidebarBottomY; ++y) {
            std::memcpy(img.pixel_ptr(kBorderLineEdgeX, y),
                        img.pixel_ptr(kBorderLineCleanSrcX, y),
                        fvp::kBgr24BytesPerPixel);
            const uint8_t* lp = img.pixel_ptr(kInterpLeftRefX, y);
            const uint8_t* rp = img.pixel_ptr(kInterpRightRefX, y);
            for (int x = kInterpStartX; x <= kInterpEndX; ++x) {
                const double t = static_cast<double>(x - kBorderLineEdgeX) / kInterpSpan;
                uint8_t* p = img.pixel_ptr(x, y);
                for (size_t c = 0; c < fvp::kBgr24BytesPerPixel; ++c) {
                    p[c] = static_cast<uint8_t>(std::clamp(
                        std::lround(lp[c] * (1.0 - t) + rp[c] * t),
                        kChannelClampMin, kChannelClampMax));
                }
            }
        }
        font.draw_vertical(canvas, kSidebarTextCenterX, kSidebarTextTopY,
                           kSidebarTextStepY, "SETTINGS", kSidebarStyle);

        const HorizontalGradientRestoreStrategy h_grad;
        h_grad.restore(canvas, kTitleEraseRect);
        font.draw_centered(canvas, kTitleDrawRect, title, kTitleStyle);
    }

    static void smooth_horizontal_seams(fvp::NvsgImage& img, int x0, int x1,
                                        int seam_y0, int seam_y1) {
        for (int y : {seam_y0, seam_y1}) {
            for (int x = x0; x < x1; ++x) {
                uint8_t* p = img.pixel_ptr(x, y);
                const uint8_t* pa = img.pixel_ptr(x, y - 1);
                const uint8_t* pb = img.pixel_ptr(x, y + 1);
                for (size_t c = 0; c < fvp::kBgr24BytesPerPixel; ++c) {
                    p[c] = static_cast<uint8_t>((pa[c] + pb[c]) / 2);
                }
            }
        }
    }

    static void erase_bright_glyph_pixels(fvp::NvsgImage& img, Rect region) {
        constexpr int kBrightnessThreshold = 8;
        const int top_ref_y = region.y - 1;
        const int bot_ref_y = region.bottom();
        const double span = static_cast<double>(bot_ref_y - top_ref_y);

        for (int y = region.y; y < region.bottom(); ++y) {
            const double t = static_cast<double>(y - top_ref_y) / span;
            for (int x = region.x; x < region.right(); ++x) {
                const uint8_t* tp = img.pixel_ptr(x, top_ref_y);
                const uint8_t* bp = img.pixel_ptr(x, bot_ref_y);
                uint8_t* p = img.pixel_ptr(x, y);
                uint8_t bg[fvp::kBgr24BytesPerPixel]{};
                for (size_t c = 0; c < fvp::kBgr24BytesPerPixel; ++c) {
                    bg[c] = static_cast<uint8_t>(std::clamp(
                        std::lround(tp[c] * (1.0 - t) + bp[c] * t),
                        kChannelClampMin, kChannelClampMax));
                }
                if (p[0] > bg[0] + kBrightnessThreshold ||
                    p[1] > bg[1] + kBrightnessThreshold ||
                    p[2] > bg[2] + kBrightnessThreshold) {
                    std::memcpy(p, bg, fvp::kBgr24BytesPerPixel);
                }
            }
        }
    }

    static void patch_sheet1(fvp::NvsgImage& s1, const fvp::NvsgImage& s2_orig,
                             const fvp::NvsgImage& s3_orig, const Font& font) {
        ImageCanvas canvas(s1);
        const CrossSheetCopyStrategy from_s2(s2_orig);
        const CrossSheetCopyStrategy from_s3(s3_orig);
        const VerticalGradientRestoreStrategy v_grad;

        from_s3.restore(canvas, Rect{70, 100, 1080, 345});
        from_s2.restore(canvas, Rect{225, 480, 150, 20});
        from_s3.restore(canvas, Rect{96, 480, 129, 20});
        smooth_horizontal_seams(s1, 96, 225, 485, 499);
        v_grad.restore(canvas, Rect{75, 480, 21, 20});
        erase_bright_glyph_pixels(s1, Rect{470, 480, 172, 20});

        from_s2.restore(canvas, Rect{918, 445, 150, 35});
        from_s3.restore(canvas, Rect{918, 480, 150, 22});
        from_s2.restore(canvas, Rect{918, 502, 150, 53});

        patch_sidebar_and_title(canvas, font, "- System & Presentation Settings -");

        const char* kCol1[kRowCount] = {
            "* Display Mode",
            "* Right-Click Action",
            "* Auto-Move Mouse Cursor",
            "* Auto-Hide Mouse Cursor",
            "* Mouse Cursor Style",
            "* When Window is Inactive",
        };
        const char* kCol2[kRowCount] = {
            "* Window Transition Effect",
            "* Character Sprite Effect",
            "* Event & BG Transition Effect",
            "* Camera Viewpoint Effect",
            "* FVP Zoom Effect",
            "* Skip Effects on Advance",
        };
        constexpr int kCol3RowCount = 4;
        const char* kCol3[kCol3RowCount] = {
            "* Weather & Nature Effects",
            "* Background Autofocus",
            "* Time-of-Day Filter Effect",
            "* Confirmation Dialogs",
        };

        for (int r = 0; r < kRowCount; ++r) {
            font.draw_left(canvas, Rect{kCol1X, kRowY[r], 0, kHeaderBoxHeight},
                           kCol1[r], kHeaderStyle);
            font.draw_left(canvas, Rect{kCol2X, kRowY[r], 0, kHeaderBoxHeight},
                           kCol2[r], kHeaderStyle);
        }
        for (int r = 0; r < kCol3RowCount; ++r) {
            font.draw_left(canvas, Rect{kCol3X, kRowY[r], 0, kHeaderBoxHeight},
                           kCol3[r], kHeaderStyle);
        }

        constexpr int kCheckboxX = 920;
        constexpr int kCheckboxBoxHeight = 13;
        constexpr TextStyle kCheckboxStyle{
            13, true, Color::white(), {25, 15, 10, 215}, 1};
        const std::pair<int, const char*> kCheckboxes[] = {
            {360, "Quick Load"},
            {389, "Overwrite Save Data"},
            {418, "Delete Save Data"},
            {447, "Return to Previous Choice"},
            {476, "Reset Settings to Default"},
            {506, "Return to Title Screen"},
            {535, "Exit Game"},
        };
        for (const auto& [cy, label] : kCheckboxes) {
            font.draw_left(canvas, Rect{kCheckboxX, cy, 0, kCheckboxBoxHeight},
                           label, kCheckboxStyle);
        }
    }

    static void patch_sheet2(fvp::NvsgImage& s2, const fvp::NvsgImage& s3_orig,
                             const Font& font) {
        ImageCanvas canvas(s2);
        const CrossSheetCopyStrategy from_s3(s3_orig);
        const VerticalGradientRestoreStrategy v_grad;

        constexpr int kUpperRowCount = 5;
        for (int r = 0; r < kUpperRowCount; ++r) {
            from_s3.restore(canvas, Rect{75, kRowY[r] - 1, 1150, 20});
        }
        from_s3.restore(canvas, Rect{96, 480, 129, 20});
        smooth_horizontal_seams(s2, 96, 225, 485, 499);
        v_grad.restore(canvas, Rect{75, 480, 21, 20});
        from_s3.restore(canvas, Rect{865, 480, 210, 20});

        patch_sidebar_and_title(canvas, font, "- Volume & Message Settings -");

        const char* kCol1[kRowCount] = {
            "* Master Volume",
            "* BGM Volume",
            "* Voice Volume",
            "* Sound Effects (SE)",
            "* Ambient Sound (BGV)",
            "* System Sound Volume",
        };
        const char* kCol2[kUpperRowCount] = {
            "* Text Display Speed",
            "* Auto-Play Speed (Monologue)",
            "* Auto-Play Speed (Dialogue)",
            "* Message Font",
            "* Window Opacity",
        };
        const char* kCol3[kRowCount] = {
            "* Skip Mode",
            "* Ctrl Key Skip Mode",
            "* Effects on Read Text",
            "* Cancel Skip/Auto at Choices",
            "* Stop Voice on Advance",
            "* Lower BGM During Voice",
        };

        for (int r = 0; r < kRowCount; ++r) {
            font.draw_left(canvas, Rect{kCol1X, kRowY[r], 0, kHeaderBoxHeight},
                           kCol1[r], kHeaderStyle);
            font.draw_left(canvas, Rect{kCol3X, kRowY[r], 0, kHeaderBoxHeight},
                           kCol3[r], kHeaderStyle);
        }
        for (int r = 0; r < kUpperRowCount; ++r) {
            font.draw_left(canvas, Rect{kCol2X, kRowY[r], 0, kHeaderBoxHeight},
                           kCol2[r], kHeaderStyle);
        }
    }

    static void patch_sheet3(fvp::NvsgImage& s3, const fvp::NvsgImage& s1_orig,
                             const fvp::NvsgImage& s2_orig, const Font& font) {
        ImageCanvas canvas(s3);
        const CrossSheetCopyStrategy from_s1(s1_orig);
        const CrossSheetCopyStrategy from_s2(s2_orig);

        from_s1.restore(canvas, Rect{75, 460, 280, 20});
        from_s2.restore(canvas, Rect{865, 460, 245, 20});

        patch_sidebar_and_title(canvas, font, "- Character Voice Volume Settings -");

        constexpr int kSheet3HeaderY = 461;
        font.draw_left(canvas, Rect{kCol1X, kSheet3HeaderY, 0, kHeaderBoxHeight},
                       "* Selected Character Text Color", kHeaderStyle);
        font.draw_left(canvas, Rect{kCol3X, kSheet3HeaderY, 0, kHeaderBoxHeight},
                       "* Selected Character Voice Volume", kHeaderStyle);
    }
};

// Patches the interactive option button sprite atlas (opt_parts) across all
// three button states (Unselected sakura, Hovered peach, Selected white).
class OptionPartsSpritePatchCommand final : public UiPatchCommand {
public:
    // ACCESSORS
    void execute(ArchivePatchSession& session) const override {
        auto img = session.load_image("opt_parts");
        const fvp::NvsgImage orig = img;
        ImageCanvas canvas(img);
        const Font& font = session.font();

        patch_top_page_tabs(canvas, font);

        const UniformAlphaNeighborRestoreStrategy alpha77_restore(orig, kUtilityButtonAlpha);
        patch_utility_buttons(canvas, alpha77_restore, font);
        patch_standard_buttons(canvas, orig, font);

        session.commit_image("opt_parts", img);
    }

private:
    static constexpr uint8_t kUtilityButtonAlpha = 77;
    static constexpr uint8_t kSakuraButtonAlpha = 153;
    static constexpr int kButtonStates = 3;
    static constexpr int kStandardButtonHeight = 30;
    static constexpr int kButtonWidth2Opt = 190;
    static constexpr int kButtonWidth3Opt = 125;
    static constexpr int kButtonWidth4Opt = 93;

    static constexpr Color kPeachFillColor{236, 204, 172, 255};
    static constexpr Color kBrownTextColor{41, 24, 16, 255};
    static constexpr TextStyle kUnselectedTextStyle{
        13, true, Color::white(), {25, 15, 10, 190}, 1};
    static constexpr TextStyle kActiveTextStyle{
        13, true, kBrownTextColor, Color::transparent(), 0};

    static void patch_top_page_tabs(ImageCanvas& canvas, const Font& font) {
        constexpr int kTabWidth = 205;
        constexpr int kTabHeight = 20;
        constexpr int kTabIconWidth = 36;
        constexpr int kTabRightBorder = 1;
        constexpr int kTabLabelWidth = kTabWidth - kTabIconWidth - kTabRightBorder;
        constexpr Color kTabStateFill[kButtonStates] = {
            {41, 24, 16, 89},
            {250, 250, 249, 122},
            Color::white(),
        };

        struct TabSpec {
            int x_base;
            int y0;
            const char* label;
        };
        const TabSpec kTabs[] = {
            {0, 0, "System & Effects"},
            {615, 0, "Volume & Text"},
            {0, 20, "Character Voice"},
        };

        for (const auto& tab : kTabs) {
            for (int s = 0; s < kButtonStates; ++s) {
                const int sx = tab.x_base + s * kTabWidth;
                canvas.fill_rect(
                    Rect{sx + kTabIconWidth, tab.y0 + 1,
                         kTabLabelWidth, kTabHeight - 2},
                    kTabStateFill[s]);
                font.draw_centered(
                    canvas,
                    Rect{sx + kTabIconWidth, tab.y0, kTabLabelWidth, kTabHeight},
                    tab.label,
                    (s == 0) ? kUnselectedTextStyle : kActiveTextStyle);
            }
        }
    }

    static void patch_utility_buttons(ImageCanvas& canvas,
                                      const BackgroundRestoreStrategy& alpha77_restore,
                                      const Font& font) {
        constexpr Rect kResetState0Rect{0, 60, 150, 28};
        constexpr Rect kResetState1Fill{151, 61, 148, 25};
        constexpr Rect kResetState2Fill{301, 61, 148, 25};
        alpha77_restore.restore(canvas, kResetState0Rect);
        canvas.fill_rect(kResetState1Fill, kPeachFillColor);
        canvas.fill_rect(kResetState2Fill, Color::white());
        font.draw_centered(canvas, Rect{0, 60, 150, 28}, "Reset Defaults",
                           kUnselectedTextStyle);
        font.draw_centered(canvas, Rect{150, 60, 150, 28}, "Reset Defaults",
                           kActiveTextStyle);
        font.draw_centered(canvas, Rect{300, 60, 150, 28}, "Reset Defaults",
                           kActiveTextStyle);

        constexpr Rect kFontState0Rect{285, 534, 100, 30};
        constexpr Rect kFontState1Fill{671, 535, 98, 28};
        constexpr Rect kFontState2Fill{771, 535, 98, 28};
        alpha77_restore.restore(canvas, kFontState0Rect);
        canvas.fill_rect(kFontState1Fill, kPeachFillColor);
        canvas.fill_rect(kFontState2Fill, Color::white());
        font.draw_centered(canvas, Rect{285, 534, 100, 30}, "Select Font",
                           kUnselectedTextStyle);
        font.draw_centered(canvas, Rect{670, 534, 100, 30}, "Select Font",
                           kActiveTextStyle);
        font.draw_centered(canvas, Rect{770, 534, 100, 30}, "Select Font",
                           kActiveTextStyle);
    }

    static void paint_three_state_button(ImageCanvas& canvas, const Font& font,
                                         const MultiOriginButtonTemplate& tpl,
                                         int x_base, int y0, int w,
                                         const char* label) {
        tpl.stamp_onto(canvas, x_base, y0);
        canvas.fill_rect(Rect{x_base + w + 1, y0 + 1, w - 2, kStandardButtonHeight - 2},
                         kPeachFillColor);
        canvas.fill_rect(Rect{x_base + 2 * w + 1, y0 + 1, w - 2, kStandardButtonHeight - 2},
                         Color::white());

        font.draw_centered(canvas, Rect{x_base, y0, w, kStandardButtonHeight},
                           label, kUnselectedTextStyle);
        font.draw_centered(canvas, Rect{x_base + w, y0, w, kStandardButtonHeight},
                           label, kActiveTextStyle);
        font.draw_centered(canvas, Rect{x_base + 2 * w, y0, w, kStandardButtonHeight},
                           label, kActiveTextStyle);
    }

    static void patch_standard_buttons(ImageCanvas& canvas,
                                       const fvp::NvsgImage& orig,
                                       const Font& font) {
        std::vector<std::pair<int, int>> orig_190;
        for (int y0 : {118, 148, 238, 358, 388, 418, 448}) {
            orig_190.emplace_back(0, y0);
            orig_190.emplace_back(570, y0);
        }
        std::vector<std::pair<int, int>> orig_125;
        for (int y0 : {88, 208, 268, 328}) {
            orig_125.emplace_back(0, y0);
            orig_125.emplace_back(375, y0);
            orig_125.emplace_back(750, y0);
        }
        std::vector<std::pair<int, int>> orig_93;
        for (int y0 : {178, 298}) {
            orig_93.emplace_back(0, y0);
            orig_93.emplace_back(279, y0);
            orig_93.emplace_back(558, y0);
            orig_93.emplace_back(837, y0);
        }

        const MultiOriginButtonTemplate tpl_190(
            orig, kButtonWidth2Opt, kStandardButtonHeight, kSakuraButtonAlpha, orig_190);
        const MultiOriginButtonTemplate tpl_125(
            orig, kButtonWidth3Opt, kStandardButtonHeight, kSakuraButtonAlpha, orig_125);
        const MultiOriginButtonTemplate tpl_93(
            orig, kButtonWidth4Opt, kStandardButtonHeight, kSakuraButtonAlpha, orig_93);

        struct ButtonSpec {
            int x_base;
            int y0;
            int width;
            const char* label;
        };
        const ButtonSpec kSpecs[] = {
            {0, 88, kButtonWidth3Opt, "Windowed"},
            {375, 88, kButtonWidth3Opt, "Full Screen"},
            {750, 88, kButtonWidth3Opt, "Fixed Aspect"},
            {0, 118, kButtonWidth2Opt, "Hide Window"},
            {570, 118, kButtonWidth2Opt, "Open Menu"},
            {0, 148, kButtonWidth2Opt, "On"},
            {570, 148, kButtonWidth2Opt, "Off"},
            {0, 178, kButtonWidth4Opt, "Never"},
            {279, 178, kButtonWidth4Opt, "5 Sec"},
            {558, 178, kButtonWidth4Opt, "10 Sec"},
            {837, 178, kButtonWidth4Opt, "20 Sec"},
            {0, 208, kButtonWidth3Opt, "System"},
            {375, 208, kButtonWidth3Opt, "Type 1"},
            {750, 208, kButtonWidth3Opt, "Type 2"},
            {0, 238, kButtonWidth2Opt, "Stop"},
            {570, 238, kButtonWidth2Opt, "Continue"},
            {0, 268, kButtonWidth3Opt, "Normal"},
            {375, 268, kButtonWidth3Opt, "Fast"},
            {750, 268, kButtonWidth3Opt, "Off"},
            {0, 298, kButtonWidth4Opt, "All"},
            {279, 298, kButtonWidth4Opt, "Events"},
            {558, 298, kButtonWidth4Opt, "H-Scenes"},
            {837, 298, kButtonWidth4Opt, "Off"},
            {0, 328, kButtonWidth3Opt, "Space Only"},
            {375, 328, kButtonWidth3Opt, "All Inputs"},
            {750, 328, kButtonWidth3Opt, "Off"},
            {0, 358, kButtonWidth2Opt, "Read Text Only"},
            {570, 358, kButtonWidth2Opt, "All Text"},
            {0, 388, kButtonWidth2Opt, "Play Effects"},
            {570, 388, kButtonWidth2Opt, "Skip Effects"},
            {0, 418, kButtonWidth2Opt, "Keep Active"},
            {570, 418, kButtonWidth2Opt, "Cancel at Choice"},
            {0, 448, kButtonWidth2Opt, "Lower BGM"},
            {570, 448, kButtonWidth2Opt, "Keep BGM"},
        };

        for (const auto& spec : kSpecs) {
            const MultiOriginButtonTemplate& tpl =
                (spec.width == kButtonWidth2Opt)
                    ? tpl_190
                    : (spec.width == kButtonWidth3Opt) ? tpl_125 : tpl_93;
            paint_three_state_button(canvas, font, tpl, spec.x_base, spec.y0,
                                     spec.width, spec.label);
        }
    }
};

// Patches the character voice portrait nameplate grids (opt_s_pts1..2,
// opt_m_pts1..2, opt_o_pts1..2, opt_v_pts1..2).
class CharacterPortraitGridPatchCommand final : public UiPatchCommand {
public:
    // ACCESSORS
    void execute(ArchivePatchSession& session) const override {
        const Font& font = session.font();
        const std::tuple<const char*, bool, bool> kPortraitSheets[] = {
            {"opt_s_pts1", false, false},
            {"opt_s_pts2", true, false},
            {"opt_m_pts1", false, false},
            {"opt_m_pts2", true, true},
            {"opt_o_pts1", false, false},
            {"opt_o_pts2", true, true},
            {"opt_v_pts1", false, false},
            {"opt_v_pts2", true, false},
        };
        for (const auto& [name, is_pts2, has_other] : kPortraitSheets) {
            auto img = session.load_image(name);
            patch_portrait_sheet(img, font, is_pts2, has_other);
            session.commit_image(name, img);
        }
    }

private:
    struct CellName {
        int bx;
        int by;
        const char* name;
        bool no_portrait;
    };

    static std::vector<CellName> build_cell_roster(bool is_pts2, bool has_other_gender) {
        if (!is_pts2) {
            return {
                {0, 0, "Kuro", false},
                {1, 0, "Haru", false},
                {2, 0, "Chiwa", false},
                {3, 0, "Himeori", false},
                {0, 1, "Mashiro", false},
                {1, 1, "Toya", false},
                {2, 1, "Asahi", false},
                {3, 1, "Azusaki", false},
            };
        }
        std::vector<CellName> cells = {
            {0, 0, "Nana", false},
            {1, 0, "Tomohito", false},
            {2, 0, "Sol", false},
            {3, 0, "Nacht", false},
            {0, 1, "Taiga", false},
        };
        if (has_other_gender) {
            cells.push_back({1, 1, "Other Male", true});
            cells.push_back({2, 1, "Other Female", true});
        }
        return cells;
    }

    static void patch_portrait_sheet(fvp::NvsgImage& img, const Font& font,
                                     bool is_pts2, bool has_other_gender) {
        constexpr int kNameFontSizePx = 11;
        constexpr int kGapPaddingPx = 5;
        constexpr int kCellCenterX = 67;
        constexpr int kMinGapX = 4;
        constexpr int kMaxGapX = 130;
        constexpr int kBlockStrideX = 270;
        constexpr int kBlockStrideY = 330;
        constexpr int kStateCount = 3;
        constexpr int kStateOffsets[kStateCount][2] = {{0, 0}, {135, 0}, {0, 165}};

        constexpr int kUpperGlyphEraseStartDy = 152;
        constexpr int kUpperGlyphEraseEndDy = 157;
        constexpr int kUpperGlyphEraseStartDx = 14;
        constexpr int kUpperGlyphEraseEndDx = 121;
        constexpr int kPortraitSampleDy = 151;

        constexpr int kBarStartDy = 158;
        constexpr int kBarEndDy = 164;
        constexpr int kBarStartDx = 1;
        constexpr int kBarEndDx = 133;
        constexpr int kNameplateWidth = 135;
        constexpr int kNameplateHeight = 13;

        constexpr uint8_t kCutoutBgBgra[fvp::kBgra32BytesPerPixel] = {16, 24, 41, 3};
        constexpr Color kSelectedFg{236, 204, 172, 255};
        constexpr Color kNameOutline{41, 24, 16, 255};

        ImageCanvas canvas(img);
        const auto cells = build_cell_roster(is_pts2, has_other_gender);

        for (const auto& cell : cells) {
            const int tw = font.measure_width(cell.name, kNameFontSizePx, true);
            const int gap_half = tw / 2 + kGapPaddingPx;
            const int gap_x0 = std::max(kMinGapX, kCellCenterX - gap_half);
            const int gap_x1 = std::min(kMaxGapX, kCellCenterX + gap_half);

            for (int s = 0; s < kStateCount; ++s) {
                const int cx = cell.bx * kBlockStrideX + kStateOffsets[s][0];
                const int cy = cell.by * kBlockStrideY + kStateOffsets[s][1];

                for (int dy = kUpperGlyphEraseStartDy; dy <= kUpperGlyphEraseEndDy; ++dy) {
                    for (int dx = kUpperGlyphEraseStartDx; dx <= kUpperGlyphEraseEndDx; ++dx) {
                        uint8_t* p = img.pixel_ptr(cx + dx, cy + dy);
                        if (cell.no_portrait) {
                            std::memcpy(p, kCutoutBgBgra, fvp::kBgra32BytesPerPixel);
                        } else {
                            const uint8_t* top_p =
                                img.pixel_ptr(cx + dx, cy + kPortraitSampleDy);
                            std::memcpy(p, top_p, fvp::kBgra32BytesPerPixel);
                        }
                    }
                }

                const uint8_t bar_px[fvp::kBgra32BytesPerPixel] = {
                    static_cast<uint8_t>(s == 1 ? 172 : 255),
                    static_cast<uint8_t>(s == 1 ? 204 : 255),
                    static_cast<uint8_t>(s == 1 ? 236 : 255),
                    255,
                };
                for (int dy = kBarStartDy; dy <= kBarEndDy; ++dy) {
                    for (int dx = kBarStartDx; dx <= kBarEndDx; ++dx) {
                        uint8_t* p = img.pixel_ptr(cx + dx, cy + dy);
                        const uint8_t* src_px =
                            (dx >= gap_x0 && dx <= gap_x1) ? kCutoutBgBgra : bar_px;
                        std::memcpy(p, src_px, fvp::kBgra32BytesPerPixel);
                    }
                }

                const TextStyle style{
                    kNameFontSizePx,
                    true,
                    (s == 1) ? kSelectedFg : Color::white(),
                    kNameOutline,
                    1,
                };
                font.draw_centered(
                    canvas,
                    Rect{cx, cy + kPortraitSampleDy, kNameplateWidth, kNameplateHeight},
                    cell.name, style);
            }
        }
    }
};

// Patches hover tooltip descriptions (opt_001..016, opt_101..117, opt_201),
// floating control bar tooltips (fc_*_help), and system menu descriptions (sys_*_M).
class TooltipAndSystemMenuPatchCommand final : public UiPatchCommand {
public:
    // ACCESSORS
    void execute(ArchivePatchSession& session) const override {
        const Font& font = session.font();

        const std::pair<const char*, std::vector<std::string>> kOptTooltips[] = {
            {"opt_001",
             {"Display Mode",
              "  Windowed     : Run the game in a desktop window.",
              "  Full Screen  : Run in full screen (exits full screen when unfocused).",
              "  Fixed Aspect : Run in full screen and remain full screen when unfocused."}},
            {"opt_002",
             {"Right-Click Action",
              "  Hide Window : Temporarily hide the message window on right-click.",
              "  Open Menu   : Open the system menu on right-click."}},
            {"opt_003",
             {"Auto-Move Mouse Cursor",
              "  Off : Do not move the mouse cursor when choices or dialogs appear.",
              "  On  : Automatically snap the cursor to choices and dialogs."}},
            {"opt_004",
             {"Auto-Hide Mouse Cursor",
              "  Never  : Always keep the mouse cursor visible.",
              "  5 Sec  : Hide the cursor after 5 seconds of inactivity.",
              "  10 Sec : Hide the cursor after 10 seconds of inactivity.",
              "  20 Sec : Hide the cursor after 20 seconds of inactivity."}},
            {"opt_005",
             {"Mouse Cursor Style",
              "  System : Use the standard Windows mouse cursor.",
              "  Type 1 : Use custom cursor design 1.",
              "  Type 2 : Use custom cursor design 2."}},
            {"opt_006",
             {"When Window is Inactive",
              "  Stop     : Pause the game while the window is unfocused.",
              "  Continue : Keep the game running while unfocused."}},
            {"opt_007",
             {"Window Transition Effect",
              "  Normal : Animate the message window at normal speed.",
              "  Fast   : Animate the message window at fast speed.",
              "  Off    : Show and hide the message window instantly."}},
            {"opt_008",
             {"Character Sprite Effect",
              "  Normal : Animate character sprites at normal speed.",
              "  Fast   : Animate character sprites at fast speed.",
              "  Off    : Switch character sprites instantly."}},
            {"opt_009",
             {"Event & BG Transition Effect",
              "  Normal : Animate CGs and backgrounds at normal speed.",
              "  Fast   : Animate CGs and backgrounds at fast speed.",
              "  Off    : Switch CGs and backgrounds instantly."}},
            {"opt_010",
             {"Camera Viewpoint Effect",
              "  Normal : Play camera pan and perspective effects at normal speed.",
              "  Fast   : Play camera effects at fast speed.",
              "  Off    : Disable camera viewpoint effects."}},
            {"opt_011",
             {"FVP Zoom Effect",
              "  All      : Enable dynamic zoom effects in all scenes.",
              "  Events   : Enable dynamic zoom effects in normal events only.",
              "  H-Scenes : Enable dynamic zoom effects in H-scenes only.",
              "  Off      : Disable dynamic zoom effects."}},
            {"opt_012",
             {"Skip Effects on Advance",
              "  Space Only : Only skip active effects when pressing Space.",
              "  All Inputs : Skip active effects on any click or key press.",
              "  Off        : Do not skip active effects when advancing."}},
            {"opt_013",
             {"Weather & Nature Effects",
              "  On  : Enable animated weather and nature effects.",
              "  Off : Disable weather and nature effects."}},
            {"opt_014",
             {"Background Autofocus",
              "  On  : Enable background depth-of-field blur effects.",
              "  Off : Disable background autofocus blur."}},
            {"opt_015",
             {"Time-of-Day Filter Effect",
              "  On  : Apply evening/night lighting filters to character sprites.",
              "  Off : Disable time-of-day sprite lighting filters."}},
            {"opt_016",
             {"Confirmation Dialogs",
              "  Checked   : Show a confirmation prompt for this action.",
              "  Unchecked : Perform the action immediately without asking."}},
            {"opt_101",
             {"Master Volume",
              "  0 - 100% : Adjust overall game audio volume."}},
            {"opt_102",
             {"BGM Volume",
              "  0 - 100% : Adjust background music volume."}},
            {"opt_103",
             {"Voice Volume",
              "  0 - 100% : Adjust character voice volume."}},
            {"opt_104",
             {"Sound Effects (SE)",
              "  0 - 100% : Adjust sound effect volume."}},
            {"opt_105",
             {"Ambient Sound (BGV)",
              "  0 - 100% : Adjust ambient background sound volume."}},
            {"opt_106",
             {"System Sound Volume",
              "  0 - 100% : Adjust system UI sound effect volume."}},
            {"opt_107",
             {"Text Display Speed",
              "  1 - 10 spd : Adjust message text display speed."}},
            {"opt_108",
             {"Auto-Play Speed (Monologue)",
              "  1 - 10 spd : Adjust auto-advance speed for narration."}},
            {"opt_109",
             {"Auto-Play Speed (Dialogue)",
              "  1 - 10 spd : Adjust auto-advance speed for voiced lines."}},
            {"opt_110",
             {"Message Font",
              "  Select Font : Change the font used for game text."}},
            {"opt_111",
             {"Window Opacity",
              "  0 - 255 : Adjust the opacity of the message window."}},
            {"opt_112",
             {"Skip Mode",
              "  Read Text Only : Only skip previously read scenes.",
              "  All Text       : Skip both read and unread text."}},
            {"opt_113",
             {"Ctrl Key Skip Mode",
              "  Read Text Only : Holding Ctrl only skips previously read text.",
              "  All Text       : Holding Ctrl skips all text including unread lines."}},
            {"opt_114",
             {"Effects on Read Text",
              "  Play Effects : Play screen transitions normally on read text.",
              "  Skip Effects : Omit transition effects on previously read text."}},
            {"opt_115",
             {"Cancel Skip/Auto at Choices",
              "  Keep Active      : Continue Skip or Auto-Play after picking a choice.",
              "  Cancel at Choice : Stop Skip and Auto-Play when a choice appears."}},
            {"opt_116",
             {"Stop Voice on Advance",
              "  Stop     : Stop current voice line when advancing text.",
              "  Continue : Let voice playback finish after advancing text."}},
            {"opt_117",
             {"Lower BGM During Voice",
              "  Lower BGM : Reduce BGM volume while a character is speaking.",
              "  Keep BGM  : Keep BGM at normal volume during voice playback."}},
            {"opt_201",
             {"Selected Character Text Color & Voice Volume",
              "  R (0 - 255) : Adjust red component of character's dialogue text.",
              "  G (0 - 255) : Adjust green component of character's dialogue text.",
              "  B (0 - 255) : Adjust blue component of character's dialogue text.",
              "  0 - 100%    : Adjust voice playback volume for the selected character."}},
        };
        for (const auto& [name, lines] : kOptTooltips) {
            auto img = session.load_image(name);
            patch_multiline_tooltip(img, font, lines);
            session.commit_image(name, img);
        }

        const std::pair<const char*, const char*> kFcHelps[] = {
            {"fc_back_help", "Backlog"},
            {"fc_load_help", "Load"},
            {"fc_lock_help", "Lock Bar"},
            {"fc_play_help", "Auto-Play"},
            {"fc_qload_help", "Quick Load"},
            {"fc_qsave_help", "Quick Save"},
            {"fc_save_help", "Save"},
            {"fc_skip_help", "Skip"},
            {"fc_system_help", "System Menu"},
            {"fc_voice_help", "Replay Voice"},
            {"fc_wincls_help", "Hide Window"},
        };
        for (const auto& [name, label] : kFcHelps) {
            auto img = session.load_image(name);
            patch_centered_banner(img, font, label, kFcHelpStyle);
            session.commit_image(name, img);
        }

        const std::pair<const char*, const char*> kSysDescriptions[] = {
            {"sys_config_M", "- Change settings -"},
            {"sys_exit_M", "- Exit the game -"},
            {"sys_hidewin_M", "- Hide message window -"},
            {"sys_load_M", "- Load save data -"},
            {"sys_return_M", "- Return to game -"},
            {"sys_save_M", "- Save progress -"},
            {"sys_title_M", "- Return to title -"},
            {"sys_undo_M", "- Return to previous scene -"},
        };
        for (const auto& [name, label] : kSysDescriptions) {
            auto img = session.load_image(name);
            patch_centered_banner(img, font, label, kSysDescriptionStyle);
            session.commit_image(name, img);
        }
    }

private:
    static constexpr TextStyle kFcHelpStyle{
        13, true, Color::white(), {25, 15, 10, 210}, 1};
    static constexpr TextStyle kSysDescriptionStyle{
        18, true, Color::white(), {41, 24, 16, 220}, 2};

    static void patch_multiline_tooltip(fvp::NvsgImage& img, const Font& font,
                                        const std::vector<std::string>& lines) {
        constexpr int kMinLineHeight = 14;
        constexpr int kMaxLineHeight = 20;
        constexpr int kCompactLineThreshold = 16;
        constexpr int kCompactFontSizePx = 12;
        constexpr int kStandardFontSizePx = 13;
        constexpr int kLeftMarginX = 2;

        ImageCanvas canvas(img);
        canvas.clear_transparent();
        const int n = static_cast<int>(lines.size());
        if (n == 0) {
            return;
        }
        const int line_h = std::min(
            kMaxLineHeight, std::max(kMinLineHeight, canvas.height() / n));
        const int px_size =
            (line_h <= kCompactLineThreshold) ? kCompactFontSizePx : kStandardFontSizePx;
        const TextStyle style{px_size, true, Color::white(), {25, 15, 10, 220}, 1};

        int y = std::max(0, (canvas.height() - n * line_h) / 2);
        for (const auto& line : lines) {
            if (!line.empty()) {
                font.draw_left(canvas, Rect{kLeftMarginX, y, 0, line_h}, line, style);
            }
            y += line_h;
        }
    }

    static void patch_centered_banner(fvp::NvsgImage& img, const Font& font,
                                      const std::string& label,
                                      const TextStyle& style) {
        ImageCanvas canvas(img);
        canvas.clear_transparent();
        font.draw_centered(canvas, Rect{0, 0, canvas.width(), canvas.height()},
                           label, style);
    }
};

}  // namespace

struct UiPatcher::Impl {
    CompositeUiPatchCommand d_pipeline;

    Impl() {
        d_pipeline.add(std::make_unique<ConfirmationDialogPatchCommand>());
        d_pipeline.add(std::make_unique<OptionSheetPatchCommand>());
        d_pipeline.add(std::make_unique<OptionPartsSpritePatchCommand>());
        d_pipeline.add(std::make_unique<CharacterPortraitGridPatchCommand>());
        d_pipeline.add(std::make_unique<TooltipAndSystemMenuPatchCommand>());
    }
};

UiPatcher::UiPatcher() : d_impl(std::make_unique<Impl>()) {}
UiPatcher::UiPatcher(UiPatcher&&) noexcept = default;
UiPatcher& UiPatcher::operator=(UiPatcher&&) noexcept = default;
UiPatcher::~UiPatcher() = default;

int UiPatcher::patch_archive(const PatchUiOptions& opts) const {
    const std::string bin_path = opts.game_dir + "\\graph.bin";
    const std::string backup_path = opts.game_dir + "\\graph.bin.backup";

    if (!fs::exists(fs::u8path(bin_path))) {
        log_info("[ERROR] graph.bin not found at: " + bin_path);
        return 1;
    }
    if (!fs::exists(fs::u8path(backup_path))) {
        log_info("Creating backup: " + backup_path);
        std::error_code ec;
        fs::copy_file(fs::u8path(bin_path), fs::u8path(backup_path),
                      fs::copy_options::none, ec);
        if (ec) {
            log_info("[ERROR] Failed to create graph.bin.backup: " + ec.message());
            return 1;
        }
    }

    const Font font;
    ArchivePatchSession session(backup_path, font, opts.preview_dir);
    d_impl->d_pipeline.execute(session);

    log_info("Repacking " + std::to_string(session.replacement_count()) +
             " patched UI textures into " + bin_path + " ...");
    session.repack_to(bin_path);
    log_info("Successfully patched " + bin_path);
    return 0;
}

int patch_ui_archive(const PatchUiOptions& opts) {
    return UiPatcher{}.patch_archive(opts);
}

}  // namespace fav::ui
