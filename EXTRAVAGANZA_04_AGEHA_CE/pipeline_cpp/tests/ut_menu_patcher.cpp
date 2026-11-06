// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for pipeline_cpp/src/menu/menu_patcher.{h,cpp}: FXF bitwise-NOT
// codec, MurmurHash2 + XOR-delta masking, DWQ crop margins and PACKTYPE=0/2A/8A
// round-trips, GPK/GTB rebuilding, and AGHMENU1 v1 menu_patch.bin verification.
#include "menu/menu_patcher.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
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
using exm::menu::decrypt_fxf;
using exm::menu::encode_dwq_from_png_bytes;
using exm::menu::encode_dwq_from_rgba;
using exm::menu::encode_rgba_to_png_bytes;
using exm::menu::encrypt_fxf;
using exm::menu::kExpectedModifiedGpkEntryCount;
using exm::menu::kExpectedStandaloneFileCount;
using exm::menu::kPatchFormatVersion;
using exm::menu::kPatchMagic;
using exm::menu::kPatchMagicSize;
using exm::menu::parse_patch_bin;
using exm::menu::patch_backlog_fxf;
using exm::menu::patch_main_control_fxf;

constexpr size_t kDwqHeaderSize = 64;
constexpr size_t kDwqTypeStrFieldLen = 16;
constexpr size_t kDwqCropMarginsOffset = 0x10;
constexpr size_t kDwqDataSizeOffset = 0x20;
constexpr size_t kDwqWidthOffset = 0x24;
constexpr size_t kDwqHeightOffset = 0x28;
constexpr size_t kDwqBppOffset = 0x2A;
constexpr size_t kDwqPackStrOffset = 0x2C;
constexpr size_t kDwqPackStrFieldLen = 20;
constexpr size_t kLzssSubHeaderSize = 0x44;
constexpr size_t kLzssUncompSizeOffset = 0x10;

constexpr size_t kCropTopIndex = 0;
constexpr size_t kCropLeftIndex = 1;
constexpr size_t kCropRightIndex = 2;
constexpr size_t kCropBottomIndex = 3;
constexpr size_t kCropTopByteOffset = 0;
constexpr size_t kCropLeftByteOffset = 4;
constexpr size_t kCropRightByteOffset = 8;
constexpr size_t kCropBottomByteOffset = 12;

constexpr uint32_t kByteShift1 = 8;
constexpr uint32_t kByteShift2 = 16;
constexpr uint32_t kByteShift3 = 24;
constexpr size_t kByteIndex0 = 0;
constexpr size_t kByteIndex1 = 1;
constexpr size_t kByteIndex2 = 2;
constexpr size_t kByteIndex3 = 3;

constexpr size_t kRgbaChannels = 4;
constexpr size_t kRgbChannels = 3;
constexpr size_t kRgbaRedIndex = 0;
constexpr size_t kRgbaGreenIndex = 1;
constexpr size_t kRgbaBlueIndex = 2;
constexpr size_t kRgbaAlphaIndex = 3;
constexpr uint16_t kBpp24 = 24;
constexpr uint16_t kBpp32 = 32;
constexpr uint8_t kOpaqueAlpha = 255;
constexpr uint8_t kByteMask = 0xFF;

constexpr uint32_t kMurmurDefaultSeed = 0x9747B28Cu;
constexpr uint8_t kSampleByte0 = 0x10;
constexpr uint8_t kSampleByte1 = 0x20;
constexpr uint8_t kSampleByte2 = 0x30;
constexpr uint8_t kSampleByte3 = 0x40;
constexpr uint8_t kSampleByte4 = 0x55;
constexpr uint8_t kSampleByte5 = 0xAA;
constexpr uint8_t kSampleByte6 = 0xFE;

constexpr uint16_t kCropTestWidth = 10;
constexpr uint16_t kCropTestHeight = 8;
constexpr int kCropActiveMinY = 1;
constexpr int kCropActiveMaxY = 5;
constexpr int kCropActiveMinX = 2;
constexpr int kCropActiveMaxX = 6;
constexpr int32_t kExpectedCropTop = 1;
constexpr int32_t kExpectedCropLeft = 2;
constexpr int32_t kExpectedCropRight = 3;
constexpr int32_t kExpectedCropBottom = 2;
constexpr uint8_t kCropSampleRed = 100;
constexpr uint8_t kCropSampleGreen = 150;
constexpr uint8_t kCropSampleBlue = 200;
constexpr uint8_t kCropSampleAlpha = 220;

constexpr uint16_t kPack8aRgbaWidth = 12;
constexpr uint16_t kPack8aRgbaHeight = 10;
constexpr int kPack8aActiveMinY = 2;
constexpr int kPack8aActiveMaxY = 7;
constexpr int kPack8aActiveMinX = 3;
constexpr int kPack8aActiveMaxX = 8;
constexpr int32_t kPack8aExpectedMarginTop = 2;
constexpr int32_t kPack8aExpectedMarginLeft = 3;
constexpr int32_t kPack8aExpectedMarginRight = 3;
constexpr int32_t kPack8aExpectedMarginBottom = 2;
constexpr int kPack8aRedStep = 20;
constexpr int kPack8aGreenStep = 25;
constexpr uint8_t kPack8aBlueVal = 180;
constexpr uint8_t kPack8aAlphaVal = 240;

constexpr uint16_t kPack8aRgbWidth = 8;
constexpr uint16_t kPack8aRgbHeight = 6;
constexpr size_t kRgbPatternPrimeR = 7;
constexpr size_t kRgbPatternPrimeG = 13;
constexpr size_t kRgbPatternPrimeB = 29;

constexpr uint16_t kPack0And2aWidth = 6;
constexpr uint16_t kPack0And2aHeight = 4;
constexpr size_t kPack0BaseRed = 10;
constexpr size_t kPack0BaseGreen = 40;
constexpr size_t kPack0BaseBlue = 80;
constexpr size_t kPack0BaseAlpha = 100;
constexpr size_t kPack0GreenMultiplier = 2;
constexpr size_t kPack0BlueMultiplier = 3;
constexpr size_t kPack0AlphaMultiplier = 4;

constexpr size_t kBacklogLeadingFullwidthSpaces = 8;

constexpr uint16_t kSyntheticSpriteDim = 4;
constexpr uint8_t kSyntheticInitFill = 120;
constexpr size_t kSyntheticPatternMultiplier = 37;
constexpr size_t kSyntheticPatternOffset = 19;
constexpr uint32_t kSyntheticEntryCount = 2;
constexpr size_t kSyntheticEntryIndex0 = 0;
constexpr size_t kSyntheticEntryIndex1 = 1;
constexpr size_t kSyntheticEntryEndIndex = 2;
constexpr size_t kSyntheticGtbCountBytes = 4;
constexpr size_t kSyntheticGtbTableEntries = 4;
constexpr size_t kSyntheticGtbNamesBlobBytes = 24;
constexpr size_t kSyntheticGtbNameOffset0Pos = 4;
constexpr size_t kSyntheticGtbNameOffset1Pos = 8;
constexpr size_t kSyntheticGtbDataOffset0Pos = 12;
constexpr size_t kSyntheticGtbDataOffset1Pos = 16;
constexpr size_t kSyntheticGtbNamesBlobPos = 20;
constexpr size_t kSyntheticGtbSecondNamePos = 32;
constexpr uint32_t kSyntheticName0Offset = 0;
constexpr uint32_t kSyntheticName1Offset = 12;
constexpr size_t kSyntheticSpriteNameLen = 10;

constexpr size_t kExpectedSys0EntryCount = 83;
constexpr size_t kExpectedTranslatedEntryCount = 21;

fs::path ResolveLiveGameDir() {
    const char* env_dir = std::getenv("VN_GAME_DIR");
    return (env_dir && *env_dir) ? fs::u8path(env_dir) : fs::path();
}

uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[kByteIndex0]) |
           (static_cast<uint32_t>(p[kByteIndex1]) << kByteShift1) |
           (static_cast<uint32_t>(p[kByteIndex2]) << kByteShift2) |
           (static_cast<uint32_t>(p[kByteIndex3]) << kByteShift3);
}

int32_t read_i32_le(const uint8_t* p) {
    return static_cast<int32_t>(read_u32_le(p));
}

void write_u16_le(uint8_t* p, uint16_t v) {
    p[kByteIndex0] = static_cast<uint8_t>(v & kByteMask);
    p[kByteIndex1] = static_cast<uint8_t>((v >> kByteShift1) & kByteMask);
}

void write_u32_le(uint8_t* p, uint32_t v) {
    p[kByteIndex0] = static_cast<uint8_t>(v & kByteMask);
    p[kByteIndex1] = static_cast<uint8_t>((v >> kByteShift1) & kByteMask);
    p[kByteIndex2] = static_cast<uint8_t>((v >> kByteShift2) & kByteMask);
    p[kByteIndex3] = static_cast<uint8_t>((v >> kByteShift3) & kByteMask);
}

std::vector<uint8_t> make_dwq_header(uint16_t w, uint16_t h, uint16_t bpp,
                                     const char* type_str16,
                                     const char* pack_str20,
                                     uint32_t data_size) {
    std::vector<uint8_t> hdr(kDwqHeaderSize, 0);
    std::memcpy(hdr.data(), type_str16, kDwqTypeStrFieldLen);
    write_u32_le(hdr.data() + kDwqDataSizeOffset, data_size);
    write_u16_le(hdr.data() + kDwqWidthOffset, w);
    write_u16_le(hdr.data() + kDwqHeightOffset, h);
    write_u16_le(hdr.data() + kDwqBppOffset, bpp);
    std::memcpy(hdr.data() + kDwqPackStrOffset, pack_str20, kDwqPackStrFieldLen);
    return hdr;
}

TEST(MenuPatcherTest, FxfBitwiseNotRoundTrip) {
    const std::string sample = "name mainControl\r\nwindowTitle AGEHA\r\n";
    auto enc = encrypt_fxf(sample);
    ASSERT_EQ(enc.size(), sample.size());
    for (size_t i = 0; i < sample.size(); ++i) {
        EXPECT_EQ(enc[i], static_cast<uint8_t>(~static_cast<uint8_t>(sample[i])));
    }
    auto dec = decrypt_fxf(enc);
    EXPECT_EQ(dec, sample);
}

TEST(MenuPatcherTest, MurmurHash2AndXorMaskRoundTrip) {
    const std::vector<uint8_t> original = {
        kSampleByte0, kSampleByte1, kSampleByte2, kSampleByte3,
        kSampleByte4, kSampleByte5, kSampleByte6,
    };
    std::vector<uint8_t> buf = original;
    uint32_t key = ComputeMurmurHash2(original.data(), original.size(), kMurmurDefaultSeed);
    EXPECT_NE(key, 0u);
    XorMaskPayload(buf.data(), buf.size(), key);
    EXPECT_NE(buf, original);
    XorMaskPayload(buf.data(), buf.size(), key);
    EXPECT_EQ(buf, original);

    const std::vector<uint8_t> patched = {
        kSampleByte6, kSampleByte5, kSampleByte4, kSampleByte3,
        kSampleByte2, kSampleByte1, kSampleByte0, kSampleByte4,
    };
    std::vector<uint8_t> delta_buf = patched;
    XorMaskPayload(delta_buf.data(), delta_buf.size(), original.data(), original.size(), key);
    EXPECT_NE(delta_buf, patched);
    XorMaskPayload(delta_buf.data(), delta_buf.size(), original.data(), original.size(), key);
    EXPECT_EQ(delta_buf, patched);
}

TEST(MenuPatcherTest, ComputeCropMargins32BppRgba) {
    RgbaImage img;
    img.width = kCropTestWidth;
    img.height = kCropTestHeight;
    img.bpp = kBpp32;
    img.has_alpha = true;
    img.pixels.assign(static_cast<size_t>(img.width) * img.height * kRgbaChannels, 0);

    for (int y = kCropActiveMinY; y <= kCropActiveMaxY; ++y) {
        for (int x = kCropActiveMinX; x <= kCropActiveMaxX; ++x) {
            size_t idx = (static_cast<size_t>(y) * img.width + x) * kRgbaChannels;
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
        static_cast<size_t>(src_rgba.width) * src_rgba.height * kRgbaChannels, 0);

    for (int y = kPack8aActiveMinY; y <= kPack8aActiveMaxY; ++y) {
        for (int x = kPack8aActiveMinX; x <= kPack8aActiveMaxX; ++x) {
            size_t idx = (static_cast<size_t>(y) * src_rgba.width + x) * kRgbaChannels;
            src_rgba.pixels[idx + kRgbaRedIndex] = static_cast<uint8_t>(x * kPack8aRedStep);
            src_rgba.pixels[idx + kRgbaGreenIndex] =
                static_cast<uint8_t>(y * kPack8aGreenStep);
            src_rgba.pixels[idx + kRgbaBlueIndex] = kPack8aBlueVal;
            src_rgba.pixels[idx + kRgbaAlphaIndex] = kPack8aAlphaVal;
        }
    }

    auto png_rgba = encode_rgba_to_png_bytes(src_rgba, true);
    auto orig_dwq = make_dwq_header(src_rgba.width, src_rgba.height, kBpp32,
                                    "PNG             ", "    PACKTYPE=8A     ",
                                    static_cast<uint32_t>(png_rgba.size()));
    orig_dwq.insert(orig_dwq.end(), png_rgba.begin(), png_rgba.end());

    auto decoded = decode_dwq_to_rgba(orig_dwq.data(), orig_dwq.size());
    EXPECT_EQ(decoded.width, src_rgba.width);
    EXPECT_EQ(decoded.height, src_rgba.height);
    EXPECT_EQ(decoded.pixels, src_rgba.pixels);

    auto reencoded = encode_dwq_from_rgba(orig_dwq.data(), orig_dwq.size(), decoded, true);
    uint32_t new_data_size = read_u32_le(reencoded.data() + kDwqDataSizeOffset);
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
              static_cast<uint32_t>(png_rgba.size()));
    EXPECT_EQ(decode_dwq_to_rgba(from_png.data(), from_png.size()).pixels, src_rgba.pixels);

    RgbaImage src_rgb;
    src_rgb.width = kPack8aRgbWidth;
    src_rgb.height = kPack8aRgbHeight;
    src_rgb.bpp = kBpp24;
    src_rgb.has_alpha = false;
    src_rgb.pixels.resize(
        static_cast<size_t>(src_rgb.width) * src_rgb.height * kRgbaChannels);
    for (size_t i = 0; i < static_cast<size_t>(src_rgb.width) * src_rgb.height; ++i) {
        src_rgb.pixels[i * kRgbaChannels + kRgbaRedIndex] =
            static_cast<uint8_t>((i * kRgbPatternPrimeR) & kByteMask);
        src_rgb.pixels[i * kRgbaChannels + kRgbaGreenIndex] =
            static_cast<uint8_t>((i * kRgbPatternPrimeG) & kByteMask);
        src_rgb.pixels[i * kRgbaChannels + kRgbaBlueIndex] =
            static_cast<uint8_t>((i * kRgbPatternPrimeB) & kByteMask);
        src_rgb.pixels[i * kRgbaChannels + kRgbaAlphaIndex] = kOpaqueAlpha;
    }
    auto png_rgb = encode_rgba_to_png_bytes(src_rgb, false);
    auto orig_dwq_rgb = make_dwq_header(src_rgb.width, src_rgb.height, kBpp24,
                                        "PNG             ", "    PACKTYPE=8A     ",
                                        static_cast<uint32_t>(png_rgb.size()));
    orig_dwq_rgb.insert(orig_dwq_rgb.end(), png_rgb.begin(), png_rgb.end());

    auto reencoded_rgb =
        encode_dwq_from_rgba(orig_dwq_rgb.data(), orig_dwq_rgb.size(), src_rgb, true);
    auto decoded_rgb = decode_dwq_to_rgba(reencoded_rgb.data(), reencoded_rgb.size());
    EXPECT_EQ(decoded_rgb.pixels, src_rgb.pixels);
}

TEST(MenuPatcherTest, DwqPackType0And2ARoundTrip) {
    RgbaImage img32;
    img32.width = kPack0And2aWidth;
    img32.height = kPack0And2aHeight;
    img32.bpp = kBpp32;
    img32.has_alpha = true;
    img32.pixels.resize(
        static_cast<size_t>(img32.width) * img32.height * kRgbaChannels, 0);
    for (size_t i = 0; i < static_cast<size_t>(img32.width) * img32.height; ++i) {
        img32.pixels[i * kRgbaChannels + kRgbaRedIndex] =
            static_cast<uint8_t>(kPack0BaseRed + i);
        img32.pixels[i * kRgbaChannels + kRgbaGreenIndex] =
            static_cast<uint8_t>(kPack0BaseGreen + i * kPack0GreenMultiplier);
        img32.pixels[i * kRgbaChannels + kRgbaBlueIndex] =
            static_cast<uint8_t>(kPack0BaseBlue + i * kPack0BlueMultiplier);
        img32.pixels[i * kRgbaChannels + kRgbaAlphaIndex] =
            static_cast<uint8_t>(kPack0BaseAlpha + i * kPack0AlphaMultiplier);
    }

    uint32_t raw32_size = static_cast<uint32_t>(img32.pixels.size());
    auto hdr0 = make_dwq_header(img32.width, img32.height, kBpp32, "BMP 32          ",
                                "    PACKTYPE=0A     ", raw32_size);
    hdr0.resize(kDwqHeaderSize + raw32_size, 0);

    auto enc0 = encode_dwq_from_rgba(hdr0.data(), hdr0.size(), img32, true);
    auto dec0 = decode_dwq_to_rgba(enc0.data(), enc0.size());
    EXPECT_EQ(dec0.pixels, img32.pixels);

    uint32_t bgr_size = static_cast<uint32_t>(img32.width) * img32.height * kRgbChannels;
    uint32_t alpha_size = static_cast<uint32_t>(img32.width) * img32.height;
    auto hdr2a = make_dwq_header(img32.width, img32.height, kBpp24, "BMP 24          ",
                                 "    PACKTYPE=2A     ",
                                 static_cast<uint32_t>(kLzssSubHeaderSize));
    hdr2a.resize(kDwqHeaderSize + kLzssSubHeaderSize + alpha_size, 0);
    std::memcpy(hdr2a.data() + kDwqHeaderSize, "BMP 24          ", kDwqTypeStrFieldLen);
    write_u32_le(hdr2a.data() + kDwqHeaderSize + kLzssUncompSizeOffset, bgr_size);

    auto enc2a = encode_dwq_from_rgba(hdr2a.data(), hdr2a.size(), img32, false);
    auto dec2a = decode_dwq_to_rgba(enc2a.data(), enc2a.size());
    EXPECT_EQ(dec2a.pixels, img32.pixels);
}

TEST(MenuPatcherTest, PatchMainControlAndBacklogFxf) {
    const std::string src_main =
        exm::utf8_to_cp932("name mainControl\r\n"
                           "windowTitle アゲハ\r\n"
                           "windowTitleTaiken アゲハ体験版\r\n"
                           "bppErrorMessage 16bit以上\r\n");
    std::string patched_main = patch_main_control_fxf(src_main);
    EXPECT_NE(patched_main.find("windowTitle AGEHA ~EXTRAVAGANZA~ CE\r\n"),
              std::string::npos);
    EXPECT_NE(patched_main.find("windowTitleTaiken AGEHA ~EXTRAVAGANZA~ CE Trial\r\n"),
              std::string::npos);
    EXPECT_NE(patched_main.find(
                  "bppErrorMessage Please set display to 16-bit color or higher.\r\n"),
              std::string::npos);

    const std::string src_backlog =
        exm::utf8_to_cp932("modename backlog\r\n"
                           "firstMessage1 \t#苺　ユメミルアゲハチョウ\r\n");
    std::string patched_backlog = patch_backlog_fxf(src_backlog);

    const std::string cp932_space = exm::utf8_to_cp932("　");
    std::string expected_spaces;
    for (size_t i = 0; i < kBacklogLeadingFullwidthSpaces; ++i) {
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
        static_cast<size_t>(img1.width) * img1.height * kRgbaChannels, kSyntheticInitFill);

    auto png1 = encode_rgba_to_png_bytes(img1, true);
    auto dwq1 = make_dwq_header(img1.width, img1.height, kBpp32, "PNG             ",
                                "    PACKTYPE=8A     ",
                                static_cast<uint32_t>(png1.size()));
    dwq1.insert(dwq1.end(), png1.begin(), png1.end());

    auto dwq2 = dwq1;

    fs::path tmp_dir = fs::temp_directory_path() / "ageha_ut_menu_patcher";
    fs::create_directories(tmp_dir);
    fs::path gpk_path = tmp_dir / "test.gpk";
    fs::path gtb_path = tmp_dir / "test.gtb";

    std::vector<uint8_t> gpk_in;
    gpk_in.insert(gpk_in.end(), dwq1.begin(), dwq1.end());
    gpk_in.insert(gpk_in.end(), dwq2.begin(), dwq2.end());
    exm::write_file(gpk_path.string(), gpk_in);

    constexpr size_t kSyntheticGtbTotalSize =
        kSyntheticGtbCountBytes + kSyntheticGtbTableEntries * sizeof(uint32_t) +
        kSyntheticGtbNamesBlobBytes;
    std::vector<uint8_t> gtb_in(kSyntheticGtbTotalSize, 0);
    write_u32_le(gtb_in.data(), kSyntheticEntryCount);
    write_u32_le(gtb_in.data() + kSyntheticGtbNameOffset0Pos, kSyntheticName0Offset);
    write_u32_le(gtb_in.data() + kSyntheticGtbNameOffset1Pos, kSyntheticName1Offset);
    write_u32_le(gtb_in.data() + kSyntheticGtbDataOffset0Pos, 0);
    write_u32_le(gtb_in.data() + kSyntheticGtbDataOffset1Pos,
                 static_cast<uint32_t>(dwq1.size()));
    std::memcpy(gtb_in.data() + kSyntheticGtbNamesBlobPos, "sprite_one",
                kSyntheticSpriteNameLen);
    std::memcpy(gtb_in.data() + kSyntheticGtbSecondNamePos, "sprite_two",
                kSyntheticSpriteNameLen);
    exm::write_file(gtb_path.string(), gtb_in);

    MenuPatcher patcher;
    patcher.load(gpk_path, gtb_path);
    ASSERT_EQ(patcher.entry_count(), kSyntheticEntryCount);
    EXPECT_EQ(patcher.entry_name(kSyntheticEntryIndex0), "sprite_one");
    EXPECT_EQ(patcher.entry_name(kSyntheticEntryIndex1), "sprite_two");

    for (size_t i = 0; i < img1.pixels.size(); ++i) {
        img1.pixels[i] = static_cast<uint8_t>(
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
              static_cast<uint32_t>(fs::file_size(out_gpk)));
    EXPECT_EQ(reloaded.decode_entry_rgba(kSyntheticEntryIndex0).pixels, img1.pixels);

    std::error_code ec;
    fs::remove_all(tmp_dir, ec);
}

TEST(MenuPatcherTest, LiveSys0GpkAndFxfVerificationWhenGameDirPresent) {
    const fs::path game_dir = ResolveLiveGameDir();
    const fs::path gpk_path = game_dir / "dwq" / "sys0.gpk";
    const fs::path gtb_path = game_dir / "dwq" / "sys0.gtb";
    if (game_dir.empty() || !fs::exists(gpk_path) || !fs::exists(gtb_path)) {
        GTEST_SKIP() << "Live AGEHA_CE game directory not present on this machine.";
    }

    MenuArchivePatcher patcher;
    patcher.load(gpk_path, gtb_path);
    ASSERT_EQ(patcher.entry_count(), kExpectedSys0EntryCount);
    EXPECT_EQ(patcher.entry_offset(kExpectedSys0EntryCount),
              static_cast<uint32_t>(fs::file_size(gpk_path)));

    const std::array<size_t, kExpectedTranslatedEntryCount> kTranslatedIndices = {
        2,  3,  5,  39, 40, 41, 42, 47, 48, 49, 50,
        56, 65, 66, 70, 71, 78, 79, 80, 81, 82,
    };
    for (size_t idx : kTranslatedIndices) {
        const auto& raw = patcher.entry_raw_bytes(idx);
        ASSERT_GT(raw.size(), kDwqHeaderSize);
        uint32_t dsize = read_u32_le(raw.data() + kDwqDataSizeOffset);
        EXPECT_EQ(kDwqHeaderSize + dsize, raw.size()) << "entry index=" << idx;
        auto img = patcher.decode_entry_rgba(idx);
        EXPECT_GT(img.width, 0u);
        EXPECT_GT(img.height, 0u);
    }

    const fs::path main_fxf_path = game_dir / "init2" / "mainControl.fxf";
    if (fs::exists(main_fxf_path)) {
        auto dec_main = decrypt_fxf(exm::read_file(main_fxf_path.string()));
        EXPECT_NE(dec_main.find("windowTitle AGEHA ~EXTRAVAGANZA~ CE"),
                  std::string::npos);
        EXPECT_NE(dec_main.find("windowTitleTaiken AGEHA ~EXTRAVAGANZA~ CE Trial"),
                  std::string::npos);
        EXPECT_NE(dec_main.find("bppErrorMessage Please set display to 16-bit color or higher."),
                  std::string::npos);
    }

    const fs::path backlog_fxf_path = game_dir / "init2" / "backlog.fxf";
    if (fs::exists(backlog_fxf_path)) {
        auto dec_backlog = decrypt_fxf(exm::read_file(backlog_fxf_path.string()));
        const std::string expected_cp932_backlog = exm::utf8_to_cp932("Ｂａｃｋｌｏｇ");
        EXPECT_NE(dec_backlog.find(expected_cp932_backlog), std::string::npos);
    }
}

bool ContainsAsciiSubsequence(const std::vector<uint8_t>& haystack,
                              const uint8_t* needle,
                              size_t needle_len) {
    auto it = std::search(haystack.begin(), haystack.end(), needle, needle + needle_len);
    return it != haystack.end();
}

fs::path FindCommittedPatchBin() {
    fs::path cur = fs::current_path();
    constexpr int kMaxWalkLevels = 6;
    for (int i = 0; i < kMaxWalkLevels; ++i) {
        if (fs::exists(cur / "menu_patch.bin")) {
            return cur / "menu_patch.bin";
        }
        if (fs::exists(cur / "EXTRAVAGANZA_04_AGEHA_CE" / "menu_patch.bin")) {
            return cur / "EXTRAVAGANZA_04_AGEHA_CE" / "menu_patch.bin";
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) {
            break;
        }
        cur = cur.parent_path();
    }
    return {};
}

TEST(MenuPatcherTest, CommittedMenuPatchBinHeaderAndAbsenceOfRawHeaders) {
    const fs::path patch_bin_path = FindCommittedPatchBin();
    if (patch_bin_path.empty()) {
        GTEST_SKIP() << "EXTRAVAGANZA_04_AGEHA_CE/menu_patch.bin not generated yet.";
    }
    const auto bytes = exm::read_file(patch_bin_path.string());
    ASSERT_GT(bytes.size(), kPatchMagicSize);
    EXPECT_EQ(std::memcmp(bytes.data(), kPatchMagic, kPatchMagicSize), 0);

    const auto parsed = parse_patch_bin(bytes);
    EXPECT_EQ(parsed.version, kPatchFormatVersion);
    EXPECT_EQ(parsed.standalone_records.size(), kExpectedStandaloneFileCount);
    EXPECT_EQ(parsed.gpk_records.size(), kExpectedModifiedGpkEntryCount);

    constexpr uint8_t kPackBmpNeedle[] = {'P', 'A', 'C', 'K', 'B', 'M', 'P'};
    constexpr uint8_t kPngNeedle[] = {0x89, 'P', 'N', 'G'};
    EXPECT_FALSE(ContainsAsciiSubsequence(bytes, kPackBmpNeedle, sizeof(kPackBmpNeedle)));
    EXPECT_FALSE(ContainsAsciiSubsequence(bytes, kPngNeedle, sizeof(kPngNeedle)));
}

TEST(MenuPatcherTest, CommittedMenuPatchBinReconstructsLiveArchiveBitForBit) {
    const fs::path game_dir = ResolveLiveGameDir();
    if (game_dir.empty() || !fs::exists(game_dir / "dwq" / "sys0.gpk") ||
        !fs::exists(game_dir / "dwq" / "sys0.gtb")) {
        GTEST_SKIP() << "Live AGEHA_CE game directory not present on this machine.";
    }
    const fs::path patch_bin_path = FindCommittedPatchBin();
    if (patch_bin_path.empty()) {
        GTEST_SKIP() << "EXTRAVAGANZA_04_AGEHA_CE/menu_patch.bin not generated yet.";
    }

    const fs::path orig_gpk_path =
        fs::exists(game_dir / "dwq_backup" / "sys0.gpk")
            ? (game_dir / "dwq_backup" / "sys0.gpk")
            : (fs::exists(game_dir / "dwq" / "sys0.gpk.bak")
                   ? (game_dir / "dwq" / "sys0.gpk.bak")
                   : (game_dir / "dwq" / "sys0.gpk"));
    const fs::path orig_gtb_path =
        fs::exists(game_dir / "dwq_backup" / "sys0.gtb")
            ? (game_dir / "dwq_backup" / "sys0.gtb")
            : (fs::exists(game_dir / "dwq" / "sys0.gtb.bak")
                   ? (game_dir / "dwq" / "sys0.gtb.bak")
                   : (game_dir / "dwq" / "sys0.gtb"));

    const auto patch_bin_bytes = exm::read_file(patch_bin_path.string());
    const auto orig_gtb_bytes = exm::read_file(orig_gtb_path.string());
    const auto orig_gpk_bytes = exm::read_file(orig_gpk_path.string());

    const auto [built_gtb, built_gpk] =
        MenuArchivePatcher::reconstruct_archive_from_patch_bin(
            patch_bin_bytes, orig_gtb_bytes, orig_gpk_bytes);

    const auto parsed = parse_patch_bin(patch_bin_bytes);
    EXPECT_EQ(built_gpk.size(), parsed.new_gpk_size);
    EXPECT_EQ(built_gtb.size(), parsed.new_gtb_size);
    EXPECT_EQ(ComputeMurmurHash2(built_gtb.data(), built_gtb.size()),
              parsed.new_gtb_hash);
}

}  // namespace
