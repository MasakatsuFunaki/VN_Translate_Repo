// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Synthetic in-memory unit tests for the "MSGMENU1" v1 menu patcher
// (`mgi::menu`).  Runs completely in memory with zero filesystem reads.

#include "menu/menu_patcher.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mgi::menu {
namespace {

constexpr std::uint32_t kBitsPerByte = 8u;
constexpr std::uint32_t kBitsPerU64 = 64u;
constexpr std::uint32_t kByteShift1 = 8u;
constexpr std::uint32_t kByteShift2 = 16u;
constexpr std::uint32_t kByteShift3 = 24u;
constexpr std::uint32_t kLowByteMask = 0xFFu;
constexpr std::uint64_t kLowU32Mask = 0xFFFFFFFFull;

constexpr std::size_t kRgbaChannels = 4u;
constexpr std::size_t kU32ByteSize = 4u;
constexpr std::size_t kU64ByteSize = 8u;
constexpr std::size_t kDwqTypeFieldBytes = 16u;
constexpr std::size_t kDwqPackFieldBytes = 20u;

constexpr int kSyntheticMarginTop = 2;
constexpr int kSyntheticMarginLeft = 4;
constexpr int kSyntheticMarginRight = 5;
constexpr int kSyntheticMarginBottom = 3;
constexpr std::uint8_t kSyntheticPixelR = 32u;
constexpr std::uint8_t kSyntheticPixelG = 180u;
constexpr std::uint8_t kSyntheticPixelB = 170u;
constexpr std::uint8_t kSyntheticPixelA = 240u;
constexpr std::uint8_t kFullChannelByte = 255u;

constexpr std::size_t kExpectedHd10OrigMainControlBytes = 480u;
constexpr std::uint32_t kExpectedHd10OrigMainControlHash = 0x6998A1B1u;
constexpr std::size_t kExpectedHd10NewMainControlBytes = 500u;
constexpr std::uint32_t kExpectedHd10NewMainControlHash = 0x78E8EF15u;

constexpr std::size_t kExpectedCeOrigMainControlBytes = 468u;
constexpr std::uint32_t kExpectedCeOrigMainControlHash = 0xD5D73580u;
constexpr std::size_t kExpectedCeNewMainControlBytes = 488u;
constexpr std::uint32_t kExpectedCeNewMainControlHash = 0x8F0C0B82u;

constexpr std::size_t kExpectedOrigCharanameBytes = 104u;
constexpr std::uint32_t kExpectedOrigCharanameHash = 0x62F2B2CEu;
constexpr std::size_t kExpectedNewCharanameBytes = 154u;
constexpr std::uint32_t kExpectedNewCharanameHash = 0x94C1B06Cu;

constexpr std::size_t kHeaderVersionOffset = 8u;
constexpr std::uint8_t kInvalidFormatVersion = 99u;
constexpr std::uint8_t kSingleBitFlipMask = 0x01u;

std::string to_cp932_str(std::string_view utf8) {
    const auto bytes = mgi::utf8_to_cp932_strict(std::string(utf8));
    if (!bytes) {
        throw std::runtime_error("Strict UTF-8 -> CP932 conversion failed in test fixture");
    }
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

std::vector<std::uint8_t> to_cp932_bytes(std::string_view utf8) {
    const auto bytes = mgi::utf8_to_cp932_strict(std::string(utf8));
    if (!bytes) {
        throw std::runtime_error("Strict UTF-8 -> CP932 conversion failed in test fixture");
    }
    return *bytes;
}

std::vector<std::uint8_t> make_synthetic_over2g_gtb(
    const std::vector<std::uint32_t>& non_seq_name_offs,
    const std::vector<std::uint64_t>& entry_offsets,
    const std::vector<std::uint8_t>& pool_bytes) {
    const auto count = static_cast<std::uint32_t>(entry_offsets.size());
    std::vector<std::uint8_t> gtb;

    auto push_u32 = [&](std::uint32_t v) {
        gtb.push_back(static_cast<std::uint8_t>(v & kLowByteMask));
        gtb.push_back(static_cast<std::uint8_t>((v >> kByteShift1) & kLowByteMask));
        gtb.push_back(static_cast<std::uint8_t>((v >> kByteShift2) & kLowByteMask));
        gtb.push_back(static_cast<std::uint8_t>((v >> kByteShift3) & kLowByteMask));
    };
    auto push_u64 = [&](std::uint64_t v) {
        for (unsigned shift = 0; shift < kBitsPerU64; shift += kBitsPerByte) {
            gtb.push_back(static_cast<std::uint8_t>((v >> shift) & kLowByteMask));
        }
    };

    push_u32(count);
    for (std::uint32_t noff : non_seq_name_offs) {
        push_u32(noff);
    }
    for (std::uint64_t off : entry_offsets) {
        push_u32(static_cast<std::uint32_t>(off & kLowU32Mask));
    }
    gtb.insert(gtb.end(), pool_bytes.begin(), pool_bytes.end());
    for (std::uint64_t off : entry_offsets) {
        push_u64(off);
    }
    for (std::size_t i = 0; i < kU64ByteSize; ++i) {
        gtb.push_back(0u);
    }
    gtb.insert(gtb.end(), kGtbOver2GTrailer, kGtbOver2GTrailer + kGtbOver2GTrailerSize);
    return gtb;
}

std::vector<std::uint8_t> make_synthetic_dwq_entry(int w,
                                                   int h,
                                                   RgbaImage& out_img,
                                                   std::int32_t init_margin = 0) {
    out_img.width = w;
    out_img.height = h;
    out_img.channels = static_cast<int>(kRgbaChannels);
    out_img.pixels.assign(static_cast<std::size_t>(w) * h * kRgbaChannels, 0u);
    for (int y = kSyntheticMarginTop; y < h - kSyntheticMarginBottom; ++y) {
        for (int x = kSyntheticMarginLeft; x < w - kSyntheticMarginRight; ++x) {
            std::uint8_t* px =
                out_img.pixels.data() + (static_cast<std::size_t>(y) * w + x) * kRgbaChannels;
            px[0] = kSyntheticPixelR;
            px[1] = kSyntheticPixelG;
            px[2] = kSyntheticPixelB;
            px[3] = kSyntheticPixelA;
        }
    }
    const auto png = encode_png(out_img, false);

    std::vector<std::uint8_t> raw(kDwqHeaderSize, 0u);
    constexpr char kTypeField[kDwqTypeFieldBytes] = {
        'P', 'N', 'G', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    constexpr char kPackField[kDwqPackFieldBytes] = {
        ' ', ' ', ' ', ' ', 'P', 'A', 'C', 'K', 'T', 'Y',
        'P', 'E', '=', '8', 'A', ' ', ' ', ' ', ' ', ' '};
    std::memcpy(raw.data(), kTypeField, sizeof(kTypeField));
    for (std::size_t m = 0; m < kDwqCropMarginCount; ++m) {
        const auto v = static_cast<std::uint32_t>(init_margin);
        const std::size_t base = kDwqCropMarginsOffset + m * kU32ByteSize;
        raw[base + 0] = static_cast<std::uint8_t>(v & kLowByteMask);
        raw[base + 1] = static_cast<std::uint8_t>((v >> kByteShift1) & kLowByteMask);
        raw[base + 2] = static_cast<std::uint8_t>((v >> kByteShift2) & kLowByteMask);
        raw[base + 3] = static_cast<std::uint8_t>((v >> kByteShift3) & kLowByteMask);
    }
    const auto psz = static_cast<std::uint32_t>(png.size());
    raw[kDwqDataSizeOffset + 0] = static_cast<std::uint8_t>(psz & kLowByteMask);
    raw[kDwqDataSizeOffset + 1] = static_cast<std::uint8_t>((psz >> kByteShift1) & kLowByteMask);
    raw[kDwqDataSizeOffset + 2] = static_cast<std::uint8_t>((psz >> kByteShift2) & kLowByteMask);
    raw[kDwqDataSizeOffset + 3] = static_cast<std::uint8_t>((psz >> kByteShift3) & kLowByteMask);
    raw[kDwqWidthOffset + 0] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(w) & kLowByteMask);
    raw[kDwqWidthOffset + 1] =
        static_cast<std::uint8_t>((static_cast<std::uint32_t>(w) >> kByteShift1) & kLowByteMask);
    raw[kDwqHeightOffset + 0] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(h) & kLowByteMask);
    raw[kDwqHeightOffset + 1] =
        static_cast<std::uint8_t>((static_cast<std::uint32_t>(h) >> kByteShift1) & kLowByteMask);
    std::memcpy(raw.data() + kDwqPackTypeOffset, kPackField, sizeof(kPackField));
    raw.insert(raw.end(), png.begin(), png.end());
    return raw;
}

}  // namespace

TEST(MenuPatcherUnitTest, FxfBitwiseNotCodecRoundTripsCp932Bytes) {
    const std::string cp932 = to_cp932_str(
        "windowTitle EXTRAVAGANZA～蟲狂編～HD10\r\n"
        "bppErrorMessage 16bitカラー以上に設定してください\r\n");
    const auto encrypted = encrypt_fxf(cp932);
    ASSERT_EQ(encrypted.size(), cp932.size());
    for (std::size_t i = 0; i < cp932.size(); ++i) {
        EXPECT_EQ(encrypted[i], static_cast<std::uint8_t>(~static_cast<std::uint8_t>(cp932[i])));
    }
    EXPECT_EQ(decrypt_fxf(encrypted), cp932);
}

TEST(MenuPatcherUnitTest, MainControlFxfPreservesIdentifiersAndMatchesHd10AndCeHashes) {
    const std::string hd10_plain = to_cp932_str(
        "name mainControl\r\n"
        "\r\n"
        "className bcycmushikuruhd10class\r\n"
        "\r\n"
        "mutexName MUSHIKURUHD10\r\n"
        "\r\n"
        "windowTitle EXTRAVAGANZA～蟲狂編～HD10\r\n"
        "\r\n"
        "windowTitleTaiken EXTRAVAGANZA～蟲狂編～（体験版）\r\n"
        "\r\n"
        "addVersionToWindowTitleFlag 1\r\n"
        "\r\n"
        "\tversionTitleMessage version\r\n"
        "\r\n"
        "bppErrorMessage 16bitカラー以上に設定してください\r\n"
        "\r\n"
        "\r\n"
        "\r\n"
        "@iconname exe.ico\r\n"
        "\r\n"
        "windowSizeX 1280\r\n"
        "windowSizeY 720\r\n"
        "\r\n"
        "@screenSizeTypeNumber 2\r\n"
        "\r\n"
        "@realWindowSizeX1 1280\r\n"
        "@realWindowSizeY1 720\r\n"
        "\r\n"
        "@realWindowSizeX2 1366\r\n"
        "@realWindowSizeY2 768");

    const auto hd10_orig_enc = encrypt_fxf(hd10_plain);
    EXPECT_EQ(hd10_orig_enc.size(), kExpectedHd10OrigMainControlBytes);
    EXPECT_EQ(ComputeMurmurHash2(hd10_orig_enc.data(), hd10_orig_enc.size()),
              kExpectedHd10OrigMainControlHash);

    const std::string hd10_patched = patch_main_control_fxf_cp932(hd10_plain);
    EXPECT_NE(hd10_patched.find("className bcycmushikuruhd10class\r\n"), std::string::npos);
    EXPECT_NE(hd10_patched.find("mutexName MUSHIKURUHD10\r\n"), std::string::npos);
    EXPECT_NE(hd10_patched.find("windowTitle EXTRAVAGANZA ~Mushigurui~ HD10\r\n"),
              std::string::npos);
    EXPECT_NE(hd10_patched.find("windowTitleTaiken EXTRAVAGANZA ~Mushigurui~ HD10 Trial\r\n"),
              std::string::npos);
    EXPECT_NE(
        hd10_patched.find("bppErrorMessage Please set display to 16-bit color or higher.\r\n"),
        std::string::npos);
    EXPECT_NE(hd10_patched.back(), '\n');

    const auto hd10_new_enc = encrypt_fxf(hd10_patched);
    EXPECT_EQ(hd10_new_enc.size(), kExpectedHd10NewMainControlBytes);
    EXPECT_EQ(ComputeMurmurHash2(hd10_new_enc.data(), hd10_new_enc.size()),
              kExpectedHd10NewMainControlHash);

    const std::string ce_plain = to_cp932_str(
        "name mainControl\r\n"
        "\r\n"
        "className bcycmushikuruclass\r\n"
        "\r\n"
        "mutexName MUSHIKURU\r\n"
        "\r\n"
        "windowTitle EXTRAVAGANZA～蟲狂編～\r\n"
        "\r\n"
        "windowTitleTaiken EXTRAVAGANZA～蟲狂編～（体験版）\r\n"
        "\r\n"
        "addVersionToWindowTitleFlag 1\r\n"
        "\r\n"
        "\tversionTitleMessage version\r\n"
        "\r\n"
        "bppErrorMessage 16bitカラー以上に設定してください\r\n"
        "\r\n"
        "\r\n"
        "\r\n"
        "@iconname exe.ico\r\n"
        "\r\n"
        "windowSizeX 1280\r\n"
        "windowSizeY 720\r\n"
        "\r\n"
        "@screenSizeTypeNumber 2\r\n"
        "\r\n"
        "@realWindowSizeX1 1280\r\n"
        "@realWindowSizeY1 720\r\n"
        "\r\n"
        "@realWindowSizeX2 1366\r\n"
        "@realWindowSizeY2 768");

    const auto ce_orig_enc = encrypt_fxf(ce_plain);
    EXPECT_EQ(ce_orig_enc.size(), kExpectedCeOrigMainControlBytes);
    EXPECT_EQ(ComputeMurmurHash2(ce_orig_enc.data(), ce_orig_enc.size()),
              kExpectedCeOrigMainControlHash);

    const std::string ce_patched = patch_main_control_fxf_cp932(ce_plain);
    EXPECT_NE(ce_patched.find("className bcycmushikuruclass\r\n"), std::string::npos);
    EXPECT_NE(ce_patched.find("mutexName MUSHIKURU\r\n"), std::string::npos);
    EXPECT_NE(ce_patched.find("windowTitle EXTRAVAGANZA ~Mushigurui~ CE\r\n"), std::string::npos);
    EXPECT_NE(ce_patched.find("windowTitleTaiken EXTRAVAGANZA ~Mushigurui~ CE Trial\r\n"),
              std::string::npos);

    const auto ce_new_enc = encrypt_fxf(ce_patched);
    EXPECT_EQ(ce_new_enc.size(), kExpectedCeNewMainControlBytes);
    EXPECT_EQ(ComputeMurmurHash2(ce_new_enc.data(), ce_new_enc.size()),
              kExpectedCeNewMainControlHash);
}

TEST(MenuPatcherUnitTest, BacklogFxfPreservesWhiteColorTagAndFullwidthHeading) {
    const std::string orig = to_cp932_str(
        "barMessage \t#白――――――――――――――――――――――――\r\n"
        "firstMessage1 \t#白　　　　　　　　　　　　　◆◆回想◆◆　　　\r\n"
        "endFlag 1\r\n");

    const std::string patched = patch_backlog_fxf_cp932(orig);
    const std::string expected = to_cp932_str(
        "barMessage \t#白――――――――――――――――――――――――\r\n"
        "firstMessage1 \t#白　　　　　　　　Ｂａｃｋｌｏｇ\r\n"
        "endFlag 1\r\n");
    EXPECT_EQ(patched, expected);
}

TEST(MenuPatcherUnitTest, CharanameXtxMapsSpeakerKeysAndPreservesTrailingBlankCrlf) {
    const std::string orig = to_cp932_str(
        "no,名前なし\r\n"
        "aa,美弥香\r\n"
        "bb,レン\r\n"
        "cc,綾佳\r\n"
        "dd,ユーリア\r\n"
        "ee,シオ\r\n"
        "ff,夢美\r\n"
        "gg,風ノ宮\r\n"
        "hh,キョウ\r\n"
        "ii,狂\r\n"
        "\r\n");
    ASSERT_EQ(orig.size(), kExpectedOrigCharanameBytes);
    EXPECT_EQ(ComputeMurmurHash2(reinterpret_cast<const std::uint8_t*>(orig.data()), orig.size()),
              kExpectedOrigCharanameHash);

    const std::string patched = patch_charaname_xtx_cp932(orig);
    const std::string expected = to_cp932_str(
        "no,名前なし\r\n"
        "aa,Ｍｉｙａｋａ\r\n"
        "bb,Ｒｅｎ\r\n"
        "cc,Ａｙａｋａ\r\n"
        "dd,Ｙｕｒｉａ\r\n"
        "ee,Ｓｈｉｏ\r\n"
        "ff,Ｙｕｍｅｍｉ\r\n"
        "gg,Ｋａｚｅｎｏｍｉｙａ\r\n"
        "hh,Ｋｙｏｕ\r\n"
        "ii,Ｋｙｏｕ\r\n"
        "\r\n");
    EXPECT_EQ(patched, expected);
    EXPECT_EQ(patched.size(), kExpectedNewCharanameBytes);
    EXPECT_EQ(
        ComputeMurmurHash2(reinterpret_cast<const std::uint8_t*>(patched.data()), patched.size()),
        kExpectedNewCharanameHash);
}

TEST(MenuPatcherUnitTest, MurmurHash2AndXorMaskPayloadAreSelfInverse) {
    const auto orig = to_cp932_bytes("蟲狂編テストデータ");
    const auto payload = to_cp932_bytes("EXTRAVAGANZA ~Mushigurui~ English UI Payload");
    const std::uint32_t orig_hash = ComputeMurmurHash2(orig.data(), orig.size());

    auto buf = payload;
    XorMaskPayload(buf.data(), buf.size(), orig.data(), orig.size(), orig_hash);
    EXPECT_NE(buf, payload);

    XorMaskPayload(buf.data(), buf.size(), orig.data(), orig.size(), orig_hash);
    EXPECT_EQ(buf, payload);
}

TEST(MenuPatcherUnitTest, GtbDualOffsetTableUpdatesBoth32BitAnd64BitOffsets) {
    constexpr std::uint32_t kSyntheticEntryCount = 3u;
    constexpr std::size_t kPoolSizeBytes = 64u;
    constexpr std::uint8_t kPoolFillByte = 0x5Au;
    constexpr std::uint64_t kOrigEntry1Offset = 640ull;
    constexpr std::uint64_t kOrigEntry2Offset = 2048ull;
    constexpr std::uint64_t kOrigGpkSize = 4096ull;
    constexpr std::uint64_t kNewEntry1Offset = 1024ull;
    constexpr std::uint64_t kNewEntry2OffsetAbove4GiB = 0x100000480ull;
    constexpr std::uint64_t kNewGpkSizeAbove4GiB = 0x100001000ull;

    const std::vector<std::uint32_t> non_seq_names = {48u, 12u, 30u};
    const std::vector<std::uint64_t> orig_offs = {0ull, kOrigEntry1Offset, kOrigEntry2Offset};
    const std::vector<std::uint8_t> pool(kPoolSizeBytes, kPoolFillByte);
    const auto orig_gtb = make_synthetic_over2g_gtb(non_seq_names, orig_offs, pool);

    const auto parsed_orig = parse_gtb_offsets(orig_gtb, kOrigGpkSize);
    ASSERT_TRUE(parsed_orig.has_over2g_u64_table);
    ASSERT_EQ(parsed_orig.entry_count, kSyntheticEntryCount);
    EXPECT_EQ(parsed_orig.offsets[2], kOrigEntry2Offset);
    EXPECT_EQ(parsed_orig.offsets[kSyntheticEntryCount], kOrigGpkSize);

    const std::vector<std::uint64_t> new_offs = {
        0ull, kNewEntry1Offset, kNewEntry2OffsetAbove4GiB};
    const auto updated = update_gtb_offsets(orig_gtb, new_offs);
    ASSERT_EQ(updated.size(), orig_gtb.size());

    const std::size_t name_table_bytes = kSyntheticEntryCount * kU32ByteSize;
    const std::size_t pool_offset = kGtbCountFieldSize + name_table_bytes * 2u;
    EXPECT_EQ(std::memcmp(updated.data() + kGtbCountFieldSize,
                          orig_gtb.data() + kGtbCountFieldSize,
                          name_table_bytes),
              0);
    EXPECT_EQ(std::memcmp(updated.data() + pool_offset,
                          orig_gtb.data() + pool_offset,
                          pool.size()),
              0);

    const auto parsed_new = parse_gtb_offsets(updated, kNewGpkSizeAbove4GiB);
    EXPECT_TRUE(parsed_new.has_over2g_u64_table);
    EXPECT_EQ(parsed_new.offsets[0], 0ull);
    EXPECT_EQ(parsed_new.offsets[1], kNewEntry1Offset);
    EXPECT_EQ(parsed_new.offsets[2], kNewEntry2OffsetAbove4GiB);

    auto desynced = updated;
    desynced[desynced.size() - kGtbOver2GFooterPaddingBytes - kU64ByteSize] ^=
        static_cast<std::uint8_t>(kLowByteMask);
    EXPECT_THROW(parse_gtb_offsets(desynced, kNewGpkSizeAbove4GiB), std::runtime_error);
}

TEST(MenuPatcherUnitTest, DwqHeaderWrapperPreservesVerbatimMetadataAndUpdatesCropMargins) {
    constexpr int kTestWidth = 32;
    constexpr int kTestHeight = 24;
    constexpr std::int32_t kInitialSentinelMargin = 99;

    RgbaImage img;
    const auto orig_dwq =
        make_synthetic_dwq_entry(kTestWidth, kTestHeight, img, kInitialSentinelMargin);

    const auto new_png = encode_png(img, false);
    const auto wrapped_no_crop = wrap_dwq_png_preserving_header(orig_dwq, new_png, false, &img);
    EXPECT_EQ(std::memcmp(wrapped_no_crop.data(), orig_dwq.data(), kDwqDataSizeOffset), 0);
    EXPECT_EQ(std::memcmp(wrapped_no_crop.data() + kDwqWidthOffset,
                          orig_dwq.data() + kDwqWidthOffset,
                          kDwqHeaderSize - kDwqWidthOffset),
              0);

    const auto wrapped_with_crop = wrap_dwq_png_preserving_header(orig_dwq, new_png, true, &img);
    const auto margins = compute_rgba_crop_margins(img);
    EXPECT_EQ(margins[0], kSyntheticMarginTop);
    EXPECT_EQ(margins[1], kSyntheticMarginLeft);
    EXPECT_EQ(margins[2], kSyntheticMarginRight);
    EXPECT_EQ(margins[3], kSyntheticMarginBottom);
    EXPECT_EQ(std::memcmp(wrapped_with_crop.data() + kDwqWidthOffset,
                          orig_dwq.data() + kDwqWidthOffset,
                          kDwqHeaderSize - kDwqWidthOffset),
              0);
}

TEST(MenuPatcherUnitTest, SyntheticMsgMenu1PatchBinRoundTripsAndRejectsCorruption) {
    constexpr int kSprite0Width = 24;
    constexpr int kSprite0Height = 16;
    constexpr int kSprite1Width = 20;
    constexpr int kSprite1Height = 20;
    constexpr std::size_t kPoolBytes = 32u;
    constexpr std::size_t kAlphaChannelOffset = 3u;

    RgbaImage img0;
    RgbaImage img1;
    const auto dwq0 = make_synthetic_dwq_entry(kSprite0Width, kSprite0Height, img0);
    const auto dwq1 = make_synthetic_dwq_entry(kSprite1Width, kSprite1Height, img1);

    std::vector<std::uint8_t> orig_gpk = dwq0;
    orig_gpk.insert(orig_gpk.end(), dwq1.begin(), dwq1.end());
    const auto orig_gtb = make_synthetic_over2g_gtb(
        {20u, 4u},
        {0ull, static_cast<std::uint64_t>(dwq0.size())},
        std::vector<std::uint8_t>(kPoolBytes, 0u));

    img1.pixels[0] = kFullChannelByte;
    img1.pixels[kAlphaChannelOffset] = kFullChannelByte;
    const auto patched_dwq1 =
        wrap_dwq_png_preserving_header(dwq1, encode_png(img1, false), true, &img1);

    const auto new_gtb =
        update_gtb_offsets(orig_gtb, {0ull, static_cast<std::uint64_t>(dwq0.size())});

    ParsedMenuPatchBin bundle;
    StandaloneGroupRecord grp;
    grp.rel_path = "init2\\mainControl.fxf";
    const auto v0_orig = to_cp932_bytes("HD10_ORIG");
    const auto v0_new = to_cp932_bytes("HD10_PATCHED");
    StandaloneVariantRecord var0;
    var0.orig_file_size = static_cast<std::uint32_t>(v0_orig.size());
    var0.orig_data_hash = ComputeMurmurHash2(v0_orig.data(), v0_orig.size());
    var0.new_file_size = static_cast<std::uint32_t>(v0_new.size());
    var0.new_data_hash = ComputeMurmurHash2(v0_new.data(), v0_new.size());
    var0.masked_payload = v0_new;
    XorMaskPayload(var0.masked_payload.data(),
                   var0.masked_payload.size(),
                   v0_orig.data(),
                   v0_orig.size(),
                   var0.orig_data_hash);
    grp.variants.push_back(var0);
    bundle.standalone_groups.push_back(grp);

    ArchiveSectionRecord sec;
    sec.archive_stem = "dwq\\sys0";
    sec.orig_gpk_size = orig_gpk.size();
    sec.new_gpk_size = dwq0.size() + patched_dwq1.size();
    sec.orig_gtb_size = static_cast<std::uint32_t>(orig_gtb.size());
    sec.orig_gtb_hash = ComputeMurmurHash2(orig_gtb.data(), orig_gtb.size());
    sec.new_gtb_size = static_cast<std::uint32_t>(new_gtb.size());
    sec.new_gtb_hash = ComputeMurmurHash2(new_gtb.data(), new_gtb.size());
    sec.masked_gtb = new_gtb;
    XorMaskPayload(sec.masked_gtb.data(),
                   sec.masked_gtb.size(),
                   orig_gtb.data(),
                   orig_gtb.size(),
                   sec.orig_gtb_hash);

    ArchiveEntryRecord rec1;
    rec1.entry_index = 1u;
    rec1.orig_offset = dwq0.size();
    rec1.orig_size = static_cast<std::uint32_t>(dwq1.size());
    rec1.orig_data_hash = ComputeMurmurHash2(dwq1.data(), dwq1.size());
    rec1.new_size = static_cast<std::uint32_t>(patched_dwq1.size());
    rec1.new_data_hash = ComputeMurmurHash2(patched_dwq1.data(), patched_dwq1.size());
    rec1.masked_entry = patched_dwq1;
    XorMaskPayload(rec1.masked_entry.data(),
                   rec1.masked_entry.size(),
                   dwq1.data(),
                   dwq1.size(),
                   rec1.orig_data_hash);
    sec.entries.push_back(rec1);
    bundle.archives.push_back(sec);

    const auto bytes = serialize_menu_patch_bin(bundle);
    const auto parsed = parse_menu_patch_bin(bytes);

    EXPECT_EQ(reconstruct_standalone_file(parsed.standalone_groups[0], v0_orig), v0_new);
    const auto [rebuilt_gtb, rebuilt_gpk] =
        reconstruct_archive_pair(parsed.archives[0], orig_gtb, orig_gpk);
    EXPECT_EQ(rebuilt_gtb, new_gtb);
    EXPECT_EQ(rebuilt_gpk.size(), dwq0.size() + patched_dwq1.size());

    auto bad_magic = bytes;
    bad_magic[0] = 'X';
    EXPECT_THROW(parse_menu_patch_bin(bad_magic), std::runtime_error);

    auto bad_ver = bytes;
    bad_ver[kHeaderVersionOffset] = kInvalidFormatVersion;
    EXPECT_THROW(parse_menu_patch_bin(bad_ver), std::runtime_error);

    auto truncated = bytes;
    truncated.pop_back();
    EXPECT_THROW(parse_menu_patch_bin(truncated), std::runtime_error);

    auto trailing = bytes;
    trailing.push_back(0u);
    EXPECT_THROW(parse_menu_patch_bin(trailing), std::runtime_error);

    auto tampered_rec = parsed;
    tampered_rec.archives[0].entries[0].masked_entry[0] ^= kSingleBitFlipMask;
    EXPECT_THROW(reconstruct_archive_pair(tampered_rec.archives[0], orig_gtb, orig_gpk),
                 std::runtime_error);
}

}  // namespace mgi::menu
