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
#include "choices/choice_render.h"

#include "cg/image.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace mgi;
using namespace mgi::choices::detail;

namespace {

constexpr int kMenuWidth = 200;
constexpr int kMenuHeight = 120;
constexpr int kRgbChannels = 3;
constexpr std::uint8_t kBackgroundLevel = 30u;
constexpr int kButtonLeftX = 20;
constexpr int kButtonRightX = 180;
constexpr std::uint8_t kButtonBlueR = 60u;
constexpr std::uint8_t kButtonBlueG = 80u;
constexpr std::uint8_t kButtonBlueB = 140u;
constexpr std::uint8_t kGlyphYellowR = 255u;
constexpr std::uint8_t kGlyphYellowG = 215u;
constexpr std::uint8_t kGlyphYellowB = 50u;
constexpr std::uint8_t kYellowMinR = 180u;
constexpr std::uint8_t kYellowMinG = 140u;
constexpr std::uint8_t kYellowMaxB = 120u;

const std::vector<std::pair<int, int>> kMenuBands = {{10, 35}, {45, 70}, {80, 105}};

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
    img.width = kMenuWidth;
    img.height = kMenuHeight;
    img.channels = kRgbChannels;
    img.px.assign(static_cast<std::size_t>(kMenuWidth) * kMenuHeight * kRgbChannels,
                  kBackgroundLevel);
    for (const auto& [y0, y1] : kMenuBands) {
        for (int y = y0; y < y1; ++y) {
            for (int x = kButtonLeftX; x < kButtonRightX; ++x) {
                put(img, x, y, kButtonBlueR, kButtonBlueG, kButtonBlueB);
            }
        }
        for (int y = y0 + 10; y < y0 + 12; ++y) {
            for (int x = 95; x < 105; ++x) {
                put(img, x, y, kGlyphYellowR, kGlyphYellowG, kGlyphYellowB);
            }
        }
    }
    return img;
}

bool is_yellow(const std::uint8_t* p) {
    return p[0] > kYellowMinR && p[1] > kYellowMinG && p[2] < kYellowMaxB;
}

}  // namespace

TEST(Choices, detect_choice_boxes_finds_every_button) {
    const auto boxes = detect_choice_boxes(make_menu());
    ASSERT_EQ(boxes.size(), kMenuBands.size());
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        EXPECT_EQ(boxes[i].x0, kButtonLeftX);
        EXPECT_EQ(boxes[i].x1, kButtonRightX);  // xs[-1] + 1
        EXPECT_EQ(boxes[i].y0, kMenuBands[i].first);
        EXPECT_EQ(boxes[i].y1, kMenuBands[i].second);
    }
}

// Bands closer together than 4 rows are merged, and anything under 20 rows tall
// is dropped -- the two constants that keep JPEG ringing from becoming a box.
TEST(Choices, detect_choice_boxes_merges_and_filters) {
    cg::Image img;
    img.width = kMenuWidth;
    img.height = kMenuHeight;
    img.channels = kRgbChannels;
    img.px.assign(static_cast<std::size_t>(kMenuWidth) * kMenuHeight * kRgbChannels,
                  kBackgroundLevel);
    for (int y : {5, 6, 7, 8, 9}) {  // 5 rows -> below the 20-row floor
        for (int x = kButtonLeftX; x < kButtonRightX; ++x) {
            put(img, x, y, kButtonBlueR, kButtonBlueG, kButtonBlueB);
        }
    }
    EXPECT_TRUE(detect_choice_boxes(img).empty());

    // Two 12-row bands separated by 3 empty rows merge into one 27-row band.
    for (int y = 40; y < 52; ++y) {
        for (int x = kButtonLeftX; x < kButtonRightX; ++x) {
            put(img, x, y, kButtonBlueR, kButtonBlueG, kButtonBlueB);
        }
    }
    for (int y = 55; y < 67; ++y) {
        for (int x = kButtonLeftX; x < kButtonRightX; ++x) {
            put(img, x, y, kButtonBlueR, kButtonBlueG, kButtonBlueB);
        }
    }
    const auto boxes = detect_choice_boxes(img);
    ASSERT_EQ(boxes.size(), 1u);
    EXPECT_EQ(boxes[0].y0, 40);
    EXPECT_EQ(boxes[0].y1, 67);
}

TEST(Choices, render_replacement_wipes_the_yellow_glyphs) {
    const std::string font = resolve_font();
    ASSERT_FALSE(font.empty());

    const cg::Image img = make_menu();
    const auto boxes = detect_choice_boxes(img);
    ASSERT_EQ(boxes.size(), kMenuBands.size());

    // Blank replacements draw nothing, so what is left is the wipe alone: the
    // per-row median of the NON-yellow pixels, i.e. the flat button blue.
    const cg::Image wiped = render_replacement(img, boxes, {" ", " ", " "}, font);
    ASSERT_EQ(wiped.width, kMenuWidth);
    ASSERT_EQ(wiped.channels, kRgbChannels);
    for (const auto& b : boxes) {
        for (int y = b.y0 + 10; y < b.y0 + 12; ++y) {
            for (int x = 95; x < 105; ++x) {
                const std::uint8_t* p = wiped.at(x, y);
                EXPECT_FALSE(is_yellow(p)) << "x=" << x << " y=" << y;
                EXPECT_EQ(p[0], kButtonBlueR) << "x=" << x << " y=" << y;
                EXPECT_EQ(p[1], kButtonBlueG);
                EXPECT_EQ(p[2], kButtonBlueB);
            }
        }
    }

    // With real text the same band carries fresh yellow glyph pixels.
    const cg::Image out = render_replacement(img, boxes, {"One", "Two", "Three"}, font);
    int drawn = 0;
    for (const auto& b : boxes) {
        for (int y = b.y0 + 4; y < b.y1 - 4; ++y) {
            for (int x = b.x0 + 22; x < b.x1 - 22; ++x) {
                if (is_yellow(out.at(x, y))) {
                    ++drawn;
                }
            }
        }
    }
    EXPECT_GT(drawn, 0);

    // Outside the buttons nothing is touched.
    EXPECT_EQ(out.at(5, 5)[0], kBackgroundLevel);
    EXPECT_EQ(out.at(kMenuWidth - 1, kMenuHeight - 1)[2], kBackgroundLevel);
}
