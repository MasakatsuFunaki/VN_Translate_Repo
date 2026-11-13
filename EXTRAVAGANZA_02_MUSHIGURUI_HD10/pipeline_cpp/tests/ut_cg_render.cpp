// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Rendering geometry and colour maths of the narrative-CG pipeline.
//
// Three conventions here are easy to get subtly wrong and are invisible until
// a repacked asset looks off: crop() is half-open while fill_rect() is
// inclusive, the background median averages the two middles for an even count,
// and the centring arithmetic floors rather than truncates.  Each gets a test.
#include "cg/cg_pipeline.h"

#include "cg/image.h"
#include "cg/text_render.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace mgi::cg;
using namespace mgi::cg::detail;

namespace {

constexpr int kRgbChannels = 3;
constexpr int kRgbaChannels = 4;
constexpr int kBorderCaseSide = 10;
constexpr int kEdgeBandThickness = 2;
constexpr std::uint8_t kLightToneFallback = 30u;
constexpr int kWrapTestFontPixelSize = 20;
constexpr int kLargeWrapWidth = 10000;
constexpr int kMaxFontPixelSize = 36;
constexpr int kMinFontPixelSize = 8;

Image solid(int w, int h, int channels, std::uint8_t v) {
    Image img;
    img.width = w;
    img.height = h;
    img.channels = channels;
    img.px.assign(static_cast<std::size_t>(w) * h * channels, v);
    return img;
}

bool is_edge_coord(int coord) noexcept {
    return coord < kEdgeBandThickness || coord >= kBorderCaseSide - kEdgeBandThickness;
}

std::uint8_t classify_border_sample(int x,
                                    int y,
                                    std::uint8_t doubled,
                                    std::uint8_t first8,
                                    std::uint8_t rest) noexcept {
    if (is_edge_coord(y) && is_edge_coord(x)) {
        return doubled;
    }
    if ((y == 0 && x >= 2 && x <= 7) || (y == 1 && x >= 2 && x <= 3)) {
        return first8;
    }
    return rest;
}

// A 10x10 whose collected border multiset is exactly 80 samples: the 16 pixels
// in rows{0,1,8,9} x cols{0,1,8,9} appear TWICE (once via the row slices, once
// via the column slices).  `first8` covers row 0 x=2..7 plus row 1 x=2..3.
Image border_case(std::uint8_t doubled,
                  std::uint8_t first8,
                  std::uint8_t rest,
                  std::uint8_t interior) {
    Image img = solid(kBorderCaseSide, kBorderCaseSide, kRgbChannels, interior);
    for (int y = 0; y < kBorderCaseSide; ++y) {
        for (int x = 0; x < kBorderCaseSide; ++x) {
            if (!is_edge_coord(y) && !is_edge_coord(x)) {
                continue;
            }
            const std::uint8_t v = classify_border_sample(x, y, doubled, first8, rest);
            for (int c = 0; c < kRgbChannels; ++c) {
                img.at(x, y)[c] = v;
            }
        }
    }
    return img;
}

std::string font_file() {
    constexpr std::string_view kArialFontPath = "C:/Windows/Fonts/arial.ttf";
    return fs::exists(fs::u8path(kArialFontPath)) ? std::string(kArialFontPath)
                                                  : std::string();
}

}  // namespace

// 32 samples of `doubled` + 8 of `first8` + 40 of `rest` = 80.  With
// doubled == first8 == 100 and rest == 101 the two middles are 100 and 101, so
// the median is 100.5 and the cast truncates to 100.  std::nth_element would
// return the UPPER middle, 101, and shift real pixels.
TEST(CgRender, sample_bg_averages_the_two_middles_on_an_even_count) {
    const Image img = border_case(100u, 100u, 101u, 7u);
    const auto bg = sample_bg(img, 0, 0, kBorderCaseSide, kBorderCaseSide);
    EXPECT_EQ(bg[0], 100u);
    EXPECT_EQ(bg[1], 100u);
    EXPECT_EQ(bg[2], 100u);
}

// The same shape with 40 samples of 40 and 40 of 60: the median is 50 only if
// the 16 overlap pixels are counted twice (dropping the duplicates leaves 24
// vs 40 and the median becomes 60).
TEST(CgRender, sample_bg_edge_collection_order_and_duplication) {
    const Image img = border_case(40u, 40u, 60u, 200u);
    const auto bg = sample_bg(img, 0, 0, kBorderCaseSide, kBorderCaseSide);
    EXPECT_EQ(bg[0], 50u);
}

TEST(CgRender, sample_bg_dimension_gates) {
    // Height 3 fails `arr.shape[0] > 4`, so only the column edges are read --
    // the 250 interior must not leak in.
    constexpr int kWideHeight = 3;
    Image wide = solid(kBorderCaseSide, kWideHeight, kRgbChannels, 250u);
    for (int y = 0; y < kWideHeight; ++y) {
        for (int x : {0, 1, 8, 9}) {
            for (int c = 0; c < kRgbChannels; ++c) {
                wide.at(x, y)[c] = 10u;
            }
        }
    }
    EXPECT_EQ(sample_bg(wide, 0, 0, kBorderCaseSide, kWideHeight)[0], 10u);

    // 3x3 collects nothing at all.
    const Image tiny = solid(kWideHeight, kWideHeight, kRgbChannels, 77u);
    const auto bg = sample_bg(tiny, 0, 0, kWideHeight, kWideHeight);
    EXPECT_EQ(bg, (std::array<std::uint8_t, 3>{kOpaqueAlpha, kOpaqueAlpha, kOpaqueAlpha}));
}

TEST(CgRender, sample_bg_ignores_alpha) {
    Image rgba = solid(kBorderCaseSide, kBorderCaseSide, kRgbaChannels, 60u);
    for (std::size_t i = 3u; i < rgba.px.size(); i += static_cast<std::size_t>(kRgbaChannels)) {
        rgba.px[i] = 0u;  // fully transparent
    }
    EXPECT_EQ(sample_bg(rgba, 0, 0, kBorderCaseSide, kBorderCaseSide)[0], 60u);
}

TEST(CgRender, fill_rect_is_inclusive) {
    Image img = solid(kBorderCaseSide, kBorderCaseSide, kRgbaChannels, 0u);
    fill_rect(img, 2, 3, 5, 7, {1u, 2u, 3u, kOpaqueAlpha});
    int touched = 0;
    for (int y = 0; y < kBorderCaseSide; ++y) {
        for (int x = 0; x < kBorderCaseSide; ++x) {
            if (img.at(x, y)[0] == 1u) {
                ++touched;
                EXPECT_EQ(img.at(x, y)[3], kOpaqueAlpha);
            }
        }
    }
    EXPECT_EQ(touched, (5 - 2 + 1) * (7 - 3 + 1));  // 20, not 12
}

TEST(CgRender, fill_rect_clamps_out_of_range) {
    constexpr int kSmallSide = 4;
    Image img = solid(kSmallSide, kSmallSide, kRgbChannels, 0u);
    fill_rect(img, -3, -3, 99, 99, {9u, 9u, 9u, kOpaqueAlpha});
    for (int y = 0; y < kSmallSide; ++y) {
        for (int x = 0; x < kSmallSide; ++x) {
            EXPECT_EQ(img.at(x, y)[0], 9u);
        }
    }
}

TEST(CgRender, floor_div_negative) {
    EXPECT_EQ(floor_div(-5, 2), -3);  // plain -5 / 2 truncates to -2
    EXPECT_EQ(floor_div(5, 2), 2);
    EXPECT_EQ(floor_div(-4, 2), -2);
    EXPECT_EQ(floor_div(0, 2), 0);
}

TEST(CgRender, parse_hex_colour_fallbacks) {
    EXPECT_EQ(parse_hex("#2a1a0e", "dark"),
              (std::array<std::uint8_t, 4>{42u, 26u, 14u, kOpaqueAlpha}));
    EXPECT_EQ(parse_hex("2a1a0e", "dark"),
              (std::array<std::uint8_t, 4>{42u, 26u, 14u, kOpaqueAlpha}));
    // ALL leading hashes are stripped, not just one.
    EXPECT_EQ(parse_hex("##ffffff", "dark"),
              (std::array<std::uint8_t, 4>{kOpaqueAlpha, kOpaqueAlpha, kOpaqueAlpha, kOpaqueAlpha}));
    EXPECT_EQ(parse_hex("#xyz", "dark"),
              (std::array<std::uint8_t, 4>{kOpaqueAlpha, kOpaqueAlpha, kOpaqueAlpha, kOpaqueAlpha}));
    EXPECT_EQ(parse_hex("#xyz", "light"),
              (std::array<std::uint8_t, 4>{
                  kLightToneFallback, kLightToneFallback, kLightToneFallback, kOpaqueAlpha}));
    // The slices clamp instead of throwing: a 5-char value yields "ff","ff","a".
    EXPECT_EQ(parse_hex("#ffffa", "dark"),
              (std::array<std::uint8_t, 4>{kOpaqueAlpha, kOpaqueAlpha, 10u, kOpaqueAlpha}));
    // A 4-char value leaves an empty third slice and falls back to the tone default.
    EXPECT_EQ(parse_hex("#ffff", "light"),
              (std::array<std::uint8_t, 4>{
                  kLightToneFallback, kLightToneFallback, kLightToneFallback, kOpaqueAlpha}));
}

TEST(CgRender, wrap_lines_greedy_and_u3000) {
    const std::string path = font_file();
    ASSERT_FALSE(path.empty());
    const auto font = Font::load(path, kWrapTestFontPixelSize);
    ASSERT_TRUE(font);

    // Word splitting treats U+3000 as whitespace too, so the two words are
    // rejoined with a plain space.
    const auto joined = wrap_lines("alpha　beta", *font, kLargeWrapWidth);
    ASSERT_EQ(joined.size(), 1u);
    EXPECT_EQ(joined[0], "alpha beta");

    // A break only happens when the candidate overflows AND cur is non-empty,
    // so an over-wide single word still gets its own line.
    const auto narrow = wrap_lines("alpha beta gamma", *font, 1);
    EXPECT_EQ(narrow, (std::vector<std::string>{"alpha", "beta", "gamma"}));

    EXPECT_TRUE(wrap_lines("   ", *font, 100).empty());
}

TEST(CgRender, fit_font_descends_36_to_8) {
    const std::string path = font_file();
    ASSERT_FALSE(path.empty());

    EXPECT_EQ(fit_font("Hello", path, kLargeWrapWidth, kLargeWrapWidth)->pixel_size(),
              kMaxFontPixelSize);
    // Nothing fits -> the 8px fallback, never a null font.
    EXPECT_EQ(fit_font("Hello", path, 1, 1)->pixel_size(), kMinFontPixelSize);

    // The returned size is the LARGEST in [8,36] that fits.
    constexpr int kMaxWidth = 90;
    constexpr int kMaxHeight = 40;
    const auto f = fit_font("Hello", path, kMaxWidth, kMaxHeight);
    ASSERT_TRUE(f);
    const int sz = f->pixel_size();
    ASSERT_GE(sz, kMinFontPixelSize);
    ASSERT_LE(sz, kMaxFontPixelSize);
    const auto bb = f->measure("Hello");
    EXPECT_LE(bb.width(), kMaxWidth);
    EXPECT_LE(bb.height(), kMaxHeight);
    if (sz < kMaxFontPixelSize) {
        const auto bigger = Font::load(path, sz + 1);
        ASSERT_TRUE(bigger);
        const auto bb2 = bigger->measure("Hello");
        EXPECT_TRUE(bb2.width() > kMaxWidth || bb2.height() > kMaxHeight);
    }
}

// End-to-end smoke: a region with a bbox must erase inclusively with the
// sampled background and keep the image RGBA.
TEST(CgRender, render_translation_inpaints_then_draws) {
    const std::string path = font_file();
    ASSERT_FALSE(path.empty());

    constexpr int kCanvasWidth = 64;
    constexpr int kCanvasHeight = 40;
    constexpr std::uint8_t kBackgroundSample = 200u;

    Image img = solid(kCanvasWidth, kCanvasHeight, kRgbaChannels, kBackgroundSample);
    for (int y = 10; y < 30; ++y) {
        for (int x = 10; x < 50; ++x) {
            img.at(x, y)[0] = 0u;
            img.at(x, y)[1] = 0u;
            img.at(x, y)[2] = 0u;
        }
    }
    const boost::json::array regions = boost::json::parse(R"([
        {"text_en": "Hi", "bbox": [8, 8, 52, 32], "text_color": "#ff0000", "bg_tone": "dark"}
    ])").get_array();

    const Image out = render_translation(img, regions);
    EXPECT_EQ(out.channels, kRgbaChannels);
    EXPECT_EQ(out.width, kCanvasWidth);
    // The erase covers [8,52] x [8,32] INCLUSIVE, so the pixel at (52, 32) is
    // repainted with the sampled background (200) rather than left alone.
    EXPECT_EQ(out.at(52, 32)[0], kBackgroundSample);
    EXPECT_EQ(out.at(52, 32)[3], kOpaqueAlpha);
}
