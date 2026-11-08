// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// @PURPOSE: Render English typography and patch FVP UI textures in graph.bin.
//
// @CLASSES:
//   fav::ui::Color:          8-bit RGBA color value type.
//   fav::ui::Rect:           Axis-aligned integer rectangle value type.
//   fav::ui::TextStyle:      Typography and outline parameters for Font.
//   fav::ui::ImageCanvas:    2D drawing and background-restoration surface
//                            over an fvp::NvsgImage.
//   fav::ui::Font:           Insulated FreeType glyph renderer with Porter-Duff
//                            compositing and outline generation.
//   fav::ui::PatchUiOptions: Configuration for UI archive patching.
//   fav::ui::UiPatcher:      Insulated Facade orchestrating UI patch commands.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "fvp/fvp_graph.h"

namespace fav::ui {

inline constexpr int kDefaultFontSizePx = 13;
inline constexpr uint8_t kOpaqueAlpha = 255;

struct Color {
    uint8_t r = kOpaqueAlpha;
    uint8_t g = kOpaqueAlpha;
    uint8_t b = kOpaqueAlpha;
    uint8_t a = kOpaqueAlpha;

    static constexpr Color white() noexcept {
        return {kOpaqueAlpha, kOpaqueAlpha, kOpaqueAlpha, kOpaqueAlpha};
    }

    static constexpr Color transparent() noexcept {
        return {0, 0, 0, 0};
    }
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    constexpr int right() const noexcept { return x + width; }
    constexpr int bottom() const noexcept { return y + height; }
};

struct TextStyle {
    int pixel_size = kDefaultFontSizePx;
    bool bold = true;
    Color foreground = Color::white();
    Color outline = Color::transparent();
    int outline_radius = 0;
};

class ImageCanvas {
public:
    // CREATORS
    explicit ImageCanvas(fvp::NvsgImage& image) noexcept;

    // MANIPULATORS
    fvp::NvsgImage& image() noexcept;
    void clear_transparent();
    void clear_top_rows(int row_count);
    void fill_rect(Rect rect, Color color);
    void blend_pixel(int x, int y, Color src, uint8_t coverage);
    void copy_rect_from(const fvp::NvsgImage& src, Rect rect);
    void copy_vertical_offset(Rect rect, int src_dy);
    void interpolate_horizontal(Rect rect);
    void interpolate_vertical(Rect rect);

    // ACCESSORS
    int width() const noexcept;
    int height() const noexcept;
    const fvp::NvsgImage& image() const noexcept;

private:
    // DATA
    fvp::NvsgImage& d_image;
};

class Font {
public:
    // CREATORS
    Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    Font(Font&&) noexcept;
    Font& operator=(Font&&) noexcept;
    ~Font();

    // ACCESSORS
    int measure_width(const std::string& utf8_text, int px_size, bool bold = false) const;

    void draw_left(ImageCanvas& canvas, Rect box,
                   const std::string& utf8_text,
                   const TextStyle& style) const;

    void draw_centered(ImageCanvas& canvas, Rect box,
                       const std::string& utf8_text,
                       const TextStyle& style) const;

    void draw_vertical(ImageCanvas& canvas, int center_x, int top_y, int step_y,
                       const std::string& utf8_text,
                       const TextStyle& style) const;

    void draw_text_left(fvp::NvsgImage& img, int x, int box_y, int box_h,
                        const std::string& utf8_text, int px_size, bool bold,
                        Color fg, Color outline = Color::transparent(),
                        int outline_radius = 0) const;

    void draw_text_centered(fvp::NvsgImage& img, int box_x, int box_y,
                            int box_w, int box_h,
                            const std::string& utf8_text, int px_size, bool bold,
                            Color fg, Color outline = Color::transparent(),
                            int outline_radius = 0) const;

    void draw_text_vertical(fvp::NvsgImage& img, int center_x, int top_y,
                            int step_y, const std::string& utf8_text,
                            int px_size, bool bold, Color fg,
                            Color outline = Color::transparent(),
                            int outline_radius = 0) const;

private:
    // DATA
    struct Impl;
    std::unique_ptr<Impl> d_impl;
};

struct PatchUiOptions {
    std::string game_dir;
    std::string preview_dir;
};

class UiPatcher {
public:
    // CREATORS
    UiPatcher();
    UiPatcher(const UiPatcher&) = delete;
    UiPatcher& operator=(const UiPatcher&) = delete;
    UiPatcher(UiPatcher&&) noexcept;
    UiPatcher& operator=(UiPatcher&&) noexcept;
    ~UiPatcher();

    // ACCESSORS
    int patch_archive(const PatchUiOptions& opts) const;

private:
    // DATA
    struct Impl;
    std::unique_ptr<Impl> d_impl;
};

void save_nvsg_png(const fvp::NvsgImage& img, const std::string& png_path);
int patch_ui_archive(const PatchUiOptions& opts);

}  // namespace fav::ui
