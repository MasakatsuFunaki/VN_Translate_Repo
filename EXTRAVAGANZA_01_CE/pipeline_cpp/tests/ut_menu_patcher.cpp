// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for the BLACKCyc GTB/GPK/DWQ menu archive parser/repacker and
// init2/*.fxf configuration patchers.
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "charts/charts.h"
#include "common/util.h"
#include "menus/menu_patcher.h"

namespace mn = exc::menus;
namespace fs = std::filesystem;
using namespace exc;

namespace {

inline constexpr std::uint32_t kByteMask = 0xFFu;
inline constexpr unsigned int kByte1Shift = 8;
inline constexpr unsigned int kByte2Shift = 16;
inline constexpr unsigned int kByte3Shift = 24;

inline constexpr std::uint8_t kDwqHeaderPadByte = 0x20;
inline constexpr std::size_t kDwqSignatureLength = 16;
inline constexpr std::size_t kDwqPackTypeBlockOffset = 0x2C;
inline constexpr std::size_t kDwqPackTypeBlockLength = 20;
inline constexpr std::size_t kInt32ByteSize = 4;

inline constexpr std::uint16_t kBmpPlanes = 1;
inline constexpr std::uint16_t kBmp24BitDepth = 24;
inline constexpr std::uint16_t kBmp8BitDepth = 8;
inline constexpr std::uint32_t kMaskPaletteColorCount = 256;

inline constexpr std::size_t kExpectedEntryCount = 183;
inline constexpr std::size_t kExpectedTranslatedCount = 64;
inline constexpr std::size_t kExpectedUntouchedCount = 119;
inline constexpr std::size_t kExpectedBackupCharanameSize = 100;
inline constexpr std::size_t kExpectedPatchedCharanameSize = 142;

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
        static_cast<std::uint32_t>(b[off]) |
        (static_cast<std::uint32_t>(b[off + 1]) << kByte1Shift) |
        (static_cast<std::uint32_t>(b[off + 2]) << kByte2Shift) |
        (static_cast<std::uint32_t>(b[off + 3]) << kByte3Shift));
}

fs::path ResolvePatchBinPath() {
    std::error_code ec;
    const fs::path roots[] = {fs::current_path(ec), fs::u8path(__FILE__).parent_path()};
    for (const fs::path& base : roots) {
        for (fs::path cur = base; !cur.empty(); cur = cur.parent_path()) {
            const fs::path direct = cur / "extravaganza_patch.bin";
            if (fs::is_regular_file(direct, ec)) {
                return direct;
            }
            const fs::path nested = cur / "EXTRAVAGANZA_01_CE" / "extravaganza_patch.bin";
            if (fs::is_regular_file(nested, ec)) {
                return nested;
            }
            if (cur == cur.root_path()) {
                break;
            }
        }
    }
    return fs::u8path("extravaganza_patch.bin");
}

bool ContainsBytes(const Bytes& haystack, const void* needle, std::size_t needle_len) {
    if (needle_len == 0 || haystack.size() < needle_len) {
        return false;
    }
    const auto* pat = static_cast<const std::uint8_t*>(needle);
    const std::size_t limit = haystack.size() - needle_len;
    for (std::size_t i = 0; i <= limit; ++i) {
        if (std::memcmp(haystack.data() + i, pat, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

// Build a synthetic 24-bit PACKTYPE=0 or PACKTYPE=2A DWQ entry from an RgbaImage.
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

    // 54-byte BITMAPFILEHEADER + BITMAPINFOHEADER (biHeight = +H, top-down in DWQ)
    dwq.push_back('B');
    dwq.push_back('M');
    put_u32_le(dwq, static_cast<std::uint32_t>(bmp_total_bytes));
    put_u16_le(dwq, 0);
    put_u16_le(dwq, 0);
    put_u32_le(dwq, static_cast<std::uint32_t>(mn::kBmpHeaderSize));
    put_u32_le(dwq, static_cast<std::uint32_t>(mn::kBmpInfoHeaderSize));
    put_i32_le(dwq, img.width);
    put_i32_le(dwq, img.height);  // positive biHeight -> top-down in BLACKCyc DWQ
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

        // 54-byte 8-bit BMP header + 1024-byte identity grayscale palette
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

    // Row 0: 300 zeros (exceeds single 255-byte RLE run), then non-zero values.
    for (int x = kLongZeroRunLength; x < kWidth; ++x) {
        alpha[x] = static_cast<std::uint8_t>((x * kRow0PatternMultiplier) & kByteMask);
    }
    // Row 1: identical to Row 0 (vertical XOR delta is all zeros across 320 pixels).
    std::memcpy(alpha.data() + kWidth, alpha.data(), kWidth);
    // Row 2..11: varying gradient pattern.
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

    // Set active pixels at (kActiveMinX, kActiveMinY) and (kActiveMaxX, kActiveMaxY).
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
    constexpr int kTestWidth = 17;  // non-multiple of 4 to exercise 24-bit BMP row padding
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
            px[mn::kRedChannel] =
                static_cast<std::uint8_t>(kBaseRed + y * kRowRedStep);  // distinct top-to-bottom R
            px[mn::kGreenChannel] = static_cast<std::uint8_t>(kBaseGreen + x * kColGreenStep);
            px[mn::kBlueChannel] = kTestBlue;
            px[mn::kAlphaChannel] = kTestAlpha;
        }
    }

    const Bytes orig_dwq = make_synthetic_dwq(orig_img, /*with_mask=*/true);
    const mn::RgbaImage decoded = mn::decode_dwq_to_rgba(orig_dwq.data(), orig_dwq.size());
    EXPECT_EQ(decoded.width, orig_img.width);
    EXPECT_EQ(decoded.height, orig_img.height);
    EXPECT_EQ(decoded.pixels, orig_img.pixels);

    // Expand active region to (kExpandedLeft..kExpandedRight, kExpandedTop..kExpandedBottom).
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
    const Bytes dwq0 = make_synthetic_dwq(img0, /*with_mask=*/false);
    const Bytes dwq1 = make_synthetic_dwq(img1, /*with_mask=*/true);

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

    // 0 overrides -> 100% byte-for-byte identical GTB and GPK.
    const auto [gtb_zero, gpk_zero] = arch.serialize();
    EXPECT_EQ(gtb_zero, gtb_in);
    EXPECT_EQ(gpk_zero, gpk_in);

    // Modify entry 1 -> entry 0 remains 100% byte-identical.
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
        "barMessage #\xE9\x9D\x92\xE2\x80\x95\xE2\x80\x95\r\n"
        "firstMessageNumber 1\r\n"
        "firstMessage1 #\xE9\x9D\x92\xE3\x80\x80\xE8\x82\xB2\xE6\x88\x90\xE8\xA8\x98\xE9\x8C\xB2\r\n";
    const Bytes enc_backlog = charts::encrypt_fxf(utf8_to_cp932_replace(raw_backlog));
    const Bytes patched_backlog_enc = mn::patch_backlog_fxf(enc_backlog);
    const Bytes patched_backlog_dec = charts::decrypt_fxf(patched_backlog_enc);
    const std::string backlog_utf8 =
        cp932_to_utf8_replace(patched_backlog_dec.data(), patched_backlog_dec.size());

    // Verify "#青" color tag is preserved and followed by fullwidth spaces + "Ｂａｃｋｌｏｇ".
    EXPECT_NE(backlog_utf8.find(
                  "firstMessage1 #\xE9\x9D\x92"
                  "\xE3\x80\x80\xE3\x80\x80\xE3\x80\x80\xE3\x80\x80"
                  "\xE3\x80\x80\xE3\x80\x80\xE3\x80\x80\xE3\x80\x80"
                  "\xEF\xBC\xA2\xEF\xBD\x81\xEF\xBD\x83\xEF\xBD\x8B\xEF\xBD\x8C\xEF\xBD\x8F\xEF\xBD\x87\r\n"),
              std::string::npos);
    EXPECT_NE(backlog_utf8.find("barMessage #\xE9\x9D\x92"), std::string::npos);
}

TEST(MenuPatcher, LiveBackupZeroOverrideRoundTripIsByteIdentical) {
    const char* env_dir = std::getenv("VN_GAME_DIR");
    const fs::path game_root = (env_dir && *env_dir) ? fs::u8path(env_dir) : fs::path();
    const fs::path gtb_bak = game_root / "dwq_backup" / "sys0.gtb";
    const fs::path gpk_bak = game_root / "dwq_backup" / "sys0.gpk";
    std::error_code ec;
    if (game_root.empty() || !fs::is_regular_file(gtb_bak, ec) || !fs::is_regular_file(gpk_bak, ec)) {
        GTEST_SKIP() << "Game backup directory not present on this machine.";
    }

    const Bytes orig_gtb = read_file(gtb_bak.u8string());
    const Bytes orig_gpk = read_file(gpk_bak.u8string());

    const mn::MenuArchivePatcher arch =
        mn::MenuArchivePatcher::load_from_memory(orig_gtb, orig_gpk);
    EXPECT_EQ(arch.entry_count(), kExpectedEntryCount);

    const auto [rebuilt_gtb, rebuilt_gpk] = arch.serialize();
    EXPECT_EQ(rebuilt_gtb, orig_gtb);
    EXPECT_EQ(rebuilt_gpk, orig_gpk);
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

TEST(MenuPatcher, LiveArchiveRoundTripAndUntouchedEntriesBitExact) {
    const char* env_dir = std::getenv("VN_GAME_DIR");
    const fs::path game_root = (env_dir && *env_dir) ? fs::u8path(env_dir) : fs::path();
    const fs::path patch_bin_path = ResolvePatchBinPath();
    std::error_code ec;
    ASSERT_TRUE(fs::is_regular_file(patch_bin_path, ec))
        << "Missing extravaganza_patch.bin at " << patch_bin_path.u8string();

    const Bytes patch_bin = read_file(patch_bin_path.u8string());
    ASSERT_GE(patch_bin.size(), mn::kPatchHeaderSize);
    EXPECT_EQ(std::memcmp(patch_bin.data(), mn::kPatchMagic, mn::kPatchMagicSize), 0);

    // Verify zero raw asset headers exist inside extravaganza_patch.bin.
    constexpr std::uint8_t kPngHeader[] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    constexpr std::size_t kPackBmpHeaderLen = 7;
    constexpr std::size_t kPackTypePrefixLen = 9;
    EXPECT_FALSE(ContainsBytes(patch_bin, "PACKBMP", kPackBmpHeaderLen));
    EXPECT_FALSE(ContainsBytes(patch_bin, "BMP+MASK        ", kDwqSignatureLength));
    EXPECT_FALSE(ContainsBytes(patch_bin, "PACKTYPE=", kPackTypePrefixLen));
    EXPECT_FALSE(ContainsBytes(patch_bin, kPngHeader, sizeof(kPngHeader)));

    const mn::ParsedPatchBin parsed = mn::parse_patch_bin(patch_bin);
    EXPECT_EQ(parsed.version, mn::kPatchFormatVersion);
    EXPECT_EQ(parsed.standalone_file_count, mn::kExpectedStandaloneFileCount);
    EXPECT_EQ(parsed.gpk_entry_count, mn::kExpectedModifiedGpkEntryCount);

    if (game_root.empty() || !fs::is_directory(game_root / "dwq_backup", ec)) {
        GTEST_SKIP() << "Live game directory not present on this machine.";
    }

    const Bytes orig_gtb = read_file((game_root / "dwq_backup" / "sys0.gtb").u8string());
    const Bytes orig_gpk = read_file((game_root / "dwq_backup" / "sys0.gpk").u8string());
    const Bytes live_gtb = read_file((game_root / "dwq" / "sys0.gtb").u8string());
    const Bytes live_gpk = read_file((game_root / "dwq" / "sys0.gpk").u8string());

    const auto [rebuilt_gtb, rebuilt_gpk] =
        mn::MenuArchivePatcher::reconstruct_archive_from_patch_bin(
            patch_bin, orig_gtb, orig_gpk);
    EXPECT_EQ(rebuilt_gtb, live_gtb);
    EXPECT_EQ(rebuilt_gpk, live_gpk);

    const mn::MenuArchivePatcher bak_arch =
        mn::MenuArchivePatcher::load_from_memory(orig_gtb, orig_gpk);
    const mn::MenuArchivePatcher rebuilt_arch =
        mn::MenuArchivePatcher::load_from_memory(rebuilt_gtb, rebuilt_gpk);
    ASSERT_EQ(bak_arch.entry_count(), kExpectedEntryCount);
    ASSERT_EQ(rebuilt_arch.entry_count(), kExpectedEntryCount);

    std::vector<bool> is_patched(kExpectedEntryCount, false);
    for (const mn::GpkEntryPatchRecord& rec : parsed.gpk_records) {
        ASSERT_LT(rec.entry_index, kExpectedEntryCount);
        is_patched[rec.entry_index] = true;
        const Bytes dwq_bytes = rebuilt_arch.raw_entry_bytes(rec.entry_index);
        const mn::RgbaImage decoded_img =
            mn::decode_dwq_to_rgba(dwq_bytes.data(), dwq_bytes.size());
        EXPECT_GT(decoded_img.width, 0);
        EXPECT_GT(decoded_img.height, 0);
    }

    std::size_t untouched_count = 0;
    for (std::size_t i = 0; i < kExpectedEntryCount; ++i) {
        if (!is_patched[i]) {
            EXPECT_EQ(rebuilt_arch.raw_entry_bytes(i), bak_arch.raw_entry_bytes(i))
                << "Untouched entry " << i << " modified!";
            ++untouched_count;
        }
    }
    EXPECT_EQ(untouched_count, kExpectedUntouchedCount);

    // Verify all 27 standalone file records reconstruct bit-for-bit via dry-run apply
    // and match the live patched files on disk.
    const mn::MenuBuildSummary summary =
        mn::MenuArchivePatcher::apply_patch_bin_to_game_dir(
            game_root.u8string(), patch_bin_path.u8string(), /*test_mode=*/true);
    EXPECT_EQ(summary.total_archive_entries, kExpectedEntryCount);
    EXPECT_EQ(summary.patched_sprite_entries, kExpectedTranslatedCount);
    EXPECT_EQ(summary.untouched_sprite_entries, kExpectedUntouchedCount);
    EXPECT_EQ(summary.patched_standalone_files, mn::kExpectedStandaloneFileCount);

    for (const mn::StandalonePatchRecord& rec : parsed.standalone_records) {
        const Bytes live_bytes = read_file((game_root / fs::u8path(rec.rel_path)).u8string());
        EXPECT_EQ(live_bytes.size(), rec.new_file_size) << rec.rel_path;
        EXPECT_EQ(mn::ComputeMurmurHash2(live_bytes.data(), live_bytes.size()),
                  rec.new_data_hash)
            << rec.rel_path;
    }

    // Verify init2/mainControl.fxf, init2/backlog.fxf, and spt/charaname.xtx.
    const Bytes exp_main = mn::patch_main_control_fxf(
        read_file((game_root / "init2_backup" / "mainControl.fxf").u8string()));
    const Bytes live_main = read_file((game_root / "init2" / "mainControl.fxf").u8string());
    EXPECT_EQ(live_main, exp_main);

    const Bytes exp_backlog = mn::patch_backlog_fxf(
        read_file((game_root / "init2_backup" / "backlog.fxf").u8string()));
    const Bytes live_backlog = read_file((game_root / "init2" / "backlog.fxf").u8string());
    EXPECT_EQ(live_backlog, exp_backlog);

    const Bytes live_xtx = read_file((game_root / "spt" / "charaname.xtx").u8string());
    const Bytes bak_xtx = read_file((game_root / "xtx_backup" / "charaname.xtx").u8string());
    EXPECT_EQ(bak_xtx.size(), kExpectedBackupCharanameSize);
    EXPECT_EQ(live_xtx.size(), kExpectedPatchedCharanameSize);
    EXPECT_NE(live_xtx, bak_xtx);
}
