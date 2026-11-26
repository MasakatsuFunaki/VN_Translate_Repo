// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-room unit tests for BGI-native CG encoding (`encode_bgi_archive_bg`,
// `probe_archive_bg_spec`), alpha preservation across `cg::Image` & `Font::draw`,
// and idempotent `.backup`-based archive repacking (`ArchiveRepacker`).
#include "arc_fixture.h"
#include "bgi/bgi_archive.h"
#include "bgi/bgi_sysgrp.h"
#include "cg/cg_repacker.h"
#include "cg/image.h"
#include "cg/text_render.h"
#include "common/util.h"
#include "menu/bgra_image.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace bgi = snu::bgi;
namespace cg = snu::cg;
namespace fs = std::filesystem;
namespace menu = snu::menu;
using snu::Bytes;
using snu::test_fixture::make_arc;

constexpr std::size_t kCbgHeaderProbeBytes = 0x16u;
constexpr std::size_t kCbgTruncatedBytes = 0x15u;
constexpr std::size_t kCbgWidthOffset = 0x10u;
constexpr std::size_t kCbgHeightOffset = 0x12u;
constexpr std::size_t kCbgBppOffset = 0x14u;
constexpr std::size_t kBmpBppOffset = 28u;
constexpr std::size_t kBmpTruncatedBytes = 30u;
constexpr std::uint8_t kBmpSigB = 'B';
constexpr std::uint8_t kBmpSigM = 'M';
constexpr int kBpp8 = 8;
constexpr int kBpp16 = 16;
constexpr int kBpp24 = 24;
constexpr int kBpp32 = 32;

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

Bytes make_cbg_header(std::uint16_t width, std::uint16_t height,
                      std::uint16_t bpp, std::size_t total_size) {
    Bytes blob(total_size, 0u);
    static constexpr char kMagic[] = "CompressedBG___";
    for (std::size_t i = 0; i < sizeof(kMagic) && i < blob.size(); ++i) {
        blob[i] = static_cast<std::uint8_t>(kMagic[i]);
    }
    if (blob.size() >= kCbgHeaderProbeBytes) {
        blob[kCbgWidthOffset] = static_cast<std::uint8_t>(width & 0xFFu);
        blob[kCbgWidthOffset + 1u] = static_cast<std::uint8_t>((width >> 8) & 0xFFu);
        blob[kCbgHeightOffset] = static_cast<std::uint8_t>(height & 0xFFu);
        blob[kCbgHeightOffset + 1u] = static_cast<std::uint8_t>((height >> 8) & 0xFFu);
        blob[kCbgBppOffset] = static_cast<std::uint8_t>(bpp & 0xFFu);
        blob[kCbgBppOffset + 1u] = static_cast<std::uint8_t>((bpp >> 8) & 0xFFu);
    }
    return blob;
}

Bytes make_bgrx_bitfields_bmp_1x1(std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    // 14B file header + 56B BITMAPV3INFOHEADER (BI_BITFIELDS, a_mask = 0) + 4B pixel.
    Bytes out;
    auto put16 = [&](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    };
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
        }
    };
    out.push_back(kBmpSigB);
    out.push_back(kBmpSigM);
    put32(74u);          // bfSize
    put16(0u);           // bfReserved1
    put16(0u);           // bfReserved2
    put32(70u);          // bfOffBits (14 + 56)
    put32(56u);          // biSize
    put32(1u);           // biWidth
    put32(1u);           // biHeight
    put16(1u);           // biPlanes
    put16(32u);          // biBitCount
    put32(3u);           // BI_BITFIELDS
    put32(4u);           // biSizeImage
    put32(2835u);        // biXPelsPerMeter
    put32(2835u);        // biYPelsPerMeter
    put32(0u);           // biClrUsed
    put32(0u);           // biClrImportant
    put32(0x00FF0000u);  // R mask
    put32(0x0000FF00u);  // G mask
    put32(0x000000FFu);  // B mask
    put32(0x00000000u);  // A mask = 0 (BGRX, no alpha)
    out.push_back(b);
    out.push_back(g);
    out.push_back(r);
    out.push_back(0x00u);
    return out;
}

bool starts_with_bm(const Bytes& blob) noexcept {
    return blob.size() >= 2u && blob[0] == kBmpSigB && blob[1] == kBmpSigM;
}

}  // namespace

TEST(CgRepacker, EncodeBgiArchiveBgPinned24BppAnd32BppVectors) {
    // Pinned 2x2 24-bpp RGB image exercising sign-bit bytes (0x00, 0x7F, 0x80, 0xFF).
    cg::Image img24;
    img24.width = 2;
    img24.height = 2;
    img24.rgb = {
        0x80, 0x7F, 0x00,  // (0,0) RGB -> BGR: 00 7F 80
        0xFE, 0x01, 0xFF,  // (1,0) RGB -> BGR: FF 01 FE
        0x56, 0x34, 0x12,  // (0,1) RGB -> BGR: 12 34 56
        0xDE, 0xBC, 0x9A,  // (1,1) RGB -> BGR: 9A BC DE
    };
    const std::array<std::uint8_t, bgi::kSysGrpReservedBytes> reserved24 = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
    };

    const Bytes encoded24 =
        cg::encode_bgi_archive_bg(img24, kBpp24, nullptr, reserved24);
    ASSERT_TRUE(bgi::is_dsc(encoded24));
    EXPECT_FALSE(starts_with_bm(encoded24));

    const Bytes expected_raw24 = {
        0x02, 0x00, 0x02, 0x00, 0x18, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
        0x00, 0x7F, 0x80,
        0xFF, 0x01, 0xFE,
        0x12, 0x34, 0x56,
        0x9A, 0xBC, 0xDE,
    };
    const Bytes decoded_raw24 = bgi::dsc_decompress(encoded24);
    EXPECT_EQ(decoded_raw24, expected_raw24);

    const bgi::SysGrpImage sys24 = bgi::decode_sysgrp(decoded_raw24);
    EXPECT_EQ(sys24.orig_bpp, kBpp24);
    EXPECT_EQ(sys24.reserved, reserved24);
    EXPECT_EQ(sys24.image.PixelAt(0, 0), (menu::BgraColor{0x00, 0x7F, 0x80, 0xFF}));
    EXPECT_EQ(sys24.image.PixelAt(1, 1), (menu::BgraColor{0x9A, 0xBC, 0xDE, 0xFF}));

    // Pinned 2x2 32-bpp straight-BGRA image preserving RGB under alpha == 0x00
    // and sign-bit alpha values (0x00, 0x7F, 0x80, 0xFF).
    cg::Image img32;
    img32.width = 2;
    img32.height = 2;
    img32.rgb = {
        0x99, 0x66, 0x33,  // (0,0) -> BGRA: 33 66 99 00
        0x7F, 0x80, 0xFE,  // (1,0) -> BGRA: FE 80 7F 80
        0x10, 0x20, 0x30,  // (0,1) -> BGRA: 30 20 10 7F
        0x00, 0xFF, 0x01,  // (1,1) -> BGRA: 01 FF 00 FF
    };
    img32.alpha = {0x00, 0x80, 0x7F, 0xFF};

    const Bytes encoded32 = cg::encode_bgi_archive_bg(img32, kBpp32);
    ASSERT_TRUE(bgi::is_dsc(encoded32));
    EXPECT_FALSE(starts_with_bm(encoded32));

    const Bytes expected_raw32 = {
        0x02, 0x00, 0x02, 0x00, 0x20, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x66, 0x99, 0x00,
        0xFE, 0x80, 0x7F, 0x80,
        0x30, 0x20, 0x10, 0x7F,
        0x01, 0xFF, 0x00, 0xFF,
    };
    const Bytes decoded_raw32 = bgi::dsc_decompress(encoded32);
    EXPECT_EQ(decoded_raw32, expected_raw32);

    const bgi::SysGrpImage sys32 = bgi::decode_sysgrp(decoded_raw32);
    EXPECT_EQ(sys32.orig_bpp, kBpp32);
    EXPECT_EQ(sys32.image.PixelAt(0, 0), (menu::BgraColor{0x33, 0x66, 0x99, 0x00}));
    EXPECT_EQ(sys32.image.PixelAt(1, 0), (menu::BgraColor{0xFE, 0x80, 0x7F, 0x80}));
    EXPECT_EQ(sys32.image.PixelAt(0, 1), (menu::BgraColor{0x30, 0x20, 0x10, 0x7F}));
    EXPECT_EQ(sys32.image.PixelAt(1, 1), (menu::BgraColor{0x01, 0xFF, 0x00, 0xFF}));
}

TEST(CgRepacker, AlphaPreservedThroughCodecsDonorOpaqueFallbackAndErrors) {
    // 1. from_cbg_pixels retains alpha for 32-bpp and omits it for 24-bpp.
    const Bytes cbg32_pixels = {
        0x11, 0x22, 0x33, 0x00,
        0x44, 0x55, 0x66, 0x80,
    };
    const cg::Image from32 = cg::from_cbg_pixels(cbg32_pixels, 2, 1, kBpp32);
    ASSERT_TRUE(from32.has_alpha());
    EXPECT_EQ(from32.rgb, (Bytes{0x33, 0x22, 0x11, 0x66, 0x55, 0x44}));
    EXPECT_EQ(from32.alpha, (Bytes{0x00, 0x80}));

    const Bytes cbg24_pixels = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    const cg::Image from24 = cg::from_cbg_pixels(cbg24_pixels, 2, 1, kBpp24);
    EXPECT_FALSE(from24.has_alpha());
    EXPECT_EQ(from24.rgb, (Bytes{0x33, 0x22, 0x11, 0x66, 0x55, 0x44}));

    // 2. encode_bmp -> decode_image preserves 32-bpp alpha verbatim, while
    //    BI_BITFIELDS BGRX (a_mask == 0) decodes without an alpha channel.
    const Bytes bmp32 = cg::encode_bmp(from32);
    ASSERT_GT(bmp32.size(), kBmpBppOffset + 1u);
    EXPECT_EQ(bmp32[kBmpBppOffset], static_cast<std::uint8_t>(kBpp32));
    const auto roundtrip32 = cg::decode_image(bmp32);
    ASSERT_TRUE(roundtrip32.has_value());
    ASSERT_TRUE(roundtrip32->has_alpha());
    EXPECT_EQ(roundtrip32->rgb, from32.rgb);
    EXPECT_EQ(roundtrip32->alpha, from32.alpha);

    const auto decoded_bgrx =
        cg::decode_image(make_bgrx_bitfields_bmp_1x1(0x12, 0x34, 0x56));
    ASSERT_TRUE(decoded_bgrx.has_value());
    EXPECT_FALSE(decoded_bgrx->has_alpha());
    EXPECT_EQ(decoded_bgrx->rgb, (Bytes{0x56, 0x34, 0x12}));

    // Empty crop / resize guards return an empty image without UB.
    EXPECT_TRUE(cg::crop(from32, 2, 0, 2, 1).empty());
    EXPECT_TRUE(cg::resize(from32, 0, 1).empty());
    EXPECT_TRUE(cg::resize(cg::Image{}, 2, 2).empty());

    // 3. 24-bpp patched image encoded at 32-bpp:
    //    - with 32-bpp donor: transplants donor alpha pixel-for-pixel;
    //    - without donor: fills opaque 0xFF alpha across all pixels.
    cg::Image legacy_patched24;
    legacy_patched24.width = 2;
    legacy_patched24.height = 1;
    legacy_patched24.rgb = {0xAA, 0xBB, 0xCC, 0x01, 0x7F, 0xFE};
    ASSERT_FALSE(legacy_patched24.has_alpha());

    const Bytes transplanted_dsc =
        cg::encode_bgi_archive_bg(legacy_patched24, kBpp32, &from32);
    ASSERT_TRUE(bgi::is_dsc(transplanted_dsc));
    const bgi::SysGrpImage transplanted =
        bgi::decode_sysgrp(bgi::dsc_decompress(transplanted_dsc));
    EXPECT_EQ(transplanted.orig_bpp, kBpp32);
    EXPECT_EQ(transplanted.image.PixelAt(0, 0),
              (menu::BgraColor{0xCC, 0xBB, 0xAA, 0x00}));
    EXPECT_EQ(transplanted.image.PixelAt(1, 0),
              (menu::BgraColor{0xFE, 0x7F, 0x01, 0x80}));

    const Bytes opaque32_dsc =
        cg::encode_bgi_archive_bg(legacy_patched24, kBpp32, nullptr);
    ASSERT_TRUE(bgi::is_dsc(opaque32_dsc));
    const bgi::SysGrpImage opaque32 =
        bgi::decode_sysgrp(bgi::dsc_decompress(opaque32_dsc));
    EXPECT_EQ(opaque32.orig_bpp, kBpp32);
    EXPECT_EQ(opaque32.image.PixelAt(0, 0),
              (menu::BgraColor{0xCC, 0xBB, 0xAA, 0xFF}));
    EXPECT_EQ(opaque32.image.PixelAt(1, 0),
              (menu::BgraColor{0xFE, 0x7F, 0x01, 0xFF}));

    // 4. Error boundaries: empty image, unsupported bpp, malformed rgb/alpha,
    //    and donor dimension/alpha mismatch.
    const cg::Image empty_img;
    EXPECT_THROW(cg::encode_bgi_archive_bg(empty_img, kBpp24), std::invalid_argument);
    EXPECT_THROW(cg::encode_bgi_archive_bg(legacy_patched24, kBpp16),
                 std::invalid_argument);
    EXPECT_THROW(cg::encode_bgi_archive_bg(legacy_patched24, kBpp8),
                 std::invalid_argument);

    cg::Image short_rgb = legacy_patched24;
    short_rgb.rgb.pop_back();
    EXPECT_THROW(cg::encode_bgi_archive_bg(short_rgb, kBpp24), std::invalid_argument);

    cg::Image bad_alpha = legacy_patched24;
    bad_alpha.alpha = {0x80};  // 1 byte for 2x1 image
    EXPECT_THROW(cg::encode_bgi_archive_bg(bad_alpha, kBpp24), std::invalid_argument);
    EXPECT_THROW(cg::encode_bgi_archive_bg(bad_alpha, kBpp32), std::invalid_argument);

    cg::Image wrong_size_donor;
    wrong_size_donor.width = 1;
    wrong_size_donor.height = 1;
    wrong_size_donor.rgb = {0x00, 0x00, 0x00};
    wrong_size_donor.alpha = {0x7F};
    EXPECT_THROW(
        cg::encode_bgi_archive_bg(legacy_patched24, kBpp32, &wrong_size_donor),
        std::invalid_argument);
    EXPECT_THROW(
        cg::encode_bgi_archive_bg(legacy_patched24, kBpp32, &bad_alpha),
        std::invalid_argument);
}

TEST(CgRepacker, FontDrawStraightAlphaOverPreservesRgbAndWeightsAlpha) {
    auto font = cg::Font::load("C:/Windows/Fonts/arial.ttf", 28);
    if (!font) {
        GTEST_SKIP() << "arial.ttf not present on host";
    }

    constexpr int kCanvasW = 40;
    constexpr int kCanvasH = 40;
    constexpr std::size_t kPixels = static_cast<std::size_t>(kCanvasW * kCanvasH);

    // 1. Transparent canvas (0,0,0,0) + pure white ink (255,255,255):
    //    Straight-alpha compositing must keep RGB == (255,255,255) even on
    //    anti-aliased semi-transparent edge pixels (never premultiplied gray).
    cg::Image transparent_canvas;
    transparent_canvas.width = kCanvasW;
    transparent_canvas.height = kCanvasH;
    transparent_canvas.rgb.assign(kPixels * 3u, 0u);
    transparent_canvas.alpha.assign(kPixels, 0u);
    font->draw(transparent_canvas, 4, 4, "O", {255u, 255u, 255u});

    std::size_t solid_pixels = 0;
    std::size_t edge_pixels = 0;
    for (int y = 0; y < kCanvasH; ++y) {
        for (int x = 0; x < kCanvasW; ++x) {
            const std::uint8_t a = *transparent_canvas.alpha_px(x, y);
            const std::uint8_t* p = transparent_canvas.px(x, y);
            if (a == 0u) {
                EXPECT_EQ(p[0], 0u);
                EXPECT_EQ(p[1], 0u);
                EXPECT_EQ(p[2], 0u);
                continue;
            }
            if (a == 255u) {
                ++solid_pixels;
            } else if (a >= 10u && a <= 245u) {
                ++edge_pixels;
            }
            EXPECT_EQ(p[0], 255u);
            EXPECT_EQ(p[1], 255u);
            EXPECT_EQ(p[2], 255u);
        }
    }
    EXPECT_GT(solid_pixels, 0u);
    EXPECT_GT(edge_pixels, 0u);

    // 2. Semi-transparent blue canvas (0,0,255,128) + pure red ink (255,0,0):
    //    At mid-coverage edge pixels (cov in [100, 155]), straight-alpha
    //    Porter-Duff over weights foreground ~2:1 over the 50%-alpha background
    //    (e.g. cov=128 -> R=170, G=0, B=85, A=192), never naive 50/50 (R~128).
    cg::Image semi_canvas;
    semi_canvas.width = kCanvasW;
    semi_canvas.height = kCanvasH;
    semi_canvas.rgb.resize(kPixels * 3u);
    for (std::size_t i = 0; i < kPixels; ++i) {
        semi_canvas.rgb[i * 3u + 0u] = 0u;
        semi_canvas.rgb[i * 3u + 1u] = 0u;
        semi_canvas.rgb[i * 3u + 2u] = 255u;
    }
    semi_canvas.alpha.assign(kPixels, 128u);
    font->draw(semi_canvas, 4, 4, "O", {255u, 0u, 0u});

    std::size_t mid_edge_checked = 0;
    for (int y = 0; y < kCanvasH; ++y) {
        for (int x = 0; x < kCanvasW; ++x) {
            const std::uint8_t cov = *transparent_canvas.alpha_px(x, y);
            const std::uint8_t* p = semi_canvas.px(x, y);
            const std::uint8_t out_a = *semi_canvas.alpha_px(x, y);
            if (cov == 0u) {
                EXPECT_EQ(p[0], 0u);
                EXPECT_EQ(p[1], 0u);
                EXPECT_EQ(p[2], 255u);
                EXPECT_EQ(out_a, 128u);
                continue;
            }
            if (cov >= 100u && cov <= 155u) {
                ++mid_edge_checked;
                EXPECT_GT(p[0], static_cast<std::uint8_t>(cov + 25u));
                EXPECT_EQ(p[1], 0u);
                EXPECT_LT(p[2], static_cast<std::uint8_t>(255u - cov - 25u));
                EXPECT_GE(out_a, 175u);
                EXPECT_LE(out_a, 210u);
            }
        }
    }
    EXPECT_GT(mid_edge_checked, 0u);
}

TEST(CgRepacker, ProbeArchiveBgSpecCoversPartitionsAndBoundaries) {
    const std::array<std::uint8_t, bgi::kSysGrpReservedBytes> res = {
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0,
    };
    const Bytes raw_sysgrp32 = bgi::encode_sysgrp(
        menu::BgraImage(3, 2, menu::BgraColor{1, 2, 3, 4}), kBpp32, res);
    const Bytes dsc_sysgrp32 = bgi::dsc_compress(raw_sysgrp32);
    const Bytes raw_sysgrp24 = bgi::encode_sysgrp(
        menu::BgraImage(4, 1, menu::BgraColor{1, 2, 3, 0xFF}), kBpp24, res);
    const Bytes bmp24 =
        menu::BgraImage(5, 3, menu::BgraColor{1, 2, 3, 0xFF}).EncodeBmp(kBpp24);
    const Bytes bmp32 =
        menu::BgraImage(6, 2, menu::BgraColor{1, 2, 3, 0x80}).EncodeBmp(kBpp32);
    Bytes truncated_bmp(kBmpTruncatedBytes, 0u);
    truncated_bmp[0] = kBmpSigB;
    truncated_bmp[1] = kBmpSigM;
    truncated_bmp[kBmpBppOffset] = static_cast<std::uint8_t>(kBpp24);

    struct ProbeCase {
        const char* name;
        Bytes payload;
        bool expect_ok;
        int expect_w;
        int expect_h;
        int expect_bpp;
        std::array<std::uint8_t, bgi::kSysGrpReservedBytes> expect_reserved;
    };

    const std::array<ProbeCase, 11> kCases = {{
        {"cbg_24bpp", make_cbg_header(1280, 720, kBpp24, kCbgHeaderProbeBytes),
         true, 1280, 720, kBpp24, {}},
        {"cbg_32bpp", make_cbg_header(1280, 720, kBpp32, kCbgHeaderProbeBytes),
         true, 1280, 720, kBpp32, {}},
        {"cbg_8bpp_unsupported",
         make_cbg_header(1280, 720, kBpp8, kCbgHeaderProbeBytes),
         false, 0, 0, 0, {}},
        {"cbg_zero_width_rejected",
         make_cbg_header(0, 720, kBpp32, kCbgHeaderProbeBytes),
         false, 0, 0, 0, {}},
        {"cbg_truncated_seam",
         make_cbg_header(1280, 720, kBpp32, kCbgTruncatedBytes),
         false, 0, 0, 0, {}},
        {"dsc_sysgrp_32bpp", dsc_sysgrp32, true, 3, 2, kBpp32, res},
        {"raw_sysgrp_24bpp", raw_sysgrp24, true, 4, 1, kBpp24, res},
        {"bmp_24bpp", bmp24, true, 5, 3, kBpp24, {}},
        {"bmp_32bpp", bmp32, true, 6, 2, kBpp32, {}},
        {"bmp_truncated_30b", truncated_bmp, false, 0, 0, 0, {}},
        {"garbage", Bytes{0xDE, 0xAD, 0xBE, 0xEF}, false, 0, 0, 0, {}},
    }};

    for (const ProbeCase& tc : kCases) {
        SCOPED_TRACE(tc.name);
        const auto probed = cg::probe_archive_bg_spec(tc.payload);
        ASSERT_EQ(probed.has_value(), tc.expect_ok);
        if (tc.expect_ok) {
            EXPECT_EQ(probed->width, tc.expect_w);
            EXPECT_EQ(probed->height, tc.expect_h);
            EXPECT_EQ(probed->bpp, tc.expect_bpp);
            EXPECT_EQ(probed->reserved, tc.expect_reserved);
        }
    }
}

TEST(CgRepacker, SkipListEnforcedAndRepackIsIdempotentFromBackup) {
    const ScopedTempDir scratch("snu_ut_cg_repacker_idempotent");
    const fs::path arc_path = scratch.path() / fs::u8path("data02503.arc");
    const fs::path bak_path = scratch.path() / fs::u8path("data02503.arc.backup");
    const fs::path tmp_path = scratch.path() / fs::u8path("data02503.arc.tmp");

    const Bytes orig_kt_ib = make_cbg_header(1280, 720, kBpp32, 32u);
    const Bytes orig_kt_mh = make_cbg_header(1280, 720, kBpp32, 36u);
    const Bytes orig_kt_ny = make_cbg_header(1280, 720, kBpp32, 40u);
    const Bytes orig_keep = make_cbg_header(1280, 720, kBpp24, 28u);
    const Bytes orig_patch_a = make_cbg_header(1280, 720, kBpp24, 30u);
    const Bytes orig_patch_b = make_cbg_header(1280, 720, kBpp32, 34u);

    const Bytes pristine_arc = make_arc({
        {"kt_ib", orig_kt_ib},
        {"kt_mh", orig_kt_mh},
        {"kt_ny", orig_kt_ny},
        {"keep_bg", orig_keep},
        {"patch_a", orig_patch_a},
        {"patch_b", orig_patch_b},
    });
    snu::write_file(arc_path.u8string(), pristine_arc);

    cg::Image replacement_img;
    replacement_img.width = 2;
    replacement_img.height = 1;
    replacement_img.rgb = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    replacement_img.alpha = {0x00, 0xFF};
    const Bytes dsc_a1 = cg::encode_bgi_archive_bg(replacement_img, kBpp24);
    const Bytes dsc_b1 = cg::encode_bgi_archive_bg(replacement_img, kBpp32);
    const Bytes bogus_bmp = {'B', 'M', 0x00, 0x01};

    const cg::ArchiveRepacker repacker;
    EXPECT_TRUE(repacker.is_skipped("kt_ib"));
    EXPECT_TRUE(repacker.is_skipped("kt_mh"));
    EXPECT_TRUE(repacker.is_skipped("kt_ny"));
    EXPECT_FALSE(repacker.is_skipped("patch_a"));

    // First repack: skipped entries must not be applied; .backup captures pristine bytes.
    const cg::RepackResult r1 = repacker.repack_from_backup(
        arc_path.u8string(), {
                                 {"kt_ib", bogus_bmp},
                                 {"kt_mh", bogus_bmp},
                                 {"kt_ny", bogus_bmp},
                                 {"patch_a", dsc_a1},
                                 {"patch_b", dsc_b1},
                             });
    EXPECT_EQ(r1.applied_count, 2u);
    EXPECT_EQ(r1.skipped_count, 3u);
    EXPECT_EQ(r1.backup_path, bak_path.u8string());
    ASSERT_TRUE(fs::exists(bak_path));
    EXPECT_FALSE(fs::exists(tmp_path));
    EXPECT_EQ(snu::read_file(bak_path.u8string()), pristine_arc);

    EXPECT_EQ(bgi::read_entry_by_name(arc_path.u8string(), "kt_ib", false), orig_kt_ib);
    EXPECT_EQ(bgi::read_entry_by_name(arc_path.u8string(), "kt_mh", false), orig_kt_mh);
    EXPECT_EQ(bgi::read_entry_by_name(arc_path.u8string(), "kt_ny", false), orig_kt_ny);
    EXPECT_EQ(bgi::read_entry_by_name(arc_path.u8string(), "keep_bg", false), orig_keep);
    EXPECT_EQ(bgi::read_entry_by_name(arc_path.u8string(), "patch_a", false), dsc_a1);
    EXPECT_EQ(bgi::read_entry_by_name(arc_path.u8string(), "patch_b", false), dsc_b1);

    // Simulate a legacy dirty live archive where `kt_ib` had previously been
    // overwritten with a Windows BMP before skip enforcement existed.
    bgi::repack_arc(arc_path.u8string(), tmp_path.u8string(), {{"kt_ib", bogus_bmp}});
    fs::rename(tmp_path, arc_path);
    ASSERT_EQ(bgi::read_entry_by_name(arc_path.u8string(), "kt_ib", false), bogus_bmp);

    // Second repack with `patch_b` removed and `kt_ib` still in candidate input:
    // rebuilding from `.backup` must restore BOTH `patch_b` and the dirtied `kt_ib`.
    cg::Image replacement_img2 = replacement_img;
    replacement_img2.rgb = {0x99, 0x88, 0x77, 0x66, 0x55, 0x44};
    const Bytes dsc_a2 = cg::encode_bgi_archive_bg(replacement_img2, kBpp24);

    const cg::RepackResult r2 = repacker.repack_from_backup(
        arc_path.u8string(), {{"kt_ib", bogus_bmp}, {"patch_a", dsc_a2}});
    EXPECT_EQ(r2.applied_count, 1u);
    EXPECT_EQ(r2.skipped_count, 1u);
    EXPECT_EQ(snu::read_file(bak_path.u8string()), pristine_arc);
    EXPECT_EQ(bgi::read_entry_by_name(arc_path.u8string(), "kt_ib", false), orig_kt_ib);
    EXPECT_EQ(bgi::read_entry_by_name(arc_path.u8string(), "patch_b", false), orig_patch_b);
    EXPECT_EQ(bgi::read_entry_by_name(arc_path.u8string(), "patch_a", false), dsc_a2);

    // Third repack with empty replacements while `.backup` exists: restores
    // entire archive byte-for-byte to pristine `.backup`.
    const cg::RepackResult r3 =
        repacker.repack_from_backup(arc_path.u8string(), {});
    EXPECT_EQ(r3.applied_count, 0u);
    EXPECT_EQ(r3.skipped_count, 0u);
    EXPECT_EQ(snu::read_file(arc_path.u8string()), pristine_arc);

    // Fourth repack with all-skipped replacements on a dirtied archive while
    // `.backup` exists: also restores byte-for-byte to pristine `.backup`.
    bgi::repack_arc(arc_path.u8string(), tmp_path.u8string(), {{"kt_ib", bogus_bmp}});
    fs::rename(tmp_path, arc_path);
    ASSERT_NE(snu::read_file(arc_path.u8string()), pristine_arc);

    const cg::RepackResult r4 =
        repacker.repack_from_backup(arc_path.u8string(), {{"kt_ib", bogus_bmp}});
    EXPECT_EQ(r4.applied_count, 0u);
    EXPECT_EQ(r4.skipped_count, 1u);
    EXPECT_EQ(snu::read_file(arc_path.u8string()), pristine_arc);
}

TEST(CgRepacker, SkippedOnlyNoBackupIsNoOpAndCorruptArchiveRollsBack) {
    const ScopedTempDir scratch("snu_ut_cg_repacker_edges");
    const cg::ArchiveRepacker repacker;

    // 1. All replacements skipped and no `.backup`: no-op, no `.backup` created.
    const fs::path pristine_path = scratch.path() / fs::u8path("pristine.arc");
    const fs::path pristine_bak = scratch.path() / fs::u8path("pristine.arc.backup");
    const Bytes pristine_arc = make_arc({{"kt_ib", Bytes{1, 2, 3, 4}}});
    snu::write_file(pristine_path.u8string(), pristine_arc);

    const cg::RepackResult noop =
        repacker.repack_from_backup(pristine_path.u8string(), {{"kt_ib", Bytes{9, 9}}});
    EXPECT_EQ(noop.applied_count, 0u);
    EXPECT_EQ(noop.skipped_count, 1u);
    EXPECT_TRUE(noop.backup_path.empty());
    EXPECT_FALSE(fs::exists(pristine_bak));
    EXPECT_EQ(snu::read_file(pristine_path.u8string()), pristine_arc);

    // 2. Valid -> Failing -> Valid seam: corrupt archive throws, leaves no `.tmp`/`.backup`,
    //    and does not poison subsequent repacks on the same `ArchiveRepacker`.
    const fs::path corrupt_path = scratch.path() / fs::u8path("corrupt.arc");
    const fs::path corrupt_bak = scratch.path() / fs::u8path("corrupt.arc.backup");
    const fs::path corrupt_tmp = scratch.path() / fs::u8path("corrupt.arc.tmp");
    const Bytes corrupt_bytes = {'N', 'O', 'T', 'A', 'R', 'C', 0, 0};
    snu::write_file(corrupt_path.u8string(), corrupt_bytes);

    EXPECT_THROW(
        repacker.repack_from_backup(corrupt_path.u8string(), {{"bg", Bytes{1, 2}}}),
        std::runtime_error);
    EXPECT_FALSE(fs::exists(corrupt_bak));
    EXPECT_FALSE(fs::exists(corrupt_tmp));
    EXPECT_EQ(snu::read_file(corrupt_path.u8string()), corrupt_bytes);

    const fs::path valid_path = scratch.path() / fs::u8path("valid.arc");
    snu::write_file(valid_path.u8string(), make_arc({{"bg", Bytes{1, 2, 3}}}));
    const cg::RepackResult after_err =
        repacker.repack_from_backup(valid_path.u8string(), {{"bg", Bytes{4, 5, 6, 7}}});
    EXPECT_EQ(after_err.applied_count, 1u);
    EXPECT_EQ(after_err.skipped_count, 0u);
    EXPECT_EQ(bgi::read_entry_by_name(valid_path.u8string(), "bg", false),
              (Bytes{4, 5, 6, 7}));
}
