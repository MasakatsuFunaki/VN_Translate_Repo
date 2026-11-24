// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-room unit tests for `snu::menu::BgraImage`, Porter-Duff compositors,
// and PIMPL 3x-supersampled `snu::menu::SpriteCanvas`.
#include "menu/bgra_image.h"
#include "menu/sprite_canvas.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace menu = snu::menu;
using menu::BgraColor;
using menu::BgraImage;
using menu::HorizontalAlign;
using menu::RectI;
using menu::SpriteCanvas;
using menu::TextLayoutMetrics;
using menu::TextStyle;
using menu::VerticalAlign;
using snu::Bytes;

namespace {

constexpr std::size_t kBmpHeaderBytes = 54;
constexpr std::size_t kBmpFileSizeOffset = 2;
constexpr std::size_t kBmpPixelArrayOffset = 10;
constexpr std::size_t kOddWidth24BmpStride = 12;
constexpr std::size_t kExpected3x2Bmp24Bytes = kBmpHeaderBytes + 2 * kOddWidth24BmpStride;
constexpr std::size_t kExpected2x2Bmp32Bytes = kBmpHeaderBytes + 2 * 2 * 4;
constexpr std::size_t kPngMagicBytes = 8;
constexpr std::uint8_t kPngSignature[kPngMagicBytes] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
};
constexpr int kInpaintToleranceLsb = 2;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view utf8_name)
        : path_(fs::temp_directory_path() / fs::u8path(utf8_name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::uint32_t ReadLe32(const Bytes& b, std::size_t off) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + off, sizeof(v));
    return v;
}

}  // namespace

TEST(BgraImage, bounds_checking_and_clamped_crop) {
    BgraImage img(4, 3, BgraColor{10, 20, 30, 40});
    img.SetPixel(1, 1, BgraColor{0xAA, 0xBB, 0xCC, 0xDD});
    img.SetPixel(3, 2, BgraColor{0x00, 0x7F, 0x80, 0xFF});

    EXPECT_THROW((void)img.PixelAt(-1, 0), std::out_of_range);
    EXPECT_THROW((void)img.PixelAt(4, 0), std::out_of_range);
    EXPECT_THROW((void)img.PixelAt(0, 3), std::out_of_range);
    EXPECT_THROW(img.SetPixel(0, -1, BgraColor{}), std::out_of_range);

    // Overhanging crop [-1, 1, 3, 5] clamps to [0, 1, 2, 2].
    const BgraImage cropped = img.Crop(RectI{-1, 1, 3, 5});
    ASSERT_EQ(cropped.width(), 2);
    ASSERT_EQ(cropped.height(), 2);
    EXPECT_EQ(cropped.PixelAt(0, 0), (BgraColor{10, 20, 30, 40}));
    EXPECT_EQ(cropped.PixelAt(1, 0), (BgraColor{0xAA, 0xBB, 0xCC, 0xDD}));

    // Disjoint crop yields an empty image.
    const BgraImage disjoint = img.Crop(RectI{10, 10, 2, 2});
    EXPECT_TRUE(disjoint.empty());
}

TEST(BgraImage, bmp_24_and_32_preserve_stride_padding_and_straight_alpha) {
    // 3x2 24-bpp BMP: 9 BGR bytes per row + 3 pad bytes = 12-byte stride,
    // bottom-up scanline order (row y=1 stored first at byte 54).
    BgraImage img24(3, 2, BgraColor{0x11, 0x22, 0x33, 0xFF});
    img24.SetPixel(0, 0, BgraColor{0x01, 0x02, 0x03, 0xFF});
    img24.SetPixel(0, 1, BgraColor{0x7F, 0x80, 0xFF, 0xFF});

    const Bytes bmp24 = img24.EncodeBmp(menu::kBmpBpp24);
    ASSERT_EQ(bmp24.size(), kExpected3x2Bmp24Bytes);
    EXPECT_EQ(bmp24[0], 'B');
    EXPECT_EQ(bmp24[1], 'M');
    EXPECT_EQ(ReadLe32(bmp24, kBmpFileSizeOffset),
              static_cast<std::uint32_t>(kExpected3x2Bmp24Bytes));
    EXPECT_EQ(ReadLe32(bmp24, kBmpPixelArrayOffset),
              static_cast<std::uint32_t>(kBmpHeaderBytes));
    // Bottom scanline (y = 1, x = 0) appears first at offset 54; pad bytes are 0.
    EXPECT_EQ(bmp24[kBmpHeaderBytes + 0], 0x7Fu);
    EXPECT_EQ(bmp24[kBmpHeaderBytes + 1], 0x80u);
    EXPECT_EQ(bmp24[kBmpHeaderBytes + 2], 0xFFu);
    EXPECT_EQ(bmp24[kBmpHeaderBytes + 9], 0x00u);
    EXPECT_EQ(bmp24[kBmpHeaderBytes + 11], 0x00u);
    EXPECT_EQ(BgraImage::DecodeBmpOrPng(bmp24), img24);

    // 2x2 32-bpp BMP: preserves exact straight-BGRA alpha {0x00, 0x7F, 0x80, 0xFF}
    // and non-zero RGB on alpha == 0 pixels.
    BgraImage img32(2, 2);
    img32.SetPixel(0, 0, BgraColor{0xAA, 0xBB, 0xCC, 0x00});
    img32.SetPixel(1, 0, BgraColor{0x12, 0x34, 0x56, 0x7F});
    img32.SetPixel(0, 1, BgraColor{0x78, 0x9A, 0xBC, 0x80});
    img32.SetPixel(1, 1, BgraColor{0xDE, 0xF0, 0x01, 0xFF});

    const Bytes bmp32 = img32.EncodeBmp(menu::kBmpBpp32);
    ASSERT_EQ(bmp32.size(), kExpected2x2Bmp32Bytes);
    EXPECT_EQ(BgraImage::DecodeBmpOrPng(bmp32), img32);
}

TEST(BgraImage, png_encode_save_and_decode_preserve_bgra_channel_order) {
    BgraImage img(2, 1);
    img.SetPixel(0, 0, BgraColor{0x10, 0x7F, 0xF0, 0xFF});
    img.SetPixel(1, 0, BgraColor{0x20, 0x40, 0x80, 0x80});

    const Bytes png = img.EncodePng();
    ASSERT_GT(png.size(), kPngMagicBytes);
    EXPECT_EQ(std::memcmp(png.data(), kPngSignature, kPngMagicBytes), 0);
    EXPECT_EQ(BgraImage::DecodeBmpOrPng(png), img);

    const ScopedTempDir scratch("snu_ut_bgra_png");
    const fs::path file_path = scratch.path() / "sub" / "test.png";
    img.SavePng(file_path);
    const Bytes on_disk = snu::read_file(file_path.u8string());
    EXPECT_EQ(BgraImage::DecodeBmpOrPng(on_disk), img);

    const Bytes corrupt = {'N', 'O', 'T', '_', 'I', 'M', 'G'};
    EXPECT_THROW((void)BgraImage::DecodeBmpOrPng(corrupt), std::runtime_error);
}

TEST(SpriteCanvas, composite_straight_over_straight_preserves_zero_alpha_exterior) {
    // Sentinel destination with non-zero RGB on alpha==0 plus an opaque blue pixel.
    BgraImage dst(3, 2, BgraColor{0x11, 0x22, 0x33, 0x00});
    dst.SetPixel(1, 0, BgraColor{200, 0, 0, 255});
    dst.SetPixel(2, 0, BgraColor{40, 50, 60, 255});

    BgraImage src(3, 1);
    // Pixel 0: alpha == 0 with non-zero RGB -> MUST NOT touch dst(0, 0)!
    src.SetPixel(0, 0, BgraColor{0xFF, 0xFF, 0xFF, 0x00});
    // Pixel 1: straight red {B=0, G=0, R=200, A=128} over opaque blue {B=200, G=0, R=0, A=255}
    // -> Porter-Duff over: B = round(200 * 127 / 255) = 100, R = round(200 * 128 / 255) = 100, A = 255.
    src.SetPixel(1, 0, BgraColor{0, 0, 200, 128});
    // Pixel 2: opaque pixel replaces dst verbatim.
    src.SetPixel(2, 0, BgraColor{7, 8, 9, 255});

    menu::CompositeStraightOverStraight(dst, src, 0, 0);

    EXPECT_EQ(dst.PixelAt(0, 0), (BgraColor{0x11, 0x22, 0x33, 0x00}));
    EXPECT_EQ(dst.PixelAt(1, 0), (BgraColor{100, 0, 100, 255}));
    EXPECT_EQ(dst.PixelAt(2, 0), (BgraColor{7, 8, 9, 255}));
    // Untouched bottom row remains byte-identical.
    EXPECT_EQ(dst.PixelAt(0, 1), (BgraColor{0x11, 0x22, 0x33, 0x00}));
}

TEST(SpriteCanvas, wipe_rect_row_median_restores_per_row_gradient_despite_outlier) {
    // 10x3 canvas where row y has clean border color {10*(y+1), 20*(y+1), 30*(y+1), 223}.
    BgraImage base(10, 3);
    for (int y = 0; y < 3; ++y) {
        const BgraColor clean{
            static_cast<std::uint8_t>(10 * (y + 1)),
            static_cast<std::uint8_t>(20 * (y + 1)),
            static_cast<std::uint8_t>(30 * (y + 1)),
            223,
        };
        for (int x = 0; x < 10; ++x) {
            base.SetPixel(x, y, clean);
        }
        // Single bright outlier inside the left border margin (1 of 6 border samples).
        base.SetPixel(2, y, BgraColor{255, 255, 255, 255});
        // Simulated Japanese ink inside target rect [3, 0, 4, 3].
        for (int x = 3; x < 7; ++x) {
            base.SetPixel(x, y, BgraColor{0, 0, 0, 255});
        }
    }

    SpriteCanvas canvas(base);
    canvas.WipeRectRowMedian(RectI{3, 0, 4, 3}, /*sample_margin_px=*/3);

    for (int y = 0; y < 3; ++y) {
        const BgraColor expected{
            static_cast<std::uint8_t>(10 * (y + 1)),
            static_cast<std::uint8_t>(20 * (y + 1)),
            static_cast<std::uint8_t>(30 * (y + 1)),
            223,
        };
        for (int x = 3; x < 7; ++x) {
            EXPECT_EQ(canvas.PixelAt(x, y), expected);
        }
        // Outlier outside the wiped rect is preserved untouched.
        EXPECT_EQ(canvas.PixelAt(2, y), (BgraColor{255, 255, 255, 255}));
    }
}

TEST(SpriteCanvas, inpaint_masked_laplace_sor_preserves_unmasked_pixels_and_validates_mask) {
    const BgraColor plate{80, 120, 200, 223};
    const BgraColor feather{250, 252, 255, 255};
    SpriteCanvas canvas(BgraImage(12, 12, plate));
    // Unmasked feather pixel inside the region MUST stay untouched.
    BgraImage img = canvas.CopyImage();
    img.SetPixel(4, 4, feather);
    // Masked dark glyph block [5..7, 5..7].
    for (int y = 5; y <= 7; ++y) {
        for (int x = 5; x <= 7; ++x) {
            img.SetPixel(x, y, BgraColor{10, 10, 10, 255});
        }
    }
    canvas = SpriteCanvas(std::move(img));

    const RectI region{3, 3, 6, 6};
    // Invalid -> valid state seam: wrong mask length throws without mutating canvas.
    const std::vector<std::uint8_t> bad_mask(5, 1u);
    EXPECT_THROW(canvas.InpaintMaskedLaplaceSor(region, bad_mask), std::invalid_argument);
    EXPECT_EQ(canvas.PixelAt(6, 6), (BgraColor{10, 10, 10, 255}));

    std::vector<std::uint8_t> mask(static_cast<std::size_t>(region.w * region.h), 0u);
    for (int y = 5; y <= 7; ++y) {
        for (int x = 5; x <= 7; ++x) {
            mask[static_cast<std::size_t>((y - region.y) * region.w + (x - region.x))] = 1u;
        }
    }
    canvas.InpaintMaskedLaplaceSor(region, mask);

    // Unmasked feather pixel inside region and exterior pixels stay byte-identical.
    EXPECT_EQ(canvas.PixelAt(4, 4), feather);
    EXPECT_EQ(canvas.PixelAt(0, 0), plate);

    // Masked glyph center is smoothly restored to the surrounding translucent plate.
    const BgraColor center = canvas.PixelAt(6, 6);
    EXPECT_NEAR(center.b, plate.b, kInpaintToleranceLsb);
    EXPECT_NEAR(center.g, plate.g, kInpaintToleranceLsb);
    EXPECT_NEAR(center.r, plate.r, kInpaintToleranceLsb);
    EXPECT_NEAR(center.a, plate.a, kInpaintToleranceLsb);
}

TEST(SpriteCanvas, draw_text_and_autofit_confine_ink_to_safe_bounds) {
    const BgraColor exterior_sentinel{0x22, 0x44, 0x66, 0x00};
    SpriteCanvas canvas(BgraImage(180, 48, exterior_sentinel));
    const RectI safe{20, 6, 140, 36};

    TextStyle style;
    style.font_family = menu::ResolveInstalledFontFamily({L"Georgia", L"Segoe UI"});
    style.size_px = 20.0f;
    style.fill = BgraColor{18, 30, 210, 255};
    style.stroke_px = 1.5f;
    style.stroke_color = BgraColor{255, 255, 255, 240};

    const TextLayoutMetrics m = canvas.DrawText("Start Auto Mode", safe, style);
    EXPECT_TRUE(m.fits);
    EXPECT_FALSE(m.ink_bounds.empty());
    EXPECT_GE(m.ink_bounds.x, safe.x);
    EXPECT_GE(m.ink_bounds.y, safe.y);
    EXPECT_LE(m.ink_bounds.x + m.ink_bounds.w, safe.x + safe.w);
    EXPECT_LE(m.ink_bounds.y + m.ink_bounds.h, safe.y + safe.h);

    int painted_inside = 0;
    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            const bool inside =
                (x >= safe.x && x < safe.x + safe.w && y >= safe.y && y < safe.y + safe.h);
            if (!inside) {
                ASSERT_EQ(canvas.PixelAt(x, y), exterior_sentinel);
            } else if (canvas.PixelAt(x, y).a > 0u) {
                ++painted_inside;
            }
        }
    }
    EXPECT_GT(painted_inside, 20);

    // AutoFitFontSize reduces font size when a long title overflows a narrow box.
    TextStyle fit_style = style;
    fit_style.size_px = 24.0f;
    fit_style.min_font_size_px = 10.0f;
    const float short_sz = canvas.AutoFitFontSize("BGM", RectI{0, 0, 120, 36}, fit_style);
    const float long_sz = canvas.AutoFitFontSize(
        "Everyday Life Like Schumann's Symphony No. 1", RectI{0, 0, 160, 36}, fit_style);
    EXPECT_FLOAT_EQ(short_sz, 24.0f);
    EXPECT_LT(long_sz, short_sz);
    EXPECT_GE(long_sz, fit_style.min_font_size_px);

    TextStyle invalid_style = style;
    invalid_style.size_px = 0.0f;
    EXPECT_THROW((void)canvas.DrawText("Bad", safe, invalid_style), std::invalid_argument);
}

TEST(SpriteCanvas, draw_text_rotated_90_ccw_matches_exact_ccw_coordinate_mapping) {
    const BgraColor exterior_sentinel{0x09, 0x18, 0x27, 0x00};
    const RectI safe{10, 12, 40, 180};

    TextStyle style;
    style.font_family = menu::ResolveInstalledFontFamily({L"Segoe UI"});
    style.size_px = 18.0f;
    style.fill = BgraColor{36, 36, 40, 255};
    style.halign = HorizontalAlign::kLeft;
    style.valign = VerticalAlign::kTop;

    // Reference horizontal render on (safe.h, safe.w) transparent canvas.
    SpriteCanvas horiz_ref(safe.h, safe.w);
    const TextLayoutMetrics hm =
        horiz_ref.DrawText("Character Voices", RectI{0, 0, safe.h, safe.w}, style);
    ASSERT_TRUE(hm.fits);
    ASSERT_FALSE(hm.ink_bounds.empty());

    SpriteCanvas vert_canvas(BgraImage(64, 210, exterior_sentinel));
    const TextLayoutMetrics vm =
        vert_canvas.DrawTextRotated90CCW("Character Voices", safe, style);

    EXPECT_TRUE(vm.fits);
    EXPECT_FALSE(vm.ink_bounds.empty());
    EXPECT_GT(vm.ink_bounds.h, vm.ink_bounds.w);
    EXPECT_GE(vm.ink_bounds.x, safe.x);
    EXPECT_GE(vm.ink_bounds.y, safe.y);
    EXPECT_LE(vm.ink_bounds.x + vm.ink_bounds.w, safe.x + safe.w);
    EXPECT_LE(vm.ink_bounds.y + vm.ink_bounds.h, safe.y + safe.h);

    // Every exterior pixel keeps its exact sentinel bytes; every interior pixel
    // with alpha > 0 matches (tx, ty) -> (safe.x + ty, safe.y + safe.h - 1 - tx).
    for (int y = 0; y < vert_canvas.height(); ++y) {
        for (int x = 0; x < vert_canvas.width(); ++x) {
            const bool inside =
                (x >= safe.x && x < safe.x + safe.w && y >= safe.y && y < safe.y + safe.h);
            if (!inside) {
                ASSERT_EQ(vert_canvas.PixelAt(x, y), exterior_sentinel);
            }
        }
    }
    for (int ty = 0; ty < safe.w; ++ty) {
        for (int tx = 0; tx < safe.h; ++tx) {
            const BgraColor hp = horiz_ref.PixelAt(tx, ty);
            const BgraColor vp = vert_canvas.PixelAt(safe.x + ty, safe.y + safe.h - 1 - tx);
            if (hp.a == 0u) {
                ASSERT_EQ(vp, exterior_sentinel);
            } else {
                ASSERT_EQ(vp, hp);
            }
        }
    }
}
