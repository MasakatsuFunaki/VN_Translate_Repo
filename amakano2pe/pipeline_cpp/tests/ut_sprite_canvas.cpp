// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/core/sprite_canvas.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <vector>

using namespace ama::menu;
using ama::imaging::ContactSheetCell;
using ama::imaging::ContactSheetLayout;
using ama::imaging::kOpaqueAlpha;

namespace {

constexpr int kDefaultCanvasW = 240;
constexpr int kDefaultCanvasH = 96;
constexpr int kSmallCanvasW = 80;
constexpr int kSmallCanvasH = 48;
constexpr int kGradientCanvasW = 120;
constexpr int kGradientCanvasH = 80;

constexpr BgraColor kPlateBase{0x38, 0x48, 0x58, kOpaqueAlpha};
constexpr BgraColor kSemiTransparentBase{0x40, 0x80, 0xC0, 0x70};
constexpr BgraColor kWhiteOpaque{0xFF, 0xFF, 0xFF, kOpaqueAlpha};
constexpr BgraColor kDarkStroke{0x18, 0x10, 0x24, kOpaqueAlpha};
constexpr BgraColor kGoldTop{0x60, 0xEC, 0xFF, kOpaqueAlpha};
constexpr BgraColor kGoldBottom{0x18, 0x88, 0xE8, kOpaqueAlpha};
constexpr BgraColor kSoftShadow{0x00, 0x00, 0x00, 0xB0};
constexpr BgraColor kPillFill{0x44, 0x66, 0xAA, 0xE0};
constexpr BgraColor kPillBorder{0xE8, 0xF0, 0xFF, kOpaqueAlpha};
constexpr BgraColor kSyntheticGlyphInk{0x10, 0x10, 0xF0, kOpaqueAlpha};

constexpr RectI kSafeBox{24, 16, 192, 56};
constexpr RectI kNarrowSafeBox{16, 16, 110, 36};
constexpr RectI kTinySafeBox{10, 10, 36, 16};
constexpr RectI kEraseHole{36, 20, 44, 32};
constexpr RectI kGlyphStamp{48, 26, 20, 16};
constexpr RectI kPillBox{20, 14, 160, 44};

constexpr int kRowMedianMarginPx = 12;
constexpr int kLinearGradientBaseR = 20;
constexpr int kLinearGradientBaseG = 30;
constexpr int kLinearGradientBaseB = 220;
constexpr int kLinearGradientStepX = 1;
constexpr int kLinearGradientStepY = 2;
constexpr int kMaxGradientInpaintError = 2;

constexpr float kBodyFontPx = 20.0F;
constexpr float kTitleFontPx = 26.0F;
constexpr int kWeightRegular = 400;
constexpr int kWeightSemibold = 600;
constexpr int kWeightBold = 700;
constexpr float kLightStrokePx = 1.5F;
constexpr float kMediumStrokePx = 2.0F;
constexpr float kHeavyStrokePx = 2.5F;
constexpr float kPillRadiusPx = 22.0F;
constexpr float kPillBorderPx = 2.0F;
constexpr float kTrackingSubtlePx = 0.5F;
constexpr float kTrackingMediumPx = 1.0F;
constexpr float kMinSqueezeLimit = 0.80F;
constexpr float kMinScaleLimit = 0.60F;
constexpr float kFullFactor = 1.0F;

constexpr int kDonorW = 40;
constexpr int kDonorH = 30;
constexpr int kNegativeClipOffset = -8;
constexpr int kNearRightEdgeX = 64;
constexpr int kNearBottomEdgeY = 34;
constexpr int kExpectedWrappedLines = 2;
constexpr int kSemiAlphaRampStep = 3;
constexpr int kMaxAlphaInpaintError = 3;

constexpr int kSampleSheetW = 640;
constexpr int kSampleSheetH = 420;
constexpr int kSampleBandX = 24;
constexpr int kSampleBandW = 592;
constexpr int kSampleBandH = 64;
constexpr int kSampleBandStepY = 78;
constexpr int kSampleBandStartY = 20;
constexpr int kSampleInsetX = 16;
constexpr int kSampleInsetY = 6;
constexpr int kSampleSqueezedW = 460;
constexpr std::size_t kHandwrittenRowIndex = 4U;
constexpr float kSamplePillRadius = 14.0F;
constexpr float kSamplePillBorder = 1.5F;
constexpr BgraColor kSampleBackdrop{0x22, 0x1E, 0x28, kOpaqueAlpha};
constexpr BgraColor kSampleBandFill{0x38, 0x32, 0x44, kOpaqueAlpha};
constexpr BgraColor kSampleBandBorder{0x78, 0x6E, 0x90, kOpaqueAlpha};

bool IsInsideRect(const RectI& r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.h + r.y;
}

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* name)
        : path_(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST(SpriteCanvas, TextPaintsStrictlyInsideSafeRectAndMatchesMeasureText) {
    SpriteCanvas canvas(kDefaultCanvasW, kDefaultCanvasH);
    canvas.Fill(RectI{0, 0, kDefaultCanvasW, kDefaultCanvasH}, kPlateBase);

    TextStyle style;
    style.size_px = kBodyFontPx;
    style.weight = kWeightSemibold;
    style.fill = kWhiteOpaque;
    style.stroke_px = kMediumStrokePx;
    style.stroke = kDarkStroke;
    style.shadow = true;

    const TextLayoutResult measured = canvas.MeasureText("System Settings", kSafeBox, style);
    const TextLayoutResult drawn = canvas.DrawText("System Settings", kSafeBox, style);

    EXPECT_TRUE(drawn.fits);
    EXPECT_EQ(drawn.line_count, 1);
    EXPECT_FLOAT_EQ(drawn.scale, kFullFactor);
    EXPECT_FLOAT_EQ(drawn.squeeze, kFullFactor);
    EXPECT_EQ(drawn.fits, measured.fits);
    EXPECT_FLOAT_EQ(drawn.scale, measured.scale);
    EXPECT_FLOAT_EQ(drawn.squeeze, measured.squeeze);
    EXPECT_EQ(drawn.line_count, measured.line_count);
    EXPECT_EQ(drawn.ink_bounds, measured.ink_bounds);
    EXPECT_GT(drawn.ink_bounds.w, 0);
    EXPECT_GT(drawn.ink_bounds.h, 0);
    EXPECT_GE(drawn.ink_bounds.x, kSafeBox.x);
    EXPECT_GE(drawn.ink_bounds.y, kSafeBox.y);
    EXPECT_LE(drawn.ink_bounds.x + drawn.ink_bounds.w, kSafeBox.x + kSafeBox.w);
    EXPECT_LE(drawn.ink_bounds.y + drawn.ink_bounds.h, kSafeBox.y + kSafeBox.h);

    int painted_inside = 0;
    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            const BgraColor p = canvas.PixelAt(x, y);
            if (!IsInsideRect(kSafeBox, x, y)) {
                EXPECT_EQ(p, kPlateBase);
                continue;
            }
            if (p != kPlateBase) {
                ++painted_inside;
                EXPECT_TRUE(IsInsideRect(drawn.ink_bounds, x, y));
            }
        }
    }
    EXPECT_GT(painted_inside, 0);
}

TEST(SpriteCanvas, FitLoopSqueezesThenShrinksAndReportsUnfitAtLimits) {
    SpriteCanvas canvas(kDefaultCanvasW, kDefaultCanvasH);
    TextStyle style;
    style.size_px = kBodyFontPx;
    style.min_squeeze = kMinSqueezeLimit;
    style.min_scale = kMinScaleLimit;

    const TextLayoutResult squeezed =
        canvas.DrawText("Window Display Mode", kNarrowSafeBox, style);
    EXPECT_TRUE(squeezed.fits);
    EXPECT_LT(squeezed.squeeze, kFullFactor);
    EXPECT_GE(squeezed.squeeze, kMinSqueezeLimit);

    const TextLayoutResult shrunk =
        canvas.DrawText("Auto-Read Page Speed", kNarrowSafeBox, style);
    EXPECT_TRUE(shrunk.fits);
    EXPECT_FLOAT_EQ(shrunk.squeeze, kMinSqueezeLimit);
    EXPECT_LT(shrunk.scale, kFullFactor);
    EXPECT_GE(shrunk.scale, kMinScaleLimit);

    const TextLayoutResult overflow = canvas.DrawText(
        "An Extraordinarily Long Unbreakable Configuration Header Line", kTinySafeBox, style);
    EXPECT_FALSE(overflow.fits);
    EXPECT_FLOAT_EQ(overflow.squeeze, kMinSqueezeLimit);
    EXPECT_FLOAT_EQ(overflow.scale, kMinScaleLimit);
    EXPECT_GT(overflow.ink_bounds.w, 0);
    EXPECT_GE(overflow.ink_bounds.x, kTinySafeBox.x);
    EXPECT_LE(overflow.ink_bounds.x + overflow.ink_bounds.w, kTinySafeBox.x + kTinySafeBox.w);
}

TEST(SpriteCanvas, PreservesAlphaOutsideInkAndOnlyAddsCoverageInsideInk) {
    SpriteCanvas canvas(kDefaultCanvasW, kDefaultCanvasH);
    canvas.Fill(RectI{0, 0, kDefaultCanvasW, kDefaultCanvasH}, kSemiTransparentBase);

    TextStyle style;
    style.size_px = kBodyFontPx;
    style.fill = kWhiteOpaque;
    style.stroke_px = kMediumStrokePx;
    style.stroke = kDarkStroke;

    const TextLayoutResult res = canvas.DrawText("Save Data", kSafeBox, style);
    ASSERT_TRUE(res.fits);

    int elevated_alpha_count = 0;
    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            const BgraColor p = canvas.PixelAt(x, y);
            if (!IsInsideRect(res.ink_bounds, x, y)) {
                EXPECT_EQ(p, kSemiTransparentBase);
                continue;
            }
            EXPECT_GE(p.a, kSemiTransparentBase.a);
            if (p.a > kSemiTransparentBase.a) {
                ++elevated_alpha_count;
            }
        }
    }
    EXPECT_GT(elevated_alpha_count, 0);
}

TEST(SpriteCanvas, EraseWithRowMedianRemovesSyntheticGlyphOnFlatAndVerticalGradientPlate) {
    SpriteCanvas canvas(kDefaultCanvasW, kDefaultCanvasH);
    for (int y = 0; y < canvas.height(); ++y) {
        const auto row_tone = static_cast<std::uint8_t>(kLinearGradientBaseG + y);
        canvas.Fill(RectI{0, y, kDefaultCanvasW, 1},
                    BgraColor{row_tone, row_tone, row_tone, kOpaqueAlpha});
    }
    const SpriteCanvas clean_reference = canvas;

    canvas.Fill(kGlyphStamp, kSyntheticGlyphInk);
    ASSERT_EQ(canvas.PixelAt(kGlyphStamp.x, kGlyphStamp.y), kSyntheticGlyphInk);

    canvas.EraseWithRowMedian(kEraseHole, kRowMedianMarginPx);

    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            EXPECT_EQ(canvas.PixelAt(x, y), clean_reference.PixelAt(x, y));
        }
    }
}

TEST(SpriteCanvas, BorderAndMaskedInpaintReproduceLinearGradientWithinSmallTolerance) {
    SpriteCanvas canvas(kGradientCanvasW, kGradientCanvasH);
    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            const auto r = static_cast<std::uint8_t>(kLinearGradientBaseR +
                                                     x * kLinearGradientStepX +
                                                     y * kLinearGradientStepY);
            const auto g = static_cast<std::uint8_t>(kLinearGradientBaseG +
                                                     x * kLinearGradientStepX);
            const auto b = static_cast<std::uint8_t>(kLinearGradientBaseB -
                                                     y * kLinearGradientStepY);
            canvas.Fill(RectI{x, y, 1, 1}, BgraColor{b, g, r, kOpaqueAlpha});
        }
    }
    const SpriteCanvas expected = canvas;

    canvas.Fill(kEraseHole, kSyntheticGlyphInk);
    canvas.EraseWithBorderInpaint(kEraseHole);

    for (int y = kEraseHole.y; y < kEraseHole.y + kEraseHole.h; ++y) {
        for (int x = kEraseHole.x; x < kEraseHole.x + kEraseHole.w; ++x) {
            const BgraColor actual = canvas.PixelAt(x, y);
            const BgraColor target = expected.PixelAt(x, y);
            EXPECT_LE(std::abs(static_cast<int>(actual.r) - static_cast<int>(target.r)),
                      kMaxGradientInpaintError);
            EXPECT_LE(std::abs(static_cast<int>(actual.g) - static_cast<int>(target.g)),
                      kMaxGradientInpaintError);
            EXPECT_LE(std::abs(static_cast<int>(actual.b) - static_cast<int>(target.b)),
                      kMaxGradientInpaintError);
            EXPECT_EQ(actual.a, kOpaqueAlpha);
        }
    }

    std::vector<std::uint8_t> mask(
        static_cast<std::size_t>(canvas.width()) * static_cast<std::size_t>(canvas.height()), 0);
    for (int y = kGlyphStamp.y; y < kGlyphStamp.y + kGlyphStamp.h; ++y) {
        for (int x = kGlyphStamp.x; x < kGlyphStamp.x + kGlyphStamp.w; ++x) {
            canvas.Fill(RectI{x, y, 1, 1}, kSyntheticGlyphInk);
            mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas.width()) +
                 static_cast<std::size_t>(x)] = 1;
        }
    }
    canvas.EraseMaskedWithInpaint(mask);

    for (int y = kGlyphStamp.y; y < kGlyphStamp.y + kGlyphStamp.h; ++y) {
        for (int x = kGlyphStamp.x; x < kGlyphStamp.x + kGlyphStamp.w; ++x) {
            const BgraColor actual = canvas.PixelAt(x, y);
            const BgraColor target = expected.PixelAt(x, y);
            EXPECT_LE(std::abs(static_cast<int>(actual.r) - static_cast<int>(target.r)),
                      kMaxGradientInpaintError);
            EXPECT_LE(std::abs(static_cast<int>(actual.g) - static_cast<int>(target.g)),
                      kMaxGradientInpaintError);
            EXPECT_LE(std::abs(static_cast<int>(actual.b) - static_cast<int>(target.b)),
                      kMaxGradientInpaintError);
        }
    }
}

TEST(SpriteCanvas, RoundedPillCoverageIsSymmetricHorizontallyAndVertically) {
    SpriteCanvas canvas(kDefaultCanvasW, kDefaultCanvasH);
    canvas.DrawRoundedPill(kPillBox, kPillRadiusPx, kPillFill, kPillBorderPx, kPillBorder);

    const BgraColor center =
        canvas.PixelAt(kPillBox.x + kPillBox.w / 2, kPillBox.y + kPillBox.h / 2);
    EXPECT_EQ(center, kPillFill);
    EXPECT_EQ(canvas.PixelAt(0, 0).a, 0);

    for (int iy = 0; iy < kPillBox.h; ++iy) {
        for (int ix = 0; ix < kPillBox.w; ++ix) {
            const BgraColor p = canvas.PixelAt(kPillBox.x + ix, kPillBox.y + iy);
            const BgraColor mirror_x =
                canvas.PixelAt(kPillBox.x + (kPillBox.w - 1 - ix), kPillBox.y + iy);
            const BgraColor mirror_y =
                canvas.PixelAt(kPillBox.x + ix, kPillBox.y + (kPillBox.h - 1 - iy));
            EXPECT_EQ(p, mirror_x);
            EXPECT_EQ(p, mirror_y);
        }
    }
}

TEST(SpriteCanvas, BlitAndCompositeClampOutOfBoundsRegionsCleanly) {
    SpriteCanvas canvas(kSmallCanvasW, kSmallCanvasH);
    canvas.Fill(RectI{0, 0, kSmallCanvasW, kSmallCanvasH}, kPlateBase);

    const ama::imaging::BgraImage donor =
        ama::imaging::MakeFilledBgraImage(kDonorW, kDonorH, kPillFill);
    canvas.BlitCleanRegion(donor, RectI{kNegativeClipOffset, kNegativeClipOffset, kDonorW, kDonorH},
                           kNegativeClipOffset, kNegativeClipOffset);

    EXPECT_EQ(canvas.PixelAt(0, 0), kPillFill);
    EXPECT_EQ(canvas.PixelAt(kSmallCanvasW - 1, kSmallCanvasH - 1), kPlateBase);

    canvas.CompositeSubRect(donor, RectI{0, 0, kDonorW, kDonorH}, kNearRightEdgeX,
                            kNearBottomEdgeY);
    const BgraColor blended = canvas.PixelAt(kNearRightEdgeX, kNearBottomEdgeY);
    EXPECT_NE(blended, kPlateBase);
    EXPECT_EQ(blended.a, kOpaqueAlpha);
}

TEST(SpriteCanvas, CopyMoveWrapAndPremultipliedAlphaGradientInpaint) {
    SpriteCanvas canvas(kGradientCanvasW, kGradientCanvasH);
    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            const auto alpha =
                static_cast<std::uint8_t>(std::min<int>(kOpaqueAlpha, x * kSemiAlphaRampStep));
            canvas.Fill(RectI{x, y, 1, 1}, BgraColor{0x20, 0x40, 0x60, alpha});
        }
    }
    const SpriteCanvas reference_copy = canvas;
    SpriteCanvas moved_canvas = std::move(canvas);
    EXPECT_EQ(moved_canvas.width(), kGradientCanvasW);
    EXPECT_EQ(moved_canvas.height(), kGradientCanvasH);

    moved_canvas.Fill(kGlyphStamp, kSyntheticGlyphInk);
    moved_canvas.EraseWithBorderInpaint(kGlyphStamp);
    for (int y = kGlyphStamp.y; y < kGlyphStamp.y + kGlyphStamp.h; ++y) {
        for (int x = kGlyphStamp.x; x < kGlyphStamp.x + kGlyphStamp.w; ++x) {
            const BgraColor actual = moved_canvas.PixelAt(x, y);
            const BgraColor expected = reference_copy.PixelAt(x, y);
            EXPECT_LE(std::abs(static_cast<int>(actual.a) - static_cast<int>(expected.a)),
                      kMaxAlphaInpaintError);
        }
    }

    SpriteCanvas wrap_canvas(kDefaultCanvasW, kDefaultCanvasH);
    TextStyle wrap_style;
    wrap_style.size_px = kBodyFontPx;
    wrap_style.wrap = true;
    const TextLayoutResult wrapped =
        wrap_canvas.DrawText("Return to the Title Screen?", kNarrowSafeBox, wrap_style);
    EXPECT_TRUE(wrapped.fits);
    EXPECT_GE(wrapped.line_count, kExpectedWrappedLines);
}

TEST(SpriteCanvas, PngSaveLoadRoundTripAndSampleShowcaseRender) {
    SpriteCanvas showcase(kSampleSheetW, kSampleSheetH);
    showcase.Fill(RectI{0, 0, kSampleSheetW, kSampleSheetH}, kSampleBackdrop);

    struct SampleRow {
        const char* text;
        std::wstring font;
        int weight;
        bool italic;
        bool gradient;
        float stroke_px;
        bool shadow;
        float tracking_px;
        RectI safe_override;
    };

    const std::vector<SampleRow> rows{
        {"New Game  /  Continue  /  System Config", L"Segoe UI", kWeightBold, false, true,
         kHeavyStrokePx, true, kTrackingSubtlePx, RectI{}},
        {"Voice & Sound Environment Settings", L"Segoe UI Semibold", kWeightSemibold, false, false,
         kMediumStrokePx, true, 0.0F, RectI{}},
        {"Shirasagi Station  —  Nagare Chayagai  —  Renchien", L"Georgia", kWeightBold, true, true,
         kMediumStrokePx, true, 0.0F, RectI{}},
        {"Horizontal Squeeze Mode: Compresses cleanly into a narrow configuration plate",
         L"Calibri", kWeightSemibold, false, false, kLightStrokePx, true, 0.0F,
         RectI{kSampleBandX + kSampleInsetX, 0, kSampleSqueezedW,
               kSampleBandH - 2 * kSampleInsetY}},
        {"Dear Kenichi,\nThank you for walking home with me under the snow.", L"Gabriola",
         kWeightRegular, true, false, kLightStrokePx, true, kTrackingMediumPx, RectI{}},
    };

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const int band_y = kSampleBandStartY + static_cast<int>(i) * kSampleBandStepY;
        const RectI band{kSampleBandX, band_y, kSampleBandW, kSampleBandH};
        showcase.DrawRoundedPill(band, kSamplePillRadius, kSampleBandFill, kSamplePillBorder,
                                 kSampleBandBorder);

        TextStyle st;
        st.font_family = rows[i].font;
        st.weight = rows[i].weight;
        st.italic = rows[i].italic;
        st.size_px = (i == kHandwrittenRowIndex) ? kTitleFontPx : kBodyFontPx;
        st.fill = rows[i].gradient ? kGoldTop : kWhiteOpaque;
        st.gradient = rows[i].gradient;
        st.fill_bottom = kGoldBottom;
        st.stroke_px = rows[i].stroke_px;
        st.stroke = kDarkStroke;
        st.shadow = rows[i].shadow;
        st.shadow_color = kSoftShadow;
        st.tracking_px = rows[i].tracking_px;
        st.wrap = (i == kHandwrittenRowIndex);

        RectI safe{band.x + kSampleInsetX, band.y + kSampleInsetY,
                   band.w - 2 * kSampleInsetX, band.h - 2 * kSampleInsetY};
        if (rows[i].safe_override.w > 0) {
            safe = RectI{rows[i].safe_override.x, band.y + kSampleInsetY, rows[i].safe_override.w,
                         rows[i].safe_override.h};
        }
        const TextLayoutResult res = showcase.DrawText(rows[i].text, safe, st);
        EXPECT_TRUE(res.fits);
        EXPECT_GT(res.ink_bounds.w, 0);
    }

    const ScopedTempDir temp_dir("ama_ut_sprite_canvas");
    const std::filesystem::path sample_path = temp_dir.path() / "sample_text.png";
    showcase.SavePng(sample_path);
    ASSERT_TRUE(std::filesystem::exists(sample_path));

    const SpriteCanvas reloaded = SpriteCanvas::LoadPng(sample_path);
    EXPECT_EQ(reloaded.width(), showcase.width());
    EXPECT_EQ(reloaded.height(), showcase.height());
    EXPECT_EQ(reloaded.image().pixels, showcase.image().pixels);

    const std::filesystem::path sheet_dir = temp_dir.path() / "sheet_test";
    const std::vector<std::filesystem::path> pages = SaveContactSheetGrid(
        {ContactSheetCell{"sample_text", showcase.image()}}, sheet_dir, "canvas_sheet",
        ContactSheetLayout{});
    ASSERT_EQ(pages.size(), 1U);
    EXPECT_TRUE(std::filesystem::exists(pages.front()));
}
