// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/core/sprite_canvas.h"
#include "imaging/contact_sheet.h"
#include "imaging/wic_image_io.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINGDI_) || \
    defined(__wincodec_h__)
#error "Windows/GDI/WIC header leaked into sprite_canvas.h, contact_sheet.h, or wic_image_io.h"
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using adi::imaging::BgraColor;
using adi::imaging::BgraImage;
using adi::imaging::RectI;

constexpr int kGradientPlateW = 64;
constexpr int kGradientPlateH = 24;
constexpr int kInpaintPlateW = 32;
constexpr int kInpaintPlateH = 32;
constexpr std::uint8_t kSemiTranslucentAlpha = 160u;
constexpr int kMaxLaplaceInterpolationTolerance = 4;
constexpr int kNarrowButtonW = 92;
constexpr int kNarrowButtonH = 24;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0u};
        dir_ = fs::temp_directory_path() /
               fs::u8path("adi_ut_canvas_" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const { return dir_; }

private:
    fs::path dir_;
};

}  // namespace

TEST(SpriteCanvas, WipeRectRowMedianRestoresVerticalBevelGradientIgnoringInk) {
    BgraImage plate(kGradientPlateW, kGradientPlateH);
    for (int y = 0; y < kGradientPlateH; ++y) {
        const auto shade = static_cast<std::uint8_t>(40 + y * 6);
        for (int x = 0; x < kGradientPlateW; ++x) {
            plate.SetPixel(x, y, BgraColor{shade, static_cast<std::uint8_t>(shade + 15),
                                           static_cast<std::uint8_t>(shade + 30), 255u});
        }
    }

    // Paint bright contrasting glyph ink inside the interior box [16, 4) .. [48, 20)
    // plus one outlier speck in the left margin to prove median immunity over mean.
    const BgraImage clean_reference = plate;
    const RectI glyph_box{16, 4, 32, 16};
    plate.FillRect(glyph_box, BgraColor{250u, 250u, 250u, 255u});
    plate.SetPixel(14, 10, BgraColor{255u, 0u, 255u, 255u});

    adi::menu::SpriteCanvas canvas(plate);
    canvas.WipeRectRowMedian(glyph_box, 8);

    for (int y = glyph_box.y; y < glyph_box.y + glyph_box.h; ++y) {
        for (int x = glyph_box.x; x < glyph_box.x + glyph_box.w; ++x) {
            EXPECT_EQ(canvas.PixelAt(x, y), clean_reference.PixelAt(x, y))
                << "at (" << x << ", " << y << ")";
        }
    }
}

TEST(SpriteCanvas, InpaintMaskedLaplaceSorHealsGradientWhilePreservingSemiAlpha) {
    BgraImage glass(kInpaintPlateW, kInpaintPlateH);
    for (int y = 0; y < kInpaintPlateH; ++y) {
        for (int x = 0; x < kInpaintPlateW; ++x) {
            const auto blue = static_cast<std::uint8_t>(50 + x * 4);
            const auto green = static_cast<std::uint8_t>(30 + y * 4);
            glass.SetPixel(x, y, BgraColor{blue, green, 90u, kSemiTranslucentAlpha});
        }
    }
    const BgraImage expected = glass;

    const RectI region{8, 8, 16, 16};
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(region.w * region.h), 0u);
    for (int ly = 3; ly < 13; ++ly) {
        for (int lx = 3; lx < 13; ++lx) {
            mask[static_cast<std::size_t>(ly * region.w + lx)] = 1u;
            glass.SetPixel(region.x + lx, region.y + ly,
                           BgraColor{255u, 255u, 255u, 255u});
        }
    }

    adi::menu::SpriteCanvas canvas(glass);
    canvas.InpaintMaskedLaplaceSor(region, mask);

    // Unmasked pixels inside `region` must remain 100% untouched; masked pixels
    // must heal smoothly to the linear gradient and preserve `kSemiTranslucentAlpha`.
    EXPECT_EQ(canvas.PixelAt(region.x, region.y), expected.PixelAt(region.x, region.y));
    for (int ly = 3; ly < 13; ++ly) {
        for (int lx = 3; lx < 13; ++lx) {
            const BgraColor got = canvas.PixelAt(region.x + lx, region.y + ly);
            const BgraColor ref = expected.PixelAt(region.x + lx, region.y + ly);
            EXPECT_NEAR(static_cast<int>(got.b), static_cast<int>(ref.b),
                        kMaxLaplaceInterpolationTolerance);
            EXPECT_NEAR(static_cast<int>(got.g), static_cast<int>(ref.g),
                        kMaxLaplaceInterpolationTolerance);
            EXPECT_NEAR(static_cast<int>(got.r), static_cast<int>(ref.r),
                        kMaxLaplaceInterpolationTolerance);
            EXPECT_NEAR(static_cast<int>(got.a), static_cast<int>(kSemiTranslucentAlpha),
                        2);
        }
    }
}

TEST(SpriteCanvas, AutoFitFontSizeAndDrawTextFitNarrowButtonWithoutTouchingZeroAlphaExterior) {
    adi::menu::SpriteCanvas canvas(kNarrowButtonW, kNarrowButtonH);
    const RectI inner_plate{8, 3, kNarrowButtonW - 16, kNarrowButtonH - 6};
    canvas.WipeRectSolid(inner_plate, BgraColor{40u, 50u, 70u, 200u});

    adi::menu::TextStyle style;
    style.font_family = adi::menu::ResolveInstalledFontFamily();
    style.size_px = 20.0f;
    style.min_font_size_px = 8.0f;
    style.stroke_px = 1.0f;
    style.shadow = true;

    const std::string long_label = "Reset to Default";
    const int full_width = canvas.MeasureTextWidthAt1x(long_label, style);
    EXPECT_GT(full_width, inner_plate.w);

    const float fitted_px = canvas.AutoFitFontSize(long_label, inner_plate, style, 2);
    EXPECT_LT(fitted_px, style.size_px);
    EXPECT_GE(fitted_px, style.min_font_size_px);

    style.size_px = fitted_px;
    const auto metrics = canvas.DrawText(long_label, inner_plate, style);
    EXPECT_TRUE(metrics.fits);
    EXPECT_EQ(metrics.line_count, 1);
    EXPECT_FALSE(metrics.ink_bounds.empty());
    EXPECT_GE(metrics.ink_bounds.x, 0);
    EXPECT_LE(metrics.ink_bounds.x + metrics.ink_bounds.w, kNarrowButtonW);

    // Every pixel outside the button's drawn bounds (for example corners at (0,0)
    // and (w-1, h-1)) must retain exact straight-alpha 0 with RGB untouched.
    EXPECT_EQ(canvas.PixelAt(0, 0), (BgraColor{0u, 0u, 0u, 0u}));
    EXPECT_EQ(canvas.PixelAt(kNarrowButtonW - 1, kNarrowButtonH - 1),
              (BgraColor{0u, 0u, 0u, 0u}));
}

TEST(SpriteCanvas, CompositePremulOverStraightBlendsAccuratelyAndLeavesZeroAlphaUntouched) {
    BgraImage dst(4, 1);
    dst.SetPixel(0, 0, BgraColor{12u, 34u, 56u, 0u});      // transparent sentinel
    dst.SetPixel(1, 0, BgraColor{100u, 100u, 100u, 0u});   // transparent under ink
    dst.SetPixel(2, 0, BgraColor{0u, 0u, 200u, 255u});     // opaque red under 50% blue
    dst.SetPixel(3, 0, BgraColor{40u, 80u, 120u, 128u});   // untouched exterior pixel

    BgraImage src_premul(3, 1);
    src_premul.SetPixel(0, 0, BgraColor{0u, 0u, 0u, 0u});       // src_a == 0 -> no-op
    src_premul.SetPixel(1, 0, BgraColor{64u, 128u, 128u, 128u}); // premul (128,255,255) @ a=128
    src_premul.SetPixel(2, 0, BgraColor{128u, 0u, 0u, 128u});    // premul pure blue @ a=128

    adi::menu::CompositePremulOverStraight(dst, src_premul, 0, 0);

    // Pixel 0: src_a == 0 must not alter even bizarre RGB inside a dst_a == 0 pixel.
    EXPECT_EQ(dst.PixelAt(0, 0), (BgraColor{12u, 34u, 56u, 0u}));

    // Pixel 1: dst_a == 0 under premul ink -> out_a == 128, unpremultiplied RGB ~ (128, 255, 255).
    const BgraColor p1 = dst.PixelAt(1, 0);
    EXPECT_EQ(p1.a, 128u);
    EXPECT_NEAR(static_cast<int>(p1.b), 128, 2);
    EXPECT_NEAR(static_cast<int>(p1.g), 255, 1);
    EXPECT_NEAR(static_cast<int>(p1.r), 255, 1);

    // Pixel 2: 50% blue over opaque red -> out_a == 255, out_b ~ 128, out_r ~ 100.
    const BgraColor p2 = dst.PixelAt(2, 0);
    EXPECT_EQ(p2.a, 255u);
    EXPECT_NEAR(static_cast<int>(p2.b), 128, 2);
    EXPECT_EQ(p2.g, 0u);
    EXPECT_NEAR(static_cast<int>(p2.r), 100, 2);

    // Pixel 3: outside src_premul bounds -> byte-identical.
    EXPECT_EQ(dst.PixelAt(3, 0), (BgraColor{40u, 80u, 120u, 128u}));
}

TEST(SpriteCanvas, WicImageIoRoundTripsStraightAlphaPngInScopedTempDir) {
    BgraImage original(8, 6);
    for (int y = 0; y < original.height(); ++y) {
        for (int x = 0; x < original.width(); ++x) {
            original.SetPixel(
                x, y,
                BgraColor{
                    static_cast<std::uint8_t>(x * 29),
                    static_cast<std::uint8_t>(y * 41),
                    static_cast<std::uint8_t>((x + y) * 19),
                    static_cast<std::uint8_t>((x == 0 && y == 0) ? 0u : (40u + x * 25u)),
                });
        }
    }
    // Zero-alpha pixel with zero RGB must round-trip losslessly through WIC 32bppBGRA.
    original.SetPixel(0, 0, BgraColor{0u, 0u, 0u, 0u});

    ScopedTempDir scratch;
    const fs::path png_path = scratch.path() / "roundtrip_alpha.png";

    adi::imaging::WicImageIo io;
    io.SavePngFile(png_path, original);
    ASSERT_TRUE(fs::is_regular_file(png_path));

    const BgraImage reloaded = io.LoadPngFile(png_path);
    ASSERT_EQ(reloaded.width(), original.width());
    ASSERT_EQ(reloaded.height(), original.height());
    const auto diff = adi::imaging::ComputePixelDiff(original, reloaded);
    EXPECT_TRUE(diff.identical()) << "differing_pixels=" << diff.differing_pixels
                                  << " max_diff=" << static_cast<int>(diff.max_channel_abs_diff);
}

TEST(SpriteCanvas, ContactSheetBuilderPaginatesComparisonsAndSavesPngPages) {
    adi::imaging::ComparisonSheetOptions opts;
    opts.max_panel_px = 64;
    opts.rows_per_page = 2;
    opts.diff_amplification = 4;
    opts.checker_cell_px = 4;

    adi::imaging::ContactSheetBuilder builder(opts);
    EXPECT_TRUE(builder.empty());
    EXPECT_EQ(builder.row_count(), 0u);
    EXPECT_TRUE(builder.RenderPages().empty());

    BgraImage orig_chip(16, 12, BgraColor{30u, 60u, 90u, 220u});
    BgraImage patched_chip = orig_chip;
    patched_chip.FillRect(RectI{4, 3, 8, 6}, BgraColor{240u, 245u, 250u, 255u});

    BgraImage alpha_orig(16, 12, BgraColor{80u, 40u, 20u, 80u});
    BgraImage alpha_patched(16, 12, BgraColor{80u, 40u, 20u, 200u});

    builder.AddComparison("ME_02 cut[000]", orig_chip, orig_chip);
    builder.AddComparison("ME_02 cut[001]", orig_chip, patched_chip);
    builder.AddComparison("ME_02 cut[002]", alpha_orig, alpha_patched);

    EXPECT_FALSE(builder.empty());
    EXPECT_EQ(builder.row_count(), 3u);

    const std::vector<BgraImage> pages = builder.RenderPages();
    ASSERT_EQ(pages.size(), 2u);
    EXPECT_GT(pages[0].width(), 0);
    EXPECT_EQ(pages[0].width(), pages[1].width());
    EXPECT_GT(pages[0].height(), pages[1].height());

    // Identical comparison vs. modified comparison must produce distinct rendered
    // diff strips on equal-geometry single-row sheets.
    adi::imaging::ContactSheetBuilder identical_only(opts);
    identical_only.AddComparison("ME_02 cut[001]", orig_chip, orig_chip);
    adi::imaging::ContactSheetBuilder modified_only(opts);
    modified_only.AddComparison("ME_02 cut[001]", orig_chip, patched_chip);
    const std::vector<BgraImage> id_pages = identical_only.RenderPages();
    const std::vector<BgraImage> mod_pages = modified_only.RenderPages();
    ASSERT_EQ(id_pages.size(), 1u);
    ASSERT_EQ(mod_pages.size(), 1u);
    EXPECT_FALSE(adi::imaging::ComputePixelDiff(id_pages[0], mod_pages[0]).identical());

    ScopedTempDir scratch;
    const std::vector<fs::path> saved_pages =
        builder.SavePages(scratch.path(), "adieu_qa");
    ASSERT_EQ(saved_pages.size(), 2u);

    adi::imaging::WicImageIo io;
    for (std::size_t i = 0; i < saved_pages.size(); ++i) {
        ASSERT_TRUE(fs::is_regular_file(saved_pages[i]));
        const BgraImage loaded = io.LoadPngFile(saved_pages[i]);
        EXPECT_TRUE(adi::imaging::ComputePixelDiff(pages[i], loaded).identical());
    }

    adi::imaging::ContactSheetGridLayout grid_layout;
    grid_layout.columns = 2;
    grid_layout.max_cell_px = 48;
    grid_layout.cells_per_page = 2;
    const std::vector<adi::imaging::ContactSheetCell> cells = {
        {"cut[000]", orig_chip},
        {"cut[001]", patched_chip},
        {"cut[002]", alpha_patched},
    };
    const std::vector<fs::path> grid_pages =
        adi::imaging::SaveContactSheetGridPages(cells, scratch.path(), "grid", grid_layout);
    ASSERT_EQ(grid_pages.size(), 2u);
    EXPECT_TRUE(fs::is_regular_file(grid_pages[0]));
    EXPECT_TRUE(fs::is_regular_file(grid_pages[1]));
}
