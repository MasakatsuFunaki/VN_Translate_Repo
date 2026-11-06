// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Choice-menu screenshot translator (replace_choice_text).
//
// The box detector is pure integer arithmetic over the decoded pixels.  It is
// pinned here on a synthetic image rather than on the shipped JPEGs, because
// JPEG decoding is not bit-exact across implementations and a one-unit
// difference on an edge pixel alone can move a button edge by a column.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "cg/image.h"
#include "choices/choice_render.h"

using namespace mgi;
using namespace mgi::choices::detail;

namespace {

constexpr int W = 200, H = 120;
const std::vector<std::pair<int, int>> BANDS = {{10, 35}, {45, 70}, {80, 105}};

void put(cg::Image& img, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    std::uint8_t* p = img.at(x, y);
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

// Three blue buttons spanning x = [20,180), each with a couple of yellow
// "glyph" pixels on top.
cg::Image make_menu() {
    cg::Image img;
    img.width = W;
    img.height = H;
    img.channels = 3;
    img.px.assign(static_cast<std::size_t>(W) * H * 3, 30);
    for (const auto& [y0, y1] : BANDS) {
        for (int y = y0; y < y1; ++y)
            for (int x = 20; x < 180; ++x) put(img, x, y, 60, 80, 140);
        for (int y = y0 + 10; y < y0 + 12; ++y)
            for (int x = 95; x < 105; ++x) put(img, x, y, 255, 215, 50);
    }
    return img;
}

bool is_yellow(const std::uint8_t* p) {
    return p[0] > 180 && p[1] > 140 && p[2] < 120;
}

}  // namespace

TEST(Choices, detect_choice_boxes_finds_every_button) {
    const auto boxes = detect_choice_boxes(make_menu());
    ASSERT_EQ(boxes.size(), BANDS.size());
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        EXPECT_EQ(boxes[i].x0, 20);
        EXPECT_EQ(boxes[i].x1, 180);  // xs[-1] + 1
        EXPECT_EQ(boxes[i].y0, BANDS[i].first);
        EXPECT_EQ(boxes[i].y1, BANDS[i].second);
    }
}

// Bands closer together than 4 rows are merged, and anything under 20 rows tall
// is dropped -- the two constants that keep JPEG ringing from becoming a box.
TEST(Choices, detect_choice_boxes_merges_and_filters) {
    cg::Image img;
    img.width = W;
    img.height = H;
    img.channels = 3;
    img.px.assign(static_cast<std::size_t>(W) * H * 3, 30);
    for (int y : {5, 6, 7, 8, 9}) {  // 5 rows -> below the 20-row floor
        for (int x = 20; x < 180; ++x) put(img, x, y, 60, 80, 140);
    }
    EXPECT_TRUE(detect_choice_boxes(img).empty());

    // Two 12-row bands separated by 3 empty rows merge into one 27-row band.
    for (int y = 40; y < 52; ++y)
        for (int x = 20; x < 180; ++x) put(img, x, y, 60, 80, 140);
    for (int y = 55; y < 67; ++y)
        for (int x = 20; x < 180; ++x) put(img, x, y, 60, 80, 140);
    const auto boxes = detect_choice_boxes(img);
    ASSERT_EQ(boxes.size(), 1u);
    EXPECT_EQ(boxes[0].y0, 40);
    EXPECT_EQ(boxes[0].y1, 67);
}

TEST(Choices, render_replacement_wipes_the_yellow_glyphs) {
    const std::string font = resolve_font();
    if (font.empty()) GTEST_SKIP() << "no bold font on this box";

    const cg::Image img = make_menu();
    const auto boxes = detect_choice_boxes(img);
    ASSERT_EQ(boxes.size(), 3u);

    // Blank replacements draw nothing, so what is left is the wipe alone: the
    // per-row median of the NON-yellow pixels, i.e. the flat button blue.
    const cg::Image wiped = render_replacement(img, boxes, {" ", " ", " "}, font);
    ASSERT_EQ(wiped.width, W);
    ASSERT_EQ(wiped.channels, 3);
    for (const auto& b : boxes)
        for (int y = b.y0 + 10; y < b.y0 + 12; ++y)
            for (int x = 95; x < 105; ++x) {
                const std::uint8_t* p = wiped.at(x, y);
                EXPECT_FALSE(is_yellow(p)) << "x=" << x << " y=" << y;
                EXPECT_EQ(p[0], 60u) << "x=" << x << " y=" << y;
                EXPECT_EQ(p[1], 80u);
                EXPECT_EQ(p[2], 140u);
            }

    // With real text the same band carries fresh yellow glyph pixels.
    const cg::Image out = render_replacement(img, boxes, {"One", "Two", "Three"}, font);
    int drawn = 0;
    for (const auto& b : boxes)
        for (int y = b.y0 + 4; y < b.y1 - 4; ++y)
            for (int x = b.x0 + 22; x < b.x1 - 22; ++x)
                if (is_yellow(out.at(x, y))) ++drawn;
    EXPECT_GT(drawn, 0);

    // Outside the buttons nothing is touched.
    EXPECT_EQ(out.at(5, 5)[0], 30u);
    EXPECT_EQ(out.at(199, 119)[2], 30u);
}
