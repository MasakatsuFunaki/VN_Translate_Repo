// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Standalone unit tests for pipeline_cpp/src/menu/menu_patcher.{h,cpp}:
// FXF bitwise-NOT codec, MurmurHash2 + XOR-delta masking, RLE-XOR alpha mask
// round-trips, DWQ crop margins and PACKTYPE=0/2A/8A round-trips, and synthetic
// GPK/GTB rebuilding.
#include "menu/menu_patcher.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using exm::menu::ComputeMurmurHash2;
using exm::menu::MenuArchivePatcher;
using exm::menu::MenuPatcher;
using exm::menu::RgbaImage;
using exm::menu::XorMaskPayload;
using exm::menu::compute_crop_margins;
using exm::menu::decode_dwq_to_rgba;
using exm::menu::decode_rle_xor_mask;
using exm::menu::decrypt_fxf;
using exm::menu::encode_dwq_from_png_bytes;
using exm::menu::encode_dwq_from_rgba;
using exm::menu::encode_rgba_to_png_bytes;
using exm::menu::encode_rle_xor_mask;
using exm::menu::encrypt_fxf;
using exm::menu::patch_backlog_fxf;
using exm::menu::patch_main_control_fxf;

constexpr std::size_t kDwqHeaderSize = 64;
constexpr std::size_t kDwqTypeStrFieldLen = 16;
constexpr std::size_t kDwqCropMarginsOffset = 0x10;
constexpr std::size_t kDwqDataSizeOffset = 0x20;
constexpr std::size_t kDwqWidthOffset = 0x24;
constexpr std::size_t kDwqHeightOffset = 0x28;
constexpr std::size_t kDwqBppOffset = 0x2A;
constexpr std::size_t kDwqPackStrOffset = 0x2C;
constexpr std::size_t kDwqPackStrFieldLen = 20;

constexpr std::size_t kCropTopIndex = 0;
constexpr std::size_t kCropLeftIndex = 1;
constexpr std::size_t kCropRightIndex = 2;
constexpr std::size_t kCropBottomIndex = 3;
constexpr std::size_t kCropTopByteOffset = 0;
constexpr std::size_t kCropLeftByteOffset = 4;
constexpr std::size_t kCropRightByteOffset = 8;
constexpr std::size_t kCropBottomByteOffset = 12;

constexpr std::uint32_t kByteShift1 = 8;
constexpr std::uint32_t kByteShift2 = 16;
constexpr std::uint32_t kByteShift3 = 24;
constexpr std::size_t kByteIndex0 = 0;
constexpr std::size_t kByteIndex1 = 1;
constexpr std::size_t kByteIndex2 = 2;
constexpr std::size_t kByteIndex3 = 3;

constexpr std::size_t kRgbaChannels = 4;
constexpr std::size_t kRgbChannels = 3;
constexpr std::size_t kRgbaRedIndex = 0;
constexpr std::size_t kRgbaGreenIndex = 1;
constexpr std::size_t kRgbaBlueIndex = 2;
constexpr std::size_t kRgbaAlphaIndex = 3;
constexpr std::uint16_t kBpp24 = 24;
constexpr std::uint16_t kBpp32 = 32;
constexpr std::uint8_t kOpaqueAlpha = 255;
constexpr std::uint8_t kByteMask = 0xFF;

constexpr std::uint32_t kMurmurDefaultSeed = 0x9747B28Cu;
constexpr std::uint8_t kSampleByte0 = 0x10;
constexpr std::uint8_t kSampleByte1 = 0x20;
constexpr std::uint8_t kSampleByte2 = 0x30;
constexpr std::uint8_t kSampleByte3 = 0x40;
constexpr std::uint8_t kSampleByte4 = 0x55;
constexpr std::uint8_t kSampleByte5 = 0xAA;
constexpr std::uint8_t kSampleByte6 = 0xFE;

constexpr std::uint16_t kCropTestWidth = 10;
constexpr std::uint16_t kCropTestHeight = 8;
constexpr int kCropActiveMinY = 1;
constexpr int kCropActiveMaxY = 5;
constexpr int kCropActiveMinX = 2;
constexpr int kCropActiveMaxX = 6;
constexpr std::int32_t kExpectedCropTop = 1;
constexpr std::int32_t kExpectedCropLeft = 2;
constexpr std::int32_t kExpectedCropRight = 3;
constexpr std::int32_t kExpectedCropBottom = 2;
constexpr std::uint8_t kCropSampleRed = 100;
constexpr std::uint8_t kCropSampleGreen = 150;
constexpr std::uint8_t kCropSampleBlue = 200;
constexpr std::uint8_t kCropSampleAlpha = 220;

constexpr std::uint16_t kPack8aRgbaWidth = 12;
constexpr std::uint16_t kPack8aRgbaHeight = 10;
constexpr int kPack8aActiveMinY = 2;
constexpr int kPack8aActiveMaxY = 7;
constexpr int kPack8aActiveMinX = 3;
constexpr int kPack8aActiveMaxX = 8;
constexpr std::int32_t kPack8aExpectedMarginTop = 2;
constexpr std::int32_t kPack8aExpectedMarginLeft = 3;
constexpr std::int32_t kPack8aExpectedMarginRight = 3;
constexpr std::int32_t kPack8aExpectedMarginBottom = 2;
constexpr int kPack8aRedStep = 20;
constexpr int kPack8aGreenStep = 25;
constexpr std::uint8_t kPack8aBlueVal = 180;
constexpr std::uint8_t kPack8aAlphaVal = 240;

constexpr std::uint16_t kPack8aRgbWidth = 8;
constexpr std::uint16_t kPack8aRgbHeight = 6;
constexpr std::size_t kRgbPatternPrimeR = 7;
constexpr std::size_t kRgbPatternPrimeG = 13;
constexpr std::size_t kRgbPatternPrimeB = 29;

constexpr std::uint16_t kPack0And2aWidth = 6;
constexpr std::uint16_t kPack0And2aHeight = 4;
constexpr std::size_t kPack0BaseRed = 10;
constexpr std::size_t kPack0BaseGreen = 40;
constexpr std::size_t kPack0BaseBlue = 80;
constexpr std::size_t kPack0BaseAlpha = 100;
constexpr std::size_t kPack0GreenMultiplier = 2;
constexpr std::size_t kPack0BlueMultiplier = 3;
constexpr std::size_t kPack0AlphaMultiplier = 4;

constexpr std::size_t kBacklogLeadingFullwidthSpaces = 8;

constexpr std::uint16_t kSyntheticSpriteDim = 4;
constexpr std::uint8_t kSyntheticInitFill = 120;
constexpr std::size_t kSyntheticPatternMultiplier = 37;
constexpr std::size_t kSyntheticPatternOffset = 19;
constexpr std::uint32_t kSyntheticEntryCount = 2;
constexpr std::size_t kSyntheticEntryIndex0 = 0;
constexpr std::size_t kSyntheticEntryIndex1 = 1;
constexpr std::size_t kSyntheticEntryEndIndex = 2;
constexpr std::size_t kSyntheticGtbCountBytes = 4;
constexpr std::size_t kSyntheticGtbTableEntries = 4;
constexpr std::size_t kSyntheticGtbNamesBlobBytes = 24;
constexpr std::size_t kSyntheticGtbNameOffset0Pos = 4;
constexpr std::size_t kSyntheticGtbNameOffset1Pos = 8;
constexpr std::size_t kSyntheticGtbDataOffset0Pos = 12;
constexpr std::size_t kSyntheticGtbDataOffset1Pos = 16;
constexpr std::size_t kSyntheticGtbNamesBlobPos = 20;
constexpr std::size_t kSyntheticGtbSecondNamePos = 32;
constexpr std::uint32_t kSyntheticName0Offset = 0;
constexpr std::uint32_t kSyntheticName1Offset = 12;
constexpr std::size_t kSyntheticSpriteNameLen = 10;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
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

std::uint32_t read_u32_le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[kByteIndex0]) |
           (static_cast<std::uint32_t>(p[kByteIndex1]) << kByteShift1) |
           (static_cast<std::uint32_t>(p[kByteIndex2]) << kByteShift2) |
           (static_cast<std::uint32_t>(p[kByteIndex3]) << kByteShift3);
}

std::int32_t read_i32_le(const std::uint8_t* p) {
    return static_cast<std::int32_t>(read_u32_le(p));
}

void write_u16_le(std::uint8_t* p, std::uint16_t v) {
    p[kByteIndex0] = static_cast<std::uint8_t>(v & kByteMask);
    p[kByteIndex1] = static_cast<std::uint8_t>((v >> kByteShift1) & kByteMask);
}

void write_u32_le(std::uint8_t* p, std::uint32_t v) {
    p[kByteIndex0] = static_cast<std::uint8_t>(v & kByteMask);
    p[kByteIndex1] = static_cast<std::uint8_t>((v >> kByteShift1) & kByteMask);
    p[kByteIndex2] = static_cast<std::uint8_t>((v >> kByteShift2) & kByteMask);
    p[kByteIndex3] = static_cast<std::uint8_t>((v >> kByteShift3) & kByteMask);
}

std::vector<std::uint8_t> make_dwq_header(std::uint16_t w, std::uint16_t h, std::uint16_t bpp,
                                     const char* type_str16,
                                     const char* pack_str20,
                                     std::uint32_t data_size) {
    std::vector<std::uint8_t> hdr(kDwqHeaderSize, 0);
    std::memcpy(hdr.data(), type_str16, kDwqTypeStrFieldLen);
    write_u32_le(hdr.data() + kDwqDataSizeOffset, data_size);
    write_u16_le(hdr.data() + kDwqWidthOffset, w);
    write_u16_le(hdr.data() + kDwqHeightOffset, h);
    write_u16_le(hdr.data() + kDwqBppOffset, bpp);
    std::memcpy(hdr.data() + kDwqPackStrOffset, pack_str20, kDwqPackStrFieldLen);
    return hdr;
}

TEST(MenuPatcherTest, FxfBitwiseNotRoundTrip) {
    const std::string sample =
        "name mainControl\r\nwindowTitle EXTRAVAGANZA ~Matsuro~ CE\r\n";
    auto enc = encrypt_fxf(sample);
    ASSERT_EQ(enc.size(), sample.size());
    for (std::size_t i = 0; i < sample.size(); ++i) {
        EXPECT_EQ(enc[i], static_cast<std::uint8_t>(~static_cast<std::uint8_t>(sample[i])));
    }
    auto dec = decrypt_fxf(enc);
    EXPECT_EQ(dec, sample);
}

TEST(MenuPatcherTest, MurmurHash2AndXorMaskRoundTrip) {
    const std::vector<std::uint8_t> original = {
        kSampleByte0, kSampleByte1, kSampleByte2, kSampleByte3,
        kSampleByte4, kSampleByte5, kSampleByte6,
    };
    std::vector<std::uint8_t> buf = original;
    std::uint32_t key = ComputeMurmurHash2(original.data(), original.size(), kMurmurDefaultSeed);
    EXPECT_NE(key, 0u);
    XorMaskPayload(buf.data(), buf.size(), key);
    EXPECT_NE(buf, original);
    XorMaskPayload(buf.data(), buf.size(), key);
    EXPECT_EQ(buf, original);

    const std::vector<std::uint8_t> patched = {
        kSampleByte6, kSampleByte5, kSampleByte4, kSampleByte3,
        kSampleByte2, kSampleByte1, kSampleByte0, kSampleByte4,
    };
    std::vector<std::uint8_t> delta_buf = patched;
    XorMaskPayload(delta_buf.data(), delta_buf.size(), original.data(), original.size(), key);
    EXPECT_NE(delta_buf, patched);
    XorMaskPayload(delta_buf.data(), delta_buf.size(), original.data(), original.size(), key);
    EXPECT_EQ(delta_buf, patched);
}

TEST(MenuPatcherTest, RleXorMaskRoundTripHandlesLongZeroRunsAndVerticalDeltas) {
    constexpr int kWidth = 320;
    constexpr int kHeight = 12;
    constexpr int kLongZeroRunLength = 300;
    constexpr int kRow0PatternMultiplier = 13;
    constexpr int kGradientFirstRow = 2;
    constexpr int kGradientMultiplier = 7;

    std::vector<std::uint8_t> alpha(static_cast<std::size_t>(kWidth) * kHeight, 0);
    for (int x = kLongZeroRunLength; x < kWidth; ++x) {
        alpha[x] = static_cast<std::uint8_t>((x * kRow0PatternMultiplier) & kByteMask);
    }
    std::memcpy(alpha.data() + kWidth, alpha.data(), kWidth);
    for (int y = kGradientFirstRow; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            alpha[static_cast<std::size_t>(y) * kWidth + x] =
                static_cast<std::uint8_t>((x * y * kGradientMultiplier) & kByteMask);
        }
    }

    const auto encoded = encode_rle_xor_mask(alpha.data(), kWidth, kHeight);
    EXPECT_LT(encoded.size(), alpha.size());

    const auto decoded = decode_rle_xor_mask(encoded.data(), encoded.size(), kWidth, kHeight);
    EXPECT_EQ(decoded, alpha);
}

TEST(MenuPatcherTest, ComputeCropMargins32BppRgba) {
    RgbaImage img;
    img.width = kCropTestWidth;
    img.height = kCropTestHeight;
    img.bpp = kBpp32;
    img.has_alpha = true;
    img.pixels.assign(static_cast<std::size_t>(img.width) * img.height * kRgbaChannels, 0);

    for (int y = kCropActiveMinY; y <= kCropActiveMaxY; ++y) {
        for (int x = kCropActiveMinX; x <= kCropActiveMaxX; ++x) {
            std::size_t idx = (static_cast<std::size_t>(y) * img.width + x) * kRgbaChannels;
            img.pixels[idx + kRgbaRedIndex] = kCropSampleRed;
            img.pixels[idx + kRgbaGreenIndex] = kCropSampleGreen;
            img.pixels[idx + kRgbaBlueIndex] = kCropSampleBlue;
            img.pixels[idx + kRgbaAlphaIndex] = kCropSampleAlpha;
        }
    }

    auto margins = compute_crop_margins(img);
    EXPECT_EQ(margins[kCropTopIndex], kExpectedCropTop);
    EXPECT_EQ(margins[kCropLeftIndex], kExpectedCropLeft);
    EXPECT_EQ(margins[kCropRightIndex], kExpectedCropRight);
    EXPECT_EQ(margins[kCropBottomIndex], kExpectedCropBottom);
}

TEST(MenuPatcherTest, DwqPackType8ARoundTripRgbaAndRgb) {
    RgbaImage src_rgba;
    src_rgba.width = kPack8aRgbaWidth;
    src_rgba.height = kPack8aRgbaHeight;
    src_rgba.bpp = kBpp32;
    src_rgba.has_alpha = true;
    src_rgba.pixels.assign(
        static_cast<std::size_t>(src_rgba.width) * src_rgba.height * kRgbaChannels, 0);

    for (int y = kPack8aActiveMinY; y <= kPack8aActiveMaxY; ++y) {
        for (int x = kPack8aActiveMinX; x <= kPack8aActiveMaxX; ++x) {
            std::size_t idx = (static_cast<std::size_t>(y) * src_rgba.width + x) * kRgbaChannels;
            src_rgba.pixels[idx + kRgbaRedIndex] = static_cast<std::uint8_t>(x * kPack8aRedStep);
            src_rgba.pixels[idx + kRgbaGreenIndex] =
                static_cast<std::uint8_t>(y * kPack8aGreenStep);
            src_rgba.pixels[idx + kRgbaBlueIndex] = kPack8aBlueVal;
            src_rgba.pixels[idx + kRgbaAlphaIndex] = kPack8aAlphaVal;
        }
    }

    auto png_rgba = encode_rgba_to_png_bytes(src_rgba, true);
    auto orig_dwq = make_dwq_header(src_rgba.width, src_rgba.height, kBpp32,
                                    "PNG             ", "    PACKTYPE=8A     ",
                                    static_cast<std::uint32_t>(png_rgba.size()));
    orig_dwq.insert(orig_dwq.end(), png_rgba.begin(), png_rgba.end());

    auto decoded = decode_dwq_to_rgba(orig_dwq.data(), orig_dwq.size());
    EXPECT_EQ(decoded.width, src_rgba.width);
    EXPECT_EQ(decoded.height, src_rgba.height);
    EXPECT_EQ(decoded.pixels, src_rgba.pixels);

    auto reencoded = encode_dwq_from_rgba(orig_dwq.data(), orig_dwq.size(), decoded, true);
    std::uint32_t new_data_size = read_u32_le(reencoded.data() + kDwqDataSizeOffset);
    EXPECT_EQ(reencoded.size(), kDwqHeaderSize + new_data_size);
    EXPECT_EQ(read_i32_le(reencoded.data() + kDwqCropMarginsOffset + kCropTopByteOffset),
              kPack8aExpectedMarginTop);
    EXPECT_EQ(read_i32_le(reencoded.data() + kDwqCropMarginsOffset + kCropLeftByteOffset),
              kPack8aExpectedMarginLeft);
    EXPECT_EQ(read_i32_le(reencoded.data() + kDwqCropMarginsOffset + kCropRightByteOffset),
              kPack8aExpectedMarginRight);
    EXPECT_EQ(read_i32_le(reencoded.data() + kDwqCropMarginsOffset + kCropBottomByteOffset),
              kPack8aExpectedMarginBottom);

    auto redecoded = decode_dwq_to_rgba(reencoded.data(), reencoded.size());
    EXPECT_EQ(redecoded.pixels, src_rgba.pixels);

    auto from_png = encode_dwq_from_png_bytes(orig_dwq.data(), orig_dwq.size(), png_rgba, true);
    EXPECT_EQ(read_u32_le(from_png.data() + kDwqDataSizeOffset),
              static_cast<std::uint32_t>(png_rgba.size()));
    EXPECT_EQ(decode_dwq_to_rgba(from_png.data(), from_png.size()).pixels, src_rgba.pixels);

    RgbaImage src_rgb;
    src_rgb.width = kPack8aRgbWidth;
    src_rgb.height = kPack8aRgbHeight;
    src_rgb.bpp = kBpp24;
    src_rgb.has_alpha = false;
    src_rgb.pixels.resize(
        static_cast<std::size_t>(src_rgb.width) * src_rgb.height * kRgbaChannels);
    for (std::size_t i = 0; i < static_cast<std::size_t>(src_rgb.width) * src_rgb.height; ++i) {
        src_rgb.pixels[i * kRgbaChannels + kRgbaRedIndex] =
            static_cast<std::uint8_t>((i * kRgbPatternPrimeR) & kByteMask);
        src_rgb.pixels[i * kRgbaChannels + kRgbaGreenIndex] =
            static_cast<std::uint8_t>((i * kRgbPatternPrimeG) & kByteMask);
        src_rgb.pixels[i * kRgbaChannels + kRgbaBlueIndex] =
            static_cast<std::uint8_t>((i * kRgbPatternPrimeB) & kByteMask);
        src_rgb.pixels[i * kRgbaChannels + kRgbaAlphaIndex] = kOpaqueAlpha;
    }
    auto png_rgb = encode_rgba_to_png_bytes(src_rgb, false);
    auto orig_dwq_rgb = make_dwq_header(src_rgb.width, src_rgb.height, kBpp24,
                                        "PNG             ", "    PACKTYPE=8A     ",
                                        static_cast<std::uint32_t>(png_rgb.size()));
    orig_dwq_rgb.insert(orig_dwq_rgb.end(), png_rgb.begin(), png_rgb.end());

    auto reencoded_rgb =
        encode_dwq_from_rgba(orig_dwq_rgb.data(), orig_dwq_rgb.size(), src_rgb, true);
    auto decoded_rgb = decode_dwq_to_rgba(reencoded_rgb.data(), reencoded_rgb.size());
    EXPECT_EQ(decoded_rgb.pixels, src_rgb.pixels);
}

TEST(MenuPatcherTest, DwqPackType0And2ARoundTrip) {
    constexpr std::size_t kBmpFileHeaderSize = 54;
    constexpr std::size_t kBmpInfoHeaderSize = 40;
    constexpr std::size_t kBmpOffBitsOffset = 10;
    constexpr std::size_t kBmpBiSizeOffset = 14;
    constexpr std::size_t kBmpWidthOffset = 18;
    constexpr std::size_t kBmpHeightOffset = 22;
    constexpr std::size_t kBmpPlanesOffset = 26;
    constexpr std::size_t kBmpBitCountOffset = 28;
    constexpr std::size_t kBmpSizeImageOffset = 34;
    constexpr std::size_t kBmpClrUsedOffset = 46;
    constexpr std::uint16_t kBmpPlanes = 1;
    constexpr std::uint16_t kBmpBpp24 = 24;
    constexpr std::uint16_t kBmpBpp8 = 8;
    constexpr std::size_t kBmpRowAlign = 4;
    constexpr std::size_t kMaskHeaderTotalBytes = 1078;
    constexpr std::uint32_t kMaskPaletteCount = 256;

    RgbaImage img;
    img.width = kPack0And2aWidth;
    img.height = kPack0And2aHeight;
    img.bpp = kBpp32;
    img.has_alpha = true;
    img.pixels.resize(
        static_cast<std::size_t>(img.width) * img.height * kRgbaChannels, 0);
    for (std::size_t i = 0; i < static_cast<std::size_t>(img.width) * img.height; ++i) {
        img.pixels[i * kRgbaChannels + kRgbaRedIndex] =
            static_cast<std::uint8_t>(kPack0BaseRed + i);
        img.pixels[i * kRgbaChannels + kRgbaGreenIndex] =
            static_cast<std::uint8_t>(kPack0BaseGreen + i * kPack0GreenMultiplier);
        img.pixels[i * kRgbaChannels + kRgbaBlueIndex] =
            static_cast<std::uint8_t>(kPack0BaseBlue + i * kPack0BlueMultiplier);
        img.pixels[i * kRgbaChannels + kRgbaAlphaIndex] =
            static_cast<std::uint8_t>(kPack0BaseAlpha + i * kPack0AlphaMultiplier);
    }

    const std::size_t stride =
        (static_cast<std::size_t>(img.width) * kRgbChannels + (kBmpRowAlign - 1)) & ~(kBmpRowAlign - 1);
    const std::size_t bmp_px_bytes = stride * static_cast<std::size_t>(img.height);
    const std::size_t bmp_total_bytes = kBmpFileHeaderSize + bmp_px_bytes;

    auto make_bm_dwq = [&](bool with_mask) {
        auto dwq = make_dwq_header(
            static_cast<std::uint16_t>(img.width), static_cast<std::uint16_t>(img.height), kBpp24,
            with_mask ? "PACKBMP+MASK    " : "BMP             ",
            with_mask ? "    PACKTYPE=2A     " : "    PACKTYPE=0      ",
            static_cast<std::uint32_t>(bmp_total_bytes));
        write_u32_le(dwq.data() + kDwqWidthOffset, static_cast<std::uint32_t>(img.width));
        write_u32_le(dwq.data() + kDwqHeightOffset, static_cast<std::uint32_t>(img.height));

        std::vector<std::uint8_t> bmp_hdr(kBmpFileHeaderSize, 0);
        bmp_hdr[kByteIndex0] = 'B';
        bmp_hdr[kByteIndex1] = 'M';
        write_u32_le(bmp_hdr.data() + kByteIndex2, static_cast<std::uint32_t>(bmp_total_bytes));
        write_u32_le(bmp_hdr.data() + kBmpOffBitsOffset, static_cast<std::uint32_t>(kBmpFileHeaderSize));
        write_u32_le(bmp_hdr.data() + kBmpBiSizeOffset, static_cast<std::uint32_t>(kBmpInfoHeaderSize));
        write_u32_le(bmp_hdr.data() + kBmpWidthOffset, static_cast<std::uint32_t>(img.width));
        write_u32_le(bmp_hdr.data() + kBmpHeightOffset, static_cast<std::uint32_t>(img.height));
        write_u16_le(bmp_hdr.data() + kBmpPlanesOffset, kBmpPlanes);
        write_u16_le(bmp_hdr.data() + kBmpBitCountOffset, kBmpBpp24);
        write_u32_le(bmp_hdr.data() + kBmpSizeImageOffset, static_cast<std::uint32_t>(bmp_px_bytes));
        dwq.insert(dwq.end(), bmp_hdr.begin(), bmp_hdr.end());
        dwq.resize(dwq.size() + bmp_px_bytes, 0);

        if (with_mask) {
            const std::size_t total_px = static_cast<std::size_t>(img.width) * img.height;
            std::vector<std::uint8_t> mask_hdr(kMaskHeaderTotalBytes, 0);
            mask_hdr[kByteIndex0] = 'B';
            mask_hdr[kByteIndex1] = 'M';
            write_u32_le(mask_hdr.data() + kByteIndex2,
                         static_cast<std::uint32_t>(kMaskHeaderTotalBytes + total_px));
            write_u32_le(mask_hdr.data() + kBmpOffBitsOffset,
                         static_cast<std::uint32_t>(kMaskHeaderTotalBytes));
            write_u32_le(mask_hdr.data() + kBmpBiSizeOffset,
                         static_cast<std::uint32_t>(kBmpInfoHeaderSize));
            write_u32_le(mask_hdr.data() + kBmpWidthOffset, static_cast<std::uint32_t>(img.width));
            write_u32_le(mask_hdr.data() + kBmpHeightOffset, static_cast<std::uint32_t>(img.height));
            write_u16_le(mask_hdr.data() + kBmpPlanesOffset, kBmpPlanes);
            write_u16_le(mask_hdr.data() + kBmpBitCountOffset, kBmpBpp8);
            write_u32_le(mask_hdr.data() + kBmpSizeImageOffset, static_cast<std::uint32_t>(total_px));
            write_u32_le(mask_hdr.data() + kBmpClrUsedOffset, kMaskPaletteCount);
            for (std::uint32_t i = 0; i < kMaskPaletteCount; ++i) {
                const std::size_t pal_off = kBmpFileHeaderSize + i * kRgbaChannels;
                mask_hdr[pal_off + kByteIndex0] = static_cast<std::uint8_t>(i);
                mask_hdr[pal_off + kByteIndex1] = static_cast<std::uint8_t>(i);
                mask_hdr[pal_off + kByteIndex2] = static_cast<std::uint8_t>(i);
            }
            const std::vector<std::uint8_t> zero_alpha(total_px, 0);
            const auto dummy_mask =
                encode_rle_xor_mask(zero_alpha.data(), img.width, img.height);
            dwq.insert(dwq.end(), mask_hdr.begin(), mask_hdr.end());
            dwq.insert(dwq.end(), dummy_mask.begin(), dummy_mask.end());
        }
        return dwq;
    };

    auto hdr0 = make_bm_dwq(false);
    auto enc0 = encode_dwq_from_rgba(hdr0.data(), hdr0.size(), img, true);
    auto dec0 = decode_dwq_to_rgba(enc0.data(), enc0.size());
    for (std::size_t i = 0; i < static_cast<std::size_t>(img.width) * img.height; ++i) {
        EXPECT_EQ(dec0.pixels[i * kRgbaChannels + kRgbaRedIndex],
                  img.pixels[i * kRgbaChannels + kRgbaRedIndex]);
        EXPECT_EQ(dec0.pixels[i * kRgbaChannels + kRgbaGreenIndex],
                  img.pixels[i * kRgbaChannels + kRgbaGreenIndex]);
        EXPECT_EQ(dec0.pixels[i * kRgbaChannels + kRgbaBlueIndex],
                  img.pixels[i * kRgbaChannels + kRgbaBlueIndex]);
        EXPECT_EQ(dec0.pixels[i * kRgbaChannels + kRgbaAlphaIndex], kOpaqueAlpha);
    }

    auto hdr2a = make_bm_dwq(true);
    auto enc2a = encode_dwq_from_rgba(hdr2a.data(), hdr2a.size(), img, true);
    auto dec2a = decode_dwq_to_rgba(enc2a.data(), enc2a.size());
    EXPECT_EQ(dec2a.pixels, img.pixels);
}

TEST(MenuPatcherTest, PatchMainControlAndBacklogFxf) {
    const std::string src_main =
        exm::utf8_to_cp932("name mainControl\r\n"
                           "windowTitle EXTRAVAGANZA～末路の果てに～_CE版\r\n"
                           "windowTitleTaiken EXTRAVAGANZA～末路の果てに～体験版\r\n"
                           "bppErrorMessage 16bit以上\r\n");
    std::string patched_main = patch_main_control_fxf(src_main);
    EXPECT_NE(patched_main.find("windowTitle EXTRAVAGANZA ~Matsuro~ CE\r\n"),
              std::string::npos);
    EXPECT_NE(patched_main.find("windowTitleTaiken EXTRAVAGANZA ~Matsuro~ CE Trial\r\n"),
              std::string::npos);
    EXPECT_NE(patched_main.find(
                  "bppErrorMessage Please set display to 16-bit color or higher.\r\n"),
              std::string::npos);

    const std::string src_backlog =
        exm::utf8_to_cp932("modename backlog\r\n"
                           "firstMessage1 \t#苺　　　　　　　　メッセージバックログ\r\n");
    std::string patched_backlog = patch_backlog_fxf(src_backlog);

    const std::string cp932_space = exm::utf8_to_cp932("　");
    std::string expected_spaces;
    for (std::size_t i = 0; i < kBacklogLeadingFullwidthSpaces; ++i) {
        expected_spaces.append(cp932_space);
    }
    const std::string expected_line =
        exm::utf8_to_cp932("firstMessage1 \t#苺") + expected_spaces +
        exm::utf8_to_cp932("Ｂａｃｋｌｏｇ\r\n");
    EXPECT_NE(patched_backlog.find(expected_line), std::string::npos);
}

TEST(MenuPatcherTest, MenuArchivePatcherSyntheticGpkGtbRoundTrip) {
    RgbaImage img1;
    img1.width = kSyntheticSpriteDim;
    img1.height = kSyntheticSpriteDim;
    img1.bpp = kBpp32;
    img1.has_alpha = true;
    img1.pixels.assign(
        static_cast<std::size_t>(img1.width) * img1.height * kRgbaChannels, kSyntheticInitFill);

    auto png1 = encode_rgba_to_png_bytes(img1, true);
    auto dwq1 = make_dwq_header(img1.width, img1.height, kBpp32, "PNG             ",
                                "    PACKTYPE=8A     ",
                                static_cast<std::uint32_t>(png1.size()));
    dwq1.insert(dwq1.end(), png1.begin(), png1.end());

    auto dwq2 = dwq1;

    const ScopedTempDir tmp("matsuro_ut_menu_patcher");
    const fs::path& tmp_dir = tmp.path();
    fs::path gpk_path = tmp_dir / "test.gpk";
    fs::path gtb_path = tmp_dir / "test.gtb";

    std::vector<std::uint8_t> gpk_in;
    gpk_in.insert(gpk_in.end(), dwq1.begin(), dwq1.end());
    gpk_in.insert(gpk_in.end(), dwq2.begin(), dwq2.end());
    exm::write_file(gpk_path.u8string(), gpk_in);

    constexpr std::size_t kSyntheticGtbTotalSize =
        kSyntheticGtbCountBytes + kSyntheticGtbTableEntries * sizeof(std::uint32_t) +
        kSyntheticGtbNamesBlobBytes;
    std::vector<std::uint8_t> gtb_in(kSyntheticGtbTotalSize, 0);
    write_u32_le(gtb_in.data(), kSyntheticEntryCount);
    write_u32_le(gtb_in.data() + kSyntheticGtbNameOffset0Pos, kSyntheticName0Offset);
    write_u32_le(gtb_in.data() + kSyntheticGtbNameOffset1Pos, kSyntheticName1Offset);
    write_u32_le(gtb_in.data() + kSyntheticGtbDataOffset0Pos, 0);
    write_u32_le(gtb_in.data() + kSyntheticGtbDataOffset1Pos,
                 static_cast<std::uint32_t>(dwq1.size()));
    std::memcpy(gtb_in.data() + kSyntheticGtbNamesBlobPos, "sprite_one",
                kSyntheticSpriteNameLen);
    std::memcpy(gtb_in.data() + kSyntheticGtbSecondNamePos, "sprite_two",
                kSyntheticSpriteNameLen);
    exm::write_file(gtb_path.u8string(), gtb_in);

    MenuPatcher patcher;
    patcher.load(gpk_path, gtb_path);
    ASSERT_EQ(patcher.entry_count(), kSyntheticEntryCount);
    EXPECT_EQ(patcher.entry_name(kSyntheticEntryIndex0), "sprite_one");
    EXPECT_EQ(patcher.entry_name(kSyntheticEntryIndex1), "sprite_two");

    for (std::size_t i = 0; i < img1.pixels.size(); ++i) {
        img1.pixels[i] = static_cast<std::uint8_t>(
            (i * kSyntheticPatternMultiplier + kSyntheticPatternOffset) & kByteMask);
    }
    patcher.replace_entry_rgba(kSyntheticEntryIndex0, img1, true);

    fs::path out_gpk = tmp_dir / "out.gpk";
    fs::path out_gtb = tmp_dir / "out.gtb";
    patcher.save(out_gpk, out_gtb);

    MenuArchivePatcher reloaded;
    reloaded.load(out_gpk, out_gtb);
    ASSERT_EQ(reloaded.entry_count(), kSyntheticEntryCount);
    EXPECT_EQ(reloaded.entry_offset(kSyntheticEntryIndex0), 0u);
    EXPECT_EQ(reloaded.entry_offset(kSyntheticEntryIndex1),
              reloaded.entry_size(kSyntheticEntryIndex0));
    EXPECT_EQ(reloaded.entry_offset(kSyntheticEntryEndIndex),
              static_cast<std::uint32_t>(fs::file_size(out_gpk)));
    EXPECT_EQ(reloaded.decode_entry_rgba(kSyntheticEntryIndex0).pixels, img1.pixels);
}

}  // namespace
