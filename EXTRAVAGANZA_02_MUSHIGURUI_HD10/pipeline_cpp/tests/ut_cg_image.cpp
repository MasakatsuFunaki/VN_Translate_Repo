// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Channel-count fidelity for the narrative-CG image layer.
//
// 822 of the 1001 assets in this game are PNG colour type 6.  If open_image
// decides its channel count from stb's inferred `comp` instead of the container
// header, every alpha-bearing UI button repacks as an opaque box -- so the
// mapping from container header to channel count is pinned here explicitly.
#include "cg/image.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace mgi;
using namespace mgi::cg;

namespace {

void put_be32(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 24));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}

std::uint32_t crc32_of(const std::uint8_t* p, std::size_t n) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void add_chunk(Bytes& png, const char* type, const Bytes& data) {
    put_be32(png, static_cast<std::uint32_t>(data.size()));
    Bytes body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    png.insert(png.end(), body.begin(), body.end());
    put_be32(png, crc32_of(body.data(), body.size()));
}

// Minimal zlib stream using stored (uncompressed) deflate blocks -- enough for
// any real PNG decoder and no zlib dependency in the test.
Bytes zlib_stored(const Bytes& raw) {
    Bytes out = {0x78, 0x01};
    std::size_t pos = 0;
    do {
        const std::size_t n = std::min<std::size_t>(raw.size() - pos, 0xFFFF);
        const bool last = pos + n >= raw.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(n));
        out.push_back(static_cast<std::uint8_t>(n >> 8));
        out.push_back(static_cast<std::uint8_t>(~n));
        out.push_back(static_cast<std::uint8_t>(~n >> 8));
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                   raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < raw.size());
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    put_be32(out, (b << 16) | a);
    return out;
}

// A valid 8-bit PNG of the given colour type, filled with a constant sample.
Bytes make_png(int w, int h, int colour_type, std::uint8_t sample) {
    static const int SAMPLES[] = {1, 0, 3, 1, 2, 0, 4};
    const int bpp = SAMPLES[colour_type];

    Bytes png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    Bytes ihdr;
    put_be32(ihdr, static_cast<std::uint32_t>(w));
    put_be32(ihdr, static_cast<std::uint32_t>(h));
    ihdr.push_back(8);
    ihdr.push_back(static_cast<std::uint8_t>(colour_type));
    ihdr.insert(ihdr.end(), {0, 0, 0});
    add_chunk(png, "IHDR", ihdr);

    if (colour_type == 3) {
        Bytes plte;
        for (int i = 0; i < 4; ++i)
            plte.insert(plte.end(), {static_cast<std::uint8_t>(i * 40), 0x20, 0x10});
        add_chunk(png, "PLTE", plte);
    }

    Bytes raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);  // filter type: none
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < bpp; ++c)
                raw.push_back(colour_type == 3 ? static_cast<std::uint8_t>(x % 4) : sample);
    }
    add_chunk(png, "IDAT", zlib_stored(raw));
    add_chunk(png, "IEND", {});
    return png;
}

void put_le32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_le16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}

Bytes make_bmp(int w, int h, int bpp) {
    const std::size_t palette = (bpp == 8) ? 256u * 4 : 0u;
    const std::size_t stride = ((static_cast<std::size_t>(w) * bpp / 8) + 3) & ~std::size_t(3);
    const std::size_t offbits = 14 + 40 + palette;
    const std::size_t total = offbits + stride * h;

    Bytes b = {'B', 'M'};
    put_le32(b, static_cast<std::uint32_t>(total));
    put_le32(b, 0);
    put_le32(b, static_cast<std::uint32_t>(offbits));
    put_le32(b, 40);
    put_le32(b, static_cast<std::uint32_t>(w));
    put_le32(b, static_cast<std::uint32_t>(h));
    put_le16(b, 1);
    put_le16(b, static_cast<std::uint16_t>(bpp));
    put_le32(b, 0);
    put_le32(b, static_cast<std::uint32_t>(stride * h));
    put_le32(b, 2835);
    put_le32(b, 2835);
    put_le32(b, bpp == 8 ? 256 : 0);
    put_le32(b, 0);
    for (std::size_t i = 0; i < palette / 4; ++i)
        b.insert(b.end(), {static_cast<std::uint8_t>(i), 0x40, 0x80, 0});
    b.resize(total, 0x11);
    return b;
}

}  // namespace

TEST(CgImage, channels_come_from_the_container_header) {
    // PNG: ct2 -> RGB; ct6 -> RGBA; ct0/3/4 (grey/palette/grey+alpha) -> RGBA.
    EXPECT_EQ(decode_image(make_png(4, 3, 2, 0x30))->channels, 3);
    EXPECT_EQ(decode_image(make_png(4, 3, 6, 0x30))->channels, 4);
    EXPECT_EQ(decode_image(make_png(4, 3, 0, 0x30))->channels, 4);
    EXPECT_EQ(decode_image(make_png(4, 3, 3, 0x30))->channels, 4);
    EXPECT_EQ(decode_image(make_png(4, 3, 4, 0x30))->channels, 4);

    // BMP: 1/4/8bpp are palettised -> RGBA; 16/24 -> RGB; 32 -> RGBA.
    EXPECT_EQ(decode_image(make_bmp(4, 3, 8))->channels, 4);
    EXPECT_EQ(decode_image(make_bmp(4, 3, 24))->channels, 3);

    // JPEG -> RGB.
    Image rgb;
    rgb.width = rgb.height = 8;
    rgb.channels = 3;
    rgb.px.assign(8 * 8 * 3, 0x77);
    EXPECT_EQ(decode_image(encode_jpg(rgb, 90))->channels, 3);

    // An unrecognised container is not an image.
    EXPECT_FALSE(decode_image(Bytes{1, 2, 3, 4, 5, 6, 7, 8}).has_value());
}

TEST(CgImage, decode_preserves_dimensions_and_samples) {
    const auto img = decode_image(make_png(5, 2, 6, 0xA5));
    ASSERT_TRUE(img.has_value());
    EXPECT_EQ(img->width, 5);
    EXPECT_EQ(img->height, 2);
    EXPECT_EQ(img->at(0, 0)[0], 0xA5u);
    EXPECT_EQ(img->at(4, 1)[3], 0xA5u);
}

TEST(CgImage, encode_png_preserves_colour_type) {
    Image rgb;
    rgb.width = rgb.height = 4;
    rgb.channels = 3;
    rgb.px.assign(4 * 4 * 3, 0x10);
    const Bytes p3 = encode_png(rgb);
    EXPECT_EQ(p3[0x19], 2u);  // IHDR colour type

    Image rgba;
    rgba.width = rgba.height = 4;
    rgba.channels = 4;
    rgba.px.assign(4 * 4 * 4, 0x10);
    const Bytes p4 = encode_png(rgba);
    EXPECT_EQ(p4[0x19], 6u);

    // Round-trip through our own decoder keeps the channel count.
    EXPECT_EQ(decode_image(p3)->channels, 3);
    EXPECT_EQ(decode_image(p4)->channels, 4);
}

TEST(CgImage, drop_alpha_does_not_composite) {
    Image rgba;
    rgba.width = rgba.height = 1;
    rgba.channels = 4;
    rgba.px = {200, 100, 50, 0};
    const Image rgb = drop_alpha(rgba);
    EXPECT_EQ(rgb.channels, 3);
    EXPECT_EQ(rgb.px, (Bytes{200, 100, 50}));  // NOT white, NOT black
}

TEST(CgImage, crop_is_half_open_and_clamped) {
    Image img;
    img.width = img.height = 10;
    img.channels = 4;
    img.px.assign(10 * 10 * 4, 0);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x) img.at(x, y)[0] = static_cast<std::uint8_t>(y * 10 + x);

    const Image c = crop(img, 2, 3, 5, 7);
    EXPECT_EQ(c.width, 3);
    EXPECT_EQ(c.height, 4);
    EXPECT_EQ(c.channels, 4);
    EXPECT_EQ(c.at(0, 0)[0], 32u);
    EXPECT_EQ(c.at(2, 3)[0], 64u);

    const Image over = crop(img, -5, -5, 100, 100);
    EXPECT_EQ(over.width, 10);
    EXPECT_EQ(over.height, 10);
}

TEST(CgImage, base64_pads_to_a_multiple_of_four) {
    EXPECT_EQ(base64_encode(Bytes{'a'}), "YQ==");
    EXPECT_EQ(base64_encode(Bytes{'a', 'b'}), "YWI=");
    EXPECT_EQ(base64_encode(Bytes{'a', 'b', 'c'}), "YWJj");
    EXPECT_EQ(base64_encode(Bytes{}), "");
}

TEST(CgImage, bmp_32bpp_preserves_straight_alpha_and_sign_bit_boundaries) {
    constexpr int kWidth = 4;
    constexpr int kHeight = 2;
    constexpr int kRgbaChannels = 4;
    constexpr std::size_t kBmpBitCountOffset = 28u;
    constexpr std::size_t kBmpCompressionOffset = 30u;
    constexpr std::size_t kBmpOffBitsOffset = 10u;

    Image rgba;
    rgba.width = kWidth;
    rgba.height = kHeight;
    rgba.channels = kRgbaChannels;
    // Covers 0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF boundary alpha bytes and
    // non-zero straight RGB stored beneath alpha == 0.
    rgba.px = {
        10u,  20u,  30u,  0x00u,
        40u,  50u,  60u,  0x01u,
        70u,  80u,  90u,  0x7Fu,
        100u, 110u, 120u, 0x80u,
        130u, 140u, 150u, 0xFEu,
        160u, 170u, 180u, 0xFFu,
        200u, 210u, 220u, 0x00u,
        230u, 240u, 250u, 0x80u,
    };

    const Bytes encoded = encode_bmp(rgba);
    ASSERT_EQ(encoded.size(), 54u + rgba.px.size());
    EXPECT_EQ(encoded[0], 'B');
    EXPECT_EQ(encoded[1], 'M');
    const std::uint16_t bpp =
        static_cast<std::uint16_t>(encoded[kBmpBitCountOffset] |
                                   (encoded[kBmpBitCountOffset + 1u] << 8u));
    EXPECT_EQ(bpp, 32u);
    EXPECT_EQ(encoded[kBmpCompressionOffset], 0u);
    EXPECT_EQ(encoded[kBmpOffBitsOffset], 54u);

    // Pin exact bottom-up BGRA wire bytes (row y=1 first, then row y=0) so a
    // symmetric R/B swap or vertical flip in both encode & decode cannot pass.
    const Bytes expected_bgra_wire = {
        // Row y=1 (bottom row stored first in bottom-up BMP):
        150u, 140u, 130u, 0xFEu,
        180u, 170u, 160u, 0xFFu,
        220u, 210u, 200u, 0x00u,
        250u, 240u, 230u, 0x80u,
        // Row y=0 (top row stored second):
        30u,  20u,  10u,  0x00u,
        60u,  50u,  40u,  0x01u,
        90u,  80u,  70u,  0x7Fu,
        120u, 110u, 100u, 0x80u,
    };
    const Bytes actual_wire(encoded.begin() + 54, encoded.end());
    EXPECT_EQ(actual_wire, expected_bgra_wire);

    const auto decoded = decode_image(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, kWidth);
    EXPECT_EQ(decoded->height, kHeight);
    EXPECT_EQ(decoded->channels, kRgbaChannels);
    EXPECT_EQ(decoded->px, rgba.px);

    // Top-down (negative biHeight = -2) 32-bpp BI_RGB BMP stores row y=0 first.
    Bytes top_down = encoded;
    const std::uint32_t neg_h = static_cast<std::uint32_t>(-kHeight);
    top_down[22] = static_cast<std::uint8_t>(neg_h & 0xFFu);
    top_down[23] = static_cast<std::uint8_t>((neg_h >> 8u) & 0xFFu);
    top_down[24] = static_cast<std::uint8_t>((neg_h >> 16u) & 0xFFu);
    top_down[25] = static_cast<std::uint8_t>((neg_h >> 24u) & 0xFFu);
    // Reorder wire rows so row y=0 is stored first in top-down layout.
    std::copy(expected_bgra_wire.begin() + 16, expected_bgra_wire.end(), top_down.begin() + 54);
    std::copy(expected_bgra_wire.begin(), expected_bgra_wire.begin() + 16, top_down.begin() + 70);
    const auto decoded_td = decode_image(top_down);
    ASSERT_TRUE(decoded_td.has_value());
    EXPECT_EQ(decoded_td->width, kWidth);
    EXPECT_EQ(decoded_td->height, kHeight);
    EXPECT_EQ(decoded_td->channels, kRgbaChannels);
    EXPECT_EQ(decoded_td->px, rgba.px);

    // All-zero alpha 32-bpp BMP must keep alpha == 0 rather than promoting to 255.
    Image all_zero;
    all_zero.width = 2;
    all_zero.height = 2;
    all_zero.channels = kRgbaChannels;
    all_zero.px = {
        11u, 22u, 33u, 0u, 44u, 55u, 66u, 0u,
        77u, 88u, 99u, 0u, 12u, 34u, 56u, 0u,
    };
    const auto decoded_zero = decode_image(encode_bmp(all_zero));
    ASSERT_TRUE(decoded_zero.has_value());
    EXPECT_EQ(decoded_zero->channels, kRgbaChannels);
    EXPECT_EQ(decoded_zero->px, all_zero.px);

    // Corrupt / boundary 32-bpp BMP headers must return nullopt without UB or overflow.
    {
        // biHeight == INT32_MIN (-2147483648)
        Bytes int32_min_h = encoded;
        int32_min_h[22] = 0x00u;
        int32_min_h[23] = 0x00u;
        int32_min_h[24] = 0x00u;
        int32_min_h[25] = 0x80u;
        EXPECT_FALSE(decode_image(int32_min_h).has_value());

        // Huge w * h multiplication overflow (0x40000001 x 4)
        Bytes overflow_dims = encoded;
        overflow_dims[18] = 0x01u;
        overflow_dims[19] = 0x00u;
        overflow_dims[20] = 0x00u;
        overflow_dims[21] = 0x40u;
        overflow_dims[22] = 0x04u;
        overflow_dims[23] = 0x00u;
        overflow_dims[24] = 0x00u;
        overflow_dims[25] = 0x00u;
        EXPECT_FALSE(decode_image(overflow_dims).has_value());

        // Truncated pixel data
        Bytes truncated(encoded.begin(), encoded.end() - 1);
        EXPECT_FALSE(decode_image(truncated).has_value());

        // bfOffBits < 54
        Bytes bad_offbits = encoded;
        bad_offbits[10] = 53u;
        EXPECT_FALSE(decode_image(bad_offbits).has_value());
    }
}
