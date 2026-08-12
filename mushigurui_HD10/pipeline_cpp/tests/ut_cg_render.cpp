// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Rendering geometry / colour maths of the narrative-CG step.
//
// Three conventions here are easy to get subtly wrong and are invisible until
// a repacked asset looks off: crop() is half-open while fill_rect() is
// inclusive, the background median averages the two middles for an even count,
// and the centring arithmetic floors rather than truncates.  Each gets a test.
#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "cg/cg_pipeline.h"
#include "cg/image.h"
#include "cg/text_render.h"

using namespace mgi::cg;
using namespace mgi::cg::detail;

namespace {

Image solid(int w, int h, int channels, std::uint8_t v) {
    Image img;
    img.width = w;
    img.height = h;
    img.channels = channels;
    img.px.assign(static_cast<std::size_t>(w) * h * channels, v);
    return img;
}

// A 10x10 whose collected border multiset is exactly 80 samples: the 16 pixels
// in rows{0,1,8,9} x cols{0,1,8,9} appear TWICE (once via the row slices, once
// via the column slices).  `first8` covers row 0 x=2..7 plus row 1 x=2..3.
Image border_case(std::uint8_t doubled, std::uint8_t first8, std::uint8_t rest,
                  std::uint8_t interior) {
    Image img = solid(10, 10, 3, interior);
    const auto is_edge_row = [](int y) { return y <= 1 || y >= 8; };
    const auto is_edge_col = [](int x) { return x <= 1 || x >= 8; };
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x) {
            if (!is_edge_row(y) && !is_edge_col(x)) continue;  // never collected
            std::uint8_t v;
            if (is_edge_row(y) && is_edge_col(x)) v = doubled;
            else if ((y == 0 && x >= 2 && x <= 7) || (y == 1 && x >= 2 && x <= 3))
                v = first8;
            else
                v = rest;
            for (int c = 0; c < 3; ++c) img.at(x, y)[c] = v;
        }
    return img;
}

std::string font_file() {
    const char* p = "C:/Windows/Fonts/arial.ttf";
    return std::filesystem::exists(p) ? p : std::string();
}

}  // namespace

// 32 samples of `doubled` + 8 of `first8` + 40 of `rest` = 80.  With
// doubled == first8 == 100 and rest == 101 the two middles are 100 and 101, so
// the median is 100.5 and the cast truncates to 100.  std::nth_element would
// return the UPPER middle, 101, and shift real pixels.
TEST(CgRender, sample_bg_averages_the_two_middles_on_an_even_count) {
    const Image img = border_case(100, 100, 101, 7);
    const auto bg = sample_bg(img, 0, 0, 10, 10);
    EXPECT_EQ(bg[0], 100u);
    EXPECT_EQ(bg[1], 100u);
    EXPECT_EQ(bg[2], 100u);
}

// The same shape with 40 samples of 40 and 40 of 60: the median is 50 only if
// the 16 overlap pixels are counted twice (dropping the duplicates leaves 24
// vs 40 and the median becomes 60).
TEST(CgRender, sample_bg_edge_collection_order_and_duplication) {
    const Image img = border_case(40, 40, 60, 200);
    const auto bg = sample_bg(img, 0, 0, 10, 10);
    EXPECT_EQ(bg[0], 50u);
}

TEST(CgRender, sample_bg_dimension_gates) {
    // Height 3 fails `arr.shape[0] > 4`, so only the column edges are read --
    // the 250 interior must not leak in.
    Image wide = solid(10, 3, 3, 250);
    for (int y = 0; y < 3; ++y)
        for (int x : {0, 1, 8, 9})
            for (int c = 0; c < 3; ++c) wide.at(x, y)[c] = 10;
    EXPECT_EQ(sample_bg(wide, 0, 0, 10, 3)[0], 10u);

    // 3x3 collects nothing at all.
    const Image tiny = solid(3, 3, 3, 77);
    const auto bg = sample_bg(tiny, 0, 0, 3, 3);
    EXPECT_EQ(bg, (std::array<std::uint8_t, 3>{255, 255, 255}));
}

TEST(CgRender, sample_bg_ignores_alpha) {
    Image rgba = solid(10, 10, 4, 60);
    for (std::size_t i = 3; i < rgba.px.size(); i += 4) rgba.px[i] = 0;  // fully transparent
    EXPECT_EQ(sample_bg(rgba, 0, 0, 10, 10)[0], 60u);  // alpha dropped, not composited
}

TEST(CgRender, fill_rect_is_inclusive) {
    Image img = solid(10, 10, 4, 0);
    fill_rect(img, 2, 3, 5, 7, {1, 2, 3, 255});
    int touched = 0;
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x)
            if (img.at(x, y)[0] == 1) {
                ++touched;
                EXPECT_EQ(img.at(x, y)[3], 255u);
            }
    EXPECT_EQ(touched, (5 - 2 + 1) * (7 - 3 + 1));  // 20, not 12
}

TEST(CgRender, fill_rect_clamps_out_of_range) {
    Image img = solid(4, 4, 3, 0);
    fill_rect(img, -3, -3, 99, 99, {9, 9, 9, 255});
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) EXPECT_EQ(img.at(x, y)[0], 9u);
}

TEST(CgRender, floor_div_negative) {
    EXPECT_EQ(floor_div(-5, 2), -3);  // plain -5 / 2 truncates to -2
    EXPECT_EQ(floor_div(5, 2), 2);
    EXPECT_EQ(floor_div(-4, 2), -2);
    EXPECT_EQ(floor_div(0, 2), 0);
}

TEST(CgRender, parse_hex_colour_fallbacks) {
    EXPECT_EQ(parse_hex("#2a1a0e", "dark"), (std::array<std::uint8_t, 4>{42, 26, 14, 255}));
    EXPECT_EQ(parse_hex("2a1a0e", "dark"), (std::array<std::uint8_t, 4>{42, 26, 14, 255}));
    // ALL leading hashes are stripped, not just one.
    EXPECT_EQ(parse_hex("##ffffff", "dark"),
              (std::array<std::uint8_t, 4>{255, 255, 255, 255}));
    EXPECT_EQ(parse_hex("#xyz", "dark"), (std::array<std::uint8_t, 4>{255, 255, 255, 255}));
    EXPECT_EQ(parse_hex("#xyz", "light"), (std::array<std::uint8_t, 4>{30, 30, 30, 255}));
    // The slices clamp instead of throwing: a 5-char value yields "ff","ff","a".
    EXPECT_EQ(parse_hex("#ffffa", "dark"), (std::array<std::uint8_t, 4>{255, 255, 10, 255}));
    // ...but a 4-char one leaves an EMPTY third slice, which is an error and
    // falls back to the tone default.
    EXPECT_EQ(parse_hex("#ffff", "light"), (std::array<std::uint8_t, 4>{30, 30, 30, 255}));
}

TEST(CgRender, wrap_lines_greedy_and_u3000) {
    const std::string path = font_file();
    if (path.empty()) GTEST_SKIP() << "arial.ttf not present";
    const auto font = Font::load(path, 20);
    ASSERT_TRUE(font);

    // Word splitting treats U+3000 as whitespace too, so the two words are
    // rejoined with a plain space.
    const auto joined = wrap_lines("alpha\xE3\x80\x80"
                                   "beta",
                                   *font, 10000);
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
    if (path.empty()) GTEST_SKIP() << "arial.ttf not present";

    EXPECT_EQ(fit_font("Hello", path, 10000, 10000)->pixel_size(), 36);
    // Nothing fits -> the 8px fallback, never a null font.
    EXPECT_EQ(fit_font("Hello", path, 1, 1)->pixel_size(), 8);

    // The returned size is the LARGEST in [8,36] that fits.
    const int max_w = 90, max_h = 40;
    const auto f = fit_font("Hello", path, max_w, max_h);
    ASSERT_TRUE(f);
    const int sz = f->pixel_size();
    ASSERT_GE(sz, 8);
    ASSERT_LE(sz, 36);
    const auto bb = f->measure("Hello");
    EXPECT_LE(bb.width(), max_w);
    EXPECT_LE(bb.height(), max_h);
    if (sz < 36) {
        const auto bigger = Font::load(path, sz + 1);
        ASSERT_TRUE(bigger);
        const auto bb2 = bigger->measure("Hello");
        EXPECT_TRUE(bb2.width() > max_w || bb2.height() > max_h);
    }
}

// End-to-end smoke: a region with a bbox must erase inclusively with the
// sampled background and keep the image RGBA.
TEST(CgRender, render_translation_inpaints_then_draws) {
    const std::string path = font_file();
    if (path.empty()) GTEST_SKIP() << "arial.ttf not present";

    Image img = solid(64, 40, 4, 200);
    for (int y = 10; y < 30; ++y)
        for (int x = 10; x < 50; ++x) {
            img.at(x, y)[0] = 0;
            img.at(x, y)[1] = 0;
            img.at(x, y)[2] = 0;
        }
    const boost::json::array regions = boost::json::parse(R"([
        {"text_en": "Hi", "bbox": [8, 8, 52, 32], "text_color": "#ff0000", "bg_tone": "dark"}
    ])").get_array();

    const Image out = render_translation(img, regions);
    EXPECT_EQ(out.channels, 4);
    EXPECT_EQ(out.width, 64);
    // The erase covers [8,52] x [8,32] INCLUSIVE, so the pixel at (52, 32) is
    // repainted with the sampled background (200) rather than left alone.
    EXPECT_EQ(out.at(52, 32)[0], 200u);
    EXPECT_EQ(out.at(52, 32)[3], 255u);
}
