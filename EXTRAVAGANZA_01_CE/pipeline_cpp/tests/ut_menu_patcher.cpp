// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Standalone unit tests for the BLACKCyc GTB/GPK/DWQ menu archive codec,
// MurmurHash2/XOR-delta payload helpers, and init2/*.fxf configuration patchers.
#include "menus/menu_patcher.h"

#include "charts/charts.h"
#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mn = exc::menus;
using namespace exc;

namespace {

inline constexpr std::uint32_t kByteMask = 0xFFu;
inline constexpr unsigned int kByte1Shift = 8;
inline constexpr unsigned int kByte2Shift = 16;
inline constexpr unsigned int kByte3Shift = 24;
inline constexpr std::size_t kByteIndex0 = 0;
inline constexpr std::size_t kByteIndex1 = 1;
inline constexpr std::size_t kByteIndex2 = 2;
inline constexpr std::size_t kByteIndex3 = 3;

inline constexpr std::uint8_t kDwqHeaderPadByte = 0x20;
inline constexpr std::size_t kDwqSignatureLength = 16;
inline constexpr std::size_t kDwqPackTypeBlockOffset = 0x2C;
inline constexpr std::size_t kDwqPackTypeBlockLength = 20;
inline constexpr std::size_t kInt32ByteSize = 4;

inline constexpr std::uint16_t kBmpPlanes = 1;
inline constexpr std::uint16_t kBmp24BitDepth = 24;
inline constexpr std::uint16_t kBmp8BitDepth = 8;
inline constexpr std::uint32_t kMaskPaletteColorCount = 256;

inline constexpr std::int32_t kDefaultInitialCropMargin = 1;

void put_u16_le(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & kByteMask));
    b.push_back(static_cast<std::uint8_t>((v >> kByte1Shift) & kByteMask));
}

void put_u32_le(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & kByteMask));
    b.push_back(static_cast<std::uint8_t>((v >> kByte1Shift) & kByteMask));
    b.push_back(static_cast<std::uint8_t>((v >> kByte2Shift) & kByteMask));
    b.push_back(static_cast<std::uint8_t>((v >> kByte3Shift) & kByteMask));
}

void put_i32_le(Bytes& b, std::int32_t v) {
    put_u32_le(b, static_cast<std::uint32_t>(v));
}

std::int32_t rd_i32_le(const Bytes& b, std::size_t off) {
    return static_cast<std::int32_t>(
        static_cast<std::uint32_t>(b[off + kByteIndex0]) |
        (static_cast<std::uint32_t>(b[off + kByteIndex1]) << kByte1Shift) |
        (static_cast<std::uint32_t>(b[off + kByteIndex2]) << kByte2Shift) |
        (static_cast<std::uint32_t>(b[off + kByteIndex3]) << kByte3Shift));
}

Bytes make_synthetic_dwq(const mn::RgbaImage& img, bool with_mask,
                         mn::CropMargins initial_crops = {
                             kDefaultInitialCropMargin, kDefaultInitialCropMargin,
                             kDefaultInitialCropMargin, kDefaultInitialCropMargin}) {
    const std::size_t align = static_cast<std::size_t>(mn::kBmpRowAlignment);
    const std::size_t stride =
        (static_cast<std::size_t>(img.width) * mn::kBgrChannels + (align - 1)) & ~(align - 1);
    const std::size_t bmp_px_bytes = stride * static_cast<std::size_t>(img.height);
    const std::size_t bmp_total_bytes = mn::kBmpHeaderSize + bmp_px_bytes;

    Bytes dwq(mn::kDwqHeaderSize, kDwqHeaderPadByte);
    if (with_mask) {
        std::memcpy(dwq.data(), "BMP+MASK        ", kDwqSignatureLength);
        std::memcpy(dwq.data() + kDwqPackTypeBlockOffset, "    PACKTYPE=2A     ",
                    kDwqPackTypeBlockLength);
        const std::uint8_t* p_top = reinterpret_cast<const std::uint8_t*>(&initial_crops.top);
        const std::uint8_t* p_left = reinterpret_cast<const std::uint8_t*>(&initial_crops.left);
        const std::uint8_t* p_right = reinterpret_cast<const std::uint8_t*>(&initial_crops.right);
        const std::uint8_t* p_bot = reinterpret_cast<const std::uint8_t*>(&initial_crops.bottom);
        std::memcpy(dwq.data() + mn::kDwqCropTopOffset, p_top, kInt32ByteSize);
        std::memcpy(dwq.data() + mn::kDwqCropLeftOffset, p_left, kInt32ByteSize);
        std::memcpy(dwq.data() + mn::kDwqCropRightOffset, p_right, kInt32ByteSize);
        std::memcpy(dwq.data() + mn::kDwqCropBottomOffset, p_bot, kInt32ByteSize);
        const std::int32_t ds = static_cast<std::int32_t>(bmp_total_bytes);
        std::memcpy(dwq.data() + mn::kDwqDataSizeOffset, &ds, kInt32ByteSize);
    } else {
        std::memcpy(dwq.data(), "BMP             ", kDwqSignatureLength);
        std::memcpy(dwq.data() + kDwqPackTypeBlockOffset, "    PACKTYPE=0      ",
                    kDwqPackTypeBlockLength);
    }
    const std::int32_t w32 = img.width;
    const std::int32_t h32 = img.height;
    std::memcpy(dwq.data() + mn::kDwqWidthOffset, &w32, kInt32ByteSize);
    std::memcpy(dwq.data() + mn::kDwqHeightOffset, &h32, kInt32ByteSize);

    dwq.push_back('B');
    dwq.push_back('M');
    put_u32_le(dwq, static_cast<std::uint32_t>(bmp_total_bytes));
    put_u16_le(dwq, 0);
    put_u16_le(dwq, 0);
    put_u32_le(dwq, static_cast<std::uint32_t>(mn::kBmpHeaderSize));
    put_u32_le(dwq, static_cast<std::uint32_t>(mn::kBmpInfoHeaderSize));
    put_i32_le(dwq, img.width);
    put_i32_le(dwq, img.height);
    put_u16_le(dwq, kBmpPlanes);
    put_u16_le(dwq, kBmp24BitDepth);
    put_u32_le(dwq, 0);
    put_u32_le(dwq, static_cast<std::uint32_t>(bmp_px_bytes));
    put_i32_le(dwq, 0);
    put_i32_le(dwq, 0);
    put_u32_le(dwq, 0);
    put_u32_le(dwq, 0);

    Bytes bgr_rows(bmp_px_bytes, 0);
    for (int y = 0; y < img.height; ++y) {
        std::uint8_t* dst = bgr_rows.data() + static_cast<std::size_t>(y) * stride;
        const std::uint8_t* src =
            img.pixels.data() + static_cast<std::size_t>(y) * img.width * mn::kRgbaChannels;
        for (int x = 0; x < img.width; ++x) {
            dst[x * mn::kBgrChannels + mn::kBgrBlueIndex] =
                src[x * mn::kRgbaChannels + mn::kBlueChannel];
            dst[x * mn::kBgrChannels + mn::kBgrGreenIndex] =
                src[x * mn::kRgbaChannels + mn::kGreenChannel];
            dst[x * mn::kBgrChannels + mn::kBgrRedIndex] =
                src[x * mn::kRgbaChannels + mn::kRedChannel];
        }
    }
    dwq.insert(dwq.end(), bgr_rows.begin(), bgr_rows.end());

    if (with_mask) {
        const std::size_t total_px = static_cast<std::size_t>(img.width) * img.height;
        Bytes alpha(total_px, 0);
        for (std::size_t i = 0; i < total_px; ++i) {
            alpha[i] = img.pixels[i * mn::kRgbaChannels + mn::kAlphaChannel];
        }
        const Bytes rle = mn::encode_rle_xor_mask(alpha.data(), img.width, img.height);

        dwq.push_back('B');
        dwq.push_back('M');
        put_u32_le(dwq, static_cast<std::uint32_t>(mn::kMaskHeaderTotalSize + total_px));
        put_u16_le(dwq, 0);
        put_u16_le(dwq, 0);
        put_u32_le(dwq, static_cast<std::uint32_t>(mn::kMaskHeaderTotalSize));
        put_u32_le(dwq, static_cast<std::uint32_t>(mn::kBmpInfoHeaderSize));
        put_i32_le(dwq, img.width);
        put_i32_le(dwq, img.height);
        put_u16_le(dwq, kBmpPlanes);
        put_u16_le(dwq, kBmp8BitDepth);
        put_u32_le(dwq, 0);
        put_u32_le(dwq, static_cast<std::uint32_t>(total_px));
        put_i32_le(dwq, 0);
        put_i32_le(dwq, 0);
        put_u32_le(dwq, kMaskPaletteColorCount);
        put_u32_le(dwq, 0);

        for (std::uint32_t i = 0; i < kMaskPaletteColorCount; ++i) {
            dwq.push_back(static_cast<std::uint8_t>(i));
            dwq.push_back(static_cast<std::uint8_t>(i));
            dwq.push_back(static_cast<std::uint8_t>(i));
            dwq.push_back(0);
        }
        dwq.insert(dwq.end(), rle.begin(), rle.end());
    }

    return dwq;
}

}  // namespace

TEST(MenuPatcher, RleXorMaskRoundTripHandlesLongZeroRunsAndVerticalDeltas) {
    constexpr int kWidth = 320;
    constexpr int kHeight = 12;
    constexpr int kLongZeroRunLength = 300;
    constexpr int kRow0PatternMultiplier = 13;
    constexpr int kGradientFirstRow = 2;
    constexpr int kGradientMultiplier = 7;

    Bytes alpha(static_cast<std::size_t>(kWidth) * kHeight, 0);

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

    const Bytes encoded = mn::encode_rle_xor_mask(alpha.data(), kWidth, kHeight);
    EXPECT_LT(encoded.size(), alpha.size());

    const Bytes decoded =
        mn::decode_rle_xor_mask(encoded.data(), encoded.size(), kWidth, kHeight);
    EXPECT_EQ(decoded, alpha);
}

TEST(MenuPatcher, ComputeCropMarginsFindsTightActiveBoundingBox) {
    constexpr int kTestWidth = 20;
    constexpr int kTestHeight = 15;
    constexpr int kActiveMinX = 3;
    constexpr int kActiveMinY = 2;
    constexpr int kActiveMaxX = 14;
    constexpr int kActiveMaxY = 11;
    constexpr std::uint8_t kHalfAlpha = 128;
    constexpr std::uint8_t kFullAlpha = 255;

    mn::RgbaImage img;
    img.width = kTestWidth;
    img.height = kTestHeight;
    img.pixels.assign(
        static_cast<std::size_t>(img.width) * img.height * mn::kRgbaChannels, 0);

    img.pixels[(kActiveMinY * img.width + kActiveMinX) * mn::kRgbaChannels +
               mn::kAlphaChannel] = kHalfAlpha;
    img.pixels[(kActiveMaxY * img.width + kActiveMaxX) * mn::kRgbaChannels +
               mn::kAlphaChannel] = kFullAlpha;

    const mn::CropMargins margins = mn::compute_crop_margins(img);
    EXPECT_EQ(margins.top, kActiveMinY);
    EXPECT_EQ(margins.left, kActiveMinX);
    EXPECT_EQ(margins.right, kTestWidth - 1 - kActiveMaxX);
    EXPECT_EQ(margins.bottom, kTestHeight - 1 - kActiveMaxY);
}

TEST(MenuPatcher, DwqPackType2ATopDownScanlinesAndCropMarginsRoundTrip) {
    constexpr int kTestWidth = 17;
    constexpr int kTestHeight = 10;
    constexpr int kInitialTop = 2;
    constexpr int kInitialBottom = 7;
    constexpr int kInitialLeft = 4;
    constexpr int kInitialRight = 12;
    constexpr int kBaseRed = 10;
    constexpr int kRowRedStep = 20;
    constexpr int kBaseGreen = 20;
    constexpr int kColGreenStep = 10;
    constexpr std::uint8_t kTestBlue = 180;
    constexpr std::uint8_t kTestAlpha = 240;

    constexpr int kExpandedTop = 1;
    constexpr int kExpandedLeft = 1;
    constexpr int kExpandedBottom = 8;
    constexpr int kExpandedRight = 15;
    constexpr std::uint8_t kCornerRed = 250;
    constexpr std::uint8_t kCornerBlue = 220;
    constexpr std::uint8_t kCornerAlpha = 200;
    constexpr std::uint8_t kFullAlpha = 255;

    mn::RgbaImage orig_img;
    orig_img.width = kTestWidth;
    orig_img.height = kTestHeight;
    orig_img.pixels.assign(
        static_cast<std::size_t>(orig_img.width) * orig_img.height * mn::kRgbaChannels, 0);

    for (int y = kInitialTop; y <= kInitialBottom; ++y) {
        for (int x = kInitialLeft; x <= kInitialRight; ++x) {
            std::uint8_t* px =
                orig_img.pixels.data() +
                (static_cast<std::size_t>(y) * orig_img.width + x) * mn::kRgbaChannels;
            px[mn::kRedChannel] = static_cast<std::uint8_t>(kBaseRed + y * kRowRedStep);
            px[mn::kGreenChannel] = static_cast<std::uint8_t>(kBaseGreen + x * kColGreenStep);
            px[mn::kBlueChannel] = kTestBlue;
            px[mn::kAlphaChannel] = kTestAlpha;
        }
    }

    const Bytes orig_dwq = make_synthetic_dwq(orig_img, true);
    const mn::RgbaImage decoded = mn::decode_dwq_to_rgba(orig_dwq.data(), orig_dwq.size());
    EXPECT_EQ(decoded.width, orig_img.width);
    EXPECT_EQ(decoded.height, orig_img.height);
    EXPECT_EQ(decoded.pixels, orig_img.pixels);

    mn::RgbaImage mod_img = decoded;
    mod_img.pixels[(kExpandedTop * mod_img.width + kExpandedLeft) * mn::kRgbaChannels +
                   mn::kRedChannel] = kCornerRed;
    mod_img.pixels[(kExpandedTop * mod_img.width + kExpandedLeft) * mn::kRgbaChannels +
                   mn::kAlphaChannel] = kFullAlpha;
    mod_img.pixels[(kExpandedBottom * mod_img.width + kExpandedRight) * mn::kRgbaChannels +
                   mn::kBlueChannel] = kCornerBlue;
    mod_img.pixels[(kExpandedBottom * mod_img.width + kExpandedRight) * mn::kRgbaChannels +
                   mn::kAlphaChannel] = kCornerAlpha;

    const Bytes reencoded =
        mn::encode_dwq_from_rgba(orig_dwq.data(), orig_dwq.size(), mod_img, true);
    EXPECT_EQ(rd_i32_le(reencoded, mn::kDwqCropTopOffset), kExpandedTop);
    EXPECT_EQ(rd_i32_le(reencoded, mn::kDwqCropLeftOffset), kExpandedLeft);
    EXPECT_EQ(rd_i32_le(reencoded, mn::kDwqCropRightOffset),
              kTestWidth - 1 - kExpandedRight);
    EXPECT_EQ(rd_i32_le(reencoded, mn::kDwqCropBottomOffset),
              kTestHeight - 1 - kExpandedBottom);

    const mn::RgbaImage redecoded = mn::decode_dwq_to_rgba(reencoded.data(), reencoded.size());
    EXPECT_EQ(redecoded.pixels, mod_img.pixels);
}

TEST(MenuPatcher, GpkArchivePreservesUntouchedEntriesByteForByte) {
    constexpr int kImg0Width = 8;
    constexpr int kImg0Height = 6;
    constexpr int kImg1Width = 12;
    constexpr int kImg1Height = 8;
    constexpr std::uint8_t kFullAlpha = 255;
    constexpr std::uint8_t kHalfAlpha = 128;
    constexpr std::uint8_t kModifiedRed = 42;
    constexpr std::uint32_t kSyntheticEntryCount = 2;
    constexpr std::size_t kFirstEntryIndex = 0;
    constexpr std::size_t kSecondEntryIndex = 1;

    mn::RgbaImage img0{
        kImg0Width, kImg0Height,
        Bytes(static_cast<std::size_t>(kImg0Width) * kImg0Height * mn::kRgbaChannels,
              kFullAlpha)};
    mn::RgbaImage img1{
        kImg1Width, kImg1Height,
        Bytes(static_cast<std::size_t>(kImg1Width) * kImg1Height * mn::kRgbaChannels,
              kHalfAlpha)};
    const Bytes dwq0 = make_synthetic_dwq(img0, false);
    const Bytes dwq1 = make_synthetic_dwq(img1, true);

    Bytes gpk_in;
    gpk_in.insert(gpk_in.end(), dwq0.begin(), dwq0.end());
    gpk_in.insert(gpk_in.end(), dwq1.begin(), dwq1.end());

    const std::string name0 = "alpha_sprite";
    const std::string name1 = "beta_sprite";
    Bytes names_blob;
    names_blob.insert(names_blob.end(), name0.begin(), name0.end());
    names_blob.push_back(0);
    const std::uint32_t name1_off = static_cast<std::uint32_t>(names_blob.size());
    names_blob.insert(names_blob.end(), name1.begin(), name1.end());
    names_blob.push_back(0);

    Bytes gtb_in;
    put_u32_le(gtb_in, kSyntheticEntryCount);
    put_u32_le(gtb_in, 0);
    put_u32_le(gtb_in, name1_off);
    put_u32_le(gtb_in, 0);
    put_u32_le(gtb_in, static_cast<std::uint32_t>(dwq0.size()));
    gtb_in.insert(gtb_in.end(), names_blob.begin(), names_blob.end());

    mn::GpkArchive arch = mn::GpkArchive::load_from_memory(gtb_in, gpk_in);
    ASSERT_EQ(arch.entry_count(), kSyntheticEntryCount);
    ASSERT_TRUE(arch.find_entry_index("BETA_SPRITE").has_value());
    EXPECT_EQ(*arch.find_entry_index("BETA_SPRITE"), kSecondEntryIndex);

    const auto [gtb_zero, gpk_zero] = arch.serialize();
    EXPECT_EQ(gtb_zero, gtb_in);
    EXPECT_EQ(gpk_zero, gpk_in);

    img1.pixels[mn::kRedChannel] = kModifiedRed;
    img1.pixels[mn::kAlphaChannel] = kFullAlpha;
    arch.replace_entry_rgba(kSecondEntryIndex, img1);
    EXPECT_EQ(arch.raw_entry_bytes(kFirstEntryIndex), dwq0);
}

TEST(MenuPatcher, MainControlAndBacklogFxfPatchesPreserveFormatInvariants) {
    const std::string raw_main =
        "windowTitle EXTRAVAGANZA\r\n"
        "windowTitleTaiken EXTRAVAGANZA_TRIAL\r\n"
        "mutexErrorMessage JP_MUTEX_MSG\r\n"
        "mutexErrorTitle JP_MUTEX_TITLE\r\n"
        "bppErrorMessage JP_BPP_MSG\r\n"
        "bppErrorTitle Error\r\n"
        "addVersionToWindowTitleFlag 1\r\n";
    const Bytes enc_main = charts::encrypt_fxf(utf8_to_cp932_replace(raw_main));
    const Bytes patched_main_enc = mn::patch_main_control_fxf(enc_main);
    const Bytes patched_main_dec = charts::decrypt_fxf(patched_main_enc);
    const std::string main_utf8 =
        cp932_to_utf8_replace(patched_main_dec.data(), patched_main_dec.size());

    EXPECT_NE(main_utf8.find("windowTitle EXTRAVAGANZA ~The Bug-Loving Girl~ CE\r\n"),
              std::string::npos);
    EXPECT_NE(main_utf8.find("windowTitleTaiken EXTRAVAGANZA ~The Bug-Loving Girl~ Trial\r\n"),
              std::string::npos);
    EXPECT_NE(main_utf8.find("mutexErrorMessage The game is already running.\r\n"),
              std::string::npos);
    EXPECT_NE(main_utf8.find("mutexErrorTitle Already Running\r\n"), std::string::npos);
    EXPECT_NE(
        main_utf8.find("bppErrorMessage Please set display to 16-bit color or higher.\r\n"),
        std::string::npos);
    EXPECT_NE(main_utf8.find("addVersionToWindowTitleFlag 1\r\n"), std::string::npos);

    const std::string raw_backlog =
        "barMessage #青――\r\n"
        "firstMessageNumber 1\r\n"
        "firstMessage1 #青　育成記録\r\n";
    const Bytes enc_backlog = charts::encrypt_fxf(utf8_to_cp932_replace(raw_backlog));
    const Bytes patched_backlog_enc = mn::patch_backlog_fxf(enc_backlog);
    const Bytes patched_backlog_dec = charts::decrypt_fxf(patched_backlog_enc);
    const std::string backlog_utf8 =
        cp932_to_utf8_replace(patched_backlog_dec.data(), patched_backlog_dec.size());

    EXPECT_NE(backlog_utf8.find("firstMessage1 #青　　　　　　　　Ｂａｃｋｌｏｇ\r\n"),
              std::string::npos);
    EXPECT_NE(backlog_utf8.find("barMessage #青"), std::string::npos);
}

TEST(MenuPatcher, MurmurHash2AndXorMaskPayloadRoundTrip) {
    constexpr std::size_t kOrigSize = 37;
    constexpr std::size_t kPatchedSize = 113;
    constexpr std::uint8_t kOrigPatternStep = 19;
    constexpr std::uint8_t kOrigPatternOffset = 7;
    constexpr std::uint8_t kPatchedPatternStep = 41;
    constexpr std::uint8_t kPatchedPatternOffset = 3;

    Bytes orig(kOrigSize);
    for (std::size_t i = 0; i < kOrigSize; ++i) {
        orig[i] = static_cast<std::uint8_t>((i * kOrigPatternStep + kOrigPatternOffset) & kByteMask);
    }
    Bytes patched(kPatchedSize);
    for (std::size_t i = 0; i < kPatchedSize; ++i) {
        patched[i] = static_cast<std::uint8_t>((i * kPatchedPatternStep + kPatchedPatternOffset) & kByteMask);
    }

    const std::uint32_t orig_hash = mn::ComputeMurmurHash2(orig.data(), orig.size());
    const std::uint32_t new_hash = mn::ComputeMurmurHash2(patched.data(), patched.size());
    EXPECT_NE(orig_hash, 0u);
    EXPECT_NE(new_hash, 0u);
    EXPECT_NE(orig_hash, new_hash);

    Bytes buf = patched;
    mn::XorMaskPayload(buf.data(), buf.size(), orig.data(), orig.size(), orig_hash);
    EXPECT_NE(buf, patched);

    mn::XorMaskPayload(buf.data(), buf.size(), orig.data(), orig.size(), orig_hash);
    EXPECT_EQ(buf, patched);
    EXPECT_EQ(mn::ComputeMurmurHash2(buf.data(), buf.size()), new_hash);
}
