// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pure standalone unit tests for FXF bitwise-NOT, MurmurHash2 + XOR-delta
// masking, RLE-XOR mask round-trip, and FXF text patching for WelcomeToAnzU!_CE.
#include "menu/menu_patcher.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using exm::menu::ComputeMurmurHash2;
using exm::menu::XorMaskPayload;
using exm::menu::decode_rle_xor_mask;
using exm::menu::decrypt_fxf;
using exm::menu::encode_rle_xor_mask;
using exm::menu::encrypt_fxf;
using exm::menu::patch_backlog_fxf;
using exm::menu::patch_main_control_fxf;

constexpr int kMaskTestWidth = 16;
constexpr int kMaskTestHeight = 12;
constexpr std::size_t kMaskZeroModulus = 3;
constexpr std::uint32_t kMaskPatternMul = 37u;
constexpr std::uint32_t kMaskPatternAdd = 11u;
constexpr std::size_t kBacklogSpacesCount = 8;

constexpr std::uint8_t kOrigByte0 = 0x10;
constexpr std::uint8_t kOrigByte1 = 0x20;
constexpr std::uint8_t kOrigByte2 = 0x30;
constexpr std::uint8_t kOrigByte3 = 0x40;
constexpr std::uint8_t kOrigByte4 = 0x55;
constexpr std::uint8_t kOrigByte5 = 0xAA;

constexpr std::uint8_t kPatchedByte0 = 0xFE;
constexpr std::uint8_t kPatchedByte1 = 0xDC;
constexpr std::uint8_t kPatchedByte2 = 0xBA;
constexpr std::uint8_t kPatchedByte3 = 0x98;
constexpr std::uint8_t kPatchedByte4 = 0x76;
constexpr std::uint8_t kPatchedByte5 = 0x54;
constexpr std::uint8_t kPatchedByte6 = 0x32;

TEST(MenuPatcherContract, FxfAndXorMaskAndRleXorMaskRoundTrips) {
    const std::string fxf_src = "name mainControl\r\nwindowTitle Welcome to AnzU! CE\r\n";
    EXPECT_EQ(decrypt_fxf(encrypt_fxf(fxf_src)), fxf_src);

    const std::vector<std::uint8_t> orig = {
        kOrigByte0, kOrigByte1, kOrigByte2, kOrigByte3, kOrigByte4, kOrigByte5
    };
    const std::vector<std::uint8_t> patched = {
        kPatchedByte0, kPatchedByte1, kPatchedByte2, kPatchedByte3,
        kPatchedByte4, kPatchedByte5, kPatchedByte6
    };
    const std::uint32_t orig_hash = ComputeMurmurHash2(orig.data(), orig.size());
    std::vector<std::uint8_t> masked = patched;
    XorMaskPayload(masked.data(), masked.size(), orig.data(), orig.size(), orig_hash);
    EXPECT_NE(masked, patched);
    XorMaskPayload(masked.data(), masked.size(), orig.data(), orig.size(), orig_hash);
    EXPECT_EQ(masked, patched);

    std::vector<std::uint8_t> alpha(
        static_cast<std::size_t>(kMaskTestWidth) * kMaskTestHeight, 0);
    for (std::size_t i = 0; i < alpha.size(); ++i) {
        alpha[i] = (i % kMaskZeroModulus == 0)
                       ? 0u
                       : static_cast<std::uint8_t>(i * kMaskPatternMul + kMaskPatternAdd);
    }
    const auto rle = encode_rle_xor_mask(alpha.data(), kMaskTestWidth, kMaskTestHeight);
    const auto dec_alpha =
        decode_rle_xor_mask(rle.data(), rle.size(), kMaskTestWidth, kMaskTestHeight);
    EXPECT_EQ(dec_alpha, alpha);
}

TEST(MenuPatcherContract, PatchMainControlAndBacklogFxfForAnzU) {
    const std::string src_main = exm::utf8_to_cp932(
        "windowTitle うぇるかむとぅＡｎｚＵ！_CE版\r\n"
        "windowTitleTaiken うぇるかむとぅＡｎｚＵ！_CE版\r\n"
        "mutexName BCYCWELCOMETOANZU_CE\r\n"
        "mutexErrorMessage ゲームの二重起動はできません\r\n"
        "mutexErrorTitle 二重起動エラー\r\n"
        "className bcycWELCOMETOANZU_CE\r\n"
        "bppErrorMessage 16bitカラー以上に設定してください\r\n"
        "bppErrorTitle Error\r\n");
    const std::string patched_main = patch_main_control_fxf(src_main);
    EXPECT_NE(patched_main.find("windowTitle Welcome to AnzU! CE\r\n"), std::string::npos);
    EXPECT_NE(patched_main.find("windowTitleTaiken Welcome to AnzU! CE Trial\r\n"),
              std::string::npos);
    EXPECT_NE(patched_main.find("mutexErrorMessage The game is already running.\r\n"),
              std::string::npos);
    EXPECT_NE(patched_main.find("mutexErrorTitle Already Running\r\n"),
              std::string::npos);
    EXPECT_NE(patched_main.find(
                  "bppErrorMessage Please set display to 16-bit color or higher.\r\n"),
              std::string::npos);

    const std::string src_backlog = exm::utf8_to_cp932(
        "modename backlog\r\n"
        "barMessage #青――――――――――――――――――――――――\r\n"
        "firstMessageNumber 1\r\n"
        "firstMessage1 #青　　　　　　　　　　育成記録\r\n");
    const std::string patched_backlog = patch_backlog_fxf(src_backlog);
    std::string spaces;
    for (std::size_t i = 0; i < kBacklogSpacesCount; ++i) {
        spaces += exm::utf8_to_cp932("　");
    }
    const std::string expected_backlog =
        exm::utf8_to_cp932("firstMessage1 #青") + spaces +
        exm::utf8_to_cp932("Ｂａｃｋｌｏｇ\r\n");
    EXPECT_NE(patched_backlog.find(expected_backlog), std::string::npos);
}

TEST(MenuPatcherContract, EncodeDwqFromPngBytesValidatesColorTypeAndSignature) {
    constexpr std::size_t kDwqHeaderSize = 64u;
    constexpr std::size_t kDwqTypeFieldLen = 16u;
    constexpr std::size_t kDwqDataSizeOffset = 0x20u;
    constexpr std::size_t kDwqWidthOffset = 0x24u;
    constexpr std::size_t kDwqHeightOffset = 0x28u;
    constexpr std::size_t kDwqBppOffset = 0x2Au;
    constexpr std::size_t kDwqPackStrOffset = 0x2Cu;
    constexpr std::size_t kDwqPackStrLen = 20u;
    constexpr std::uint16_t kDim = 4u;
    constexpr std::size_t kRgbaChannels = 4u;
    constexpr std::uint16_t kBpp24 = 24u;
    constexpr std::uint16_t kBpp32 = 32u;
    constexpr std::uint8_t kOpaqueFill = 0xFFu;
    constexpr std::uint32_t kByteMask = 0xFFu;
    constexpr unsigned int kShiftByte1 = 8u;
    constexpr unsigned int kShiftByte2 = 16u;
    constexpr unsigned int kShiftByte3 = 24u;
    constexpr std::size_t kByteIndex0 = 0u;
    constexpr std::size_t kByteIndex1 = 1u;
    constexpr std::size_t kByteIndex2 = 2u;
    constexpr std::size_t kByteIndex3 = 3u;
    constexpr std::size_t kPngIhdrTagOffset = 12u;
    constexpr std::size_t kPngIhdrColorTypeOffset = 25u;
    constexpr std::size_t kPngMinHeaderBytes = 26u;
    constexpr std::uint8_t kPngColorTypeRgba = 6u;

    auto make_png_dwq = [&](const std::vector<std::uint8_t>& png_bytes, std::uint16_t bpp) {
        std::vector<std::uint8_t> dwq(kDwqHeaderSize, 0u);
        std::memcpy(dwq.data(), "PNG             ", kDwqTypeFieldLen);
        const std::uint32_t sz = static_cast<std::uint32_t>(png_bytes.size());
        dwq[kDwqDataSizeOffset + kByteIndex0] = static_cast<std::uint8_t>(sz & kByteMask);
        dwq[kDwqDataSizeOffset + kByteIndex1] =
            static_cast<std::uint8_t>((sz >> kShiftByte1) & kByteMask);
        dwq[kDwqDataSizeOffset + kByteIndex2] =
            static_cast<std::uint8_t>((sz >> kShiftByte2) & kByteMask);
        dwq[kDwqDataSizeOffset + kByteIndex3] =
            static_cast<std::uint8_t>((sz >> kShiftByte3) & kByteMask);
        dwq[kDwqWidthOffset] = static_cast<std::uint8_t>(kDim);
        dwq[kDwqHeightOffset] = static_cast<std::uint8_t>(kDim);
        dwq[kDwqBppOffset] = static_cast<std::uint8_t>(bpp);
        std::memcpy(dwq.data() + kDwqPackStrOffset, "    PACKTYPE=8A     ", kDwqPackStrLen);
        dwq.insert(dwq.end(), png_bytes.begin(), png_bytes.end());
        return dwq;
    };

    exm::menu::RgbaImage img;
    img.width = kDim;
    img.height = kDim;
    img.bpp = kBpp32;
    img.has_alpha = true;
    img.pixels.assign(static_cast<std::size_t>(kDim) * kDim * kRgbaChannels, kOpaqueFill);
    const auto png_rgba = exm::menu::encode_rgba_to_png_bytes(img, true);
    const auto png_rgb = exm::menu::encode_rgba_to_png_bytes(img, false);

    const auto orig_rgba_dwq = make_png_dwq(png_rgba, kBpp32);
    const auto orig_rgb_dwq = make_png_dwq(png_rgb, kBpp24);

    const auto ok_rgba = exm::menu::encode_dwq_from_png_bytes(
        orig_rgba_dwq.data(), orig_rgba_dwq.size(), png_rgba, true);
    ASSERT_EQ(ok_rgba.size(), kDwqHeaderSize + png_rgba.size());
    EXPECT_EQ(std::memcmp(ok_rgba.data() + kDwqHeaderSize, png_rgba.data(), png_rgba.size()), 0);

    const auto ok_rgb = exm::menu::encode_dwq_from_png_bytes(
        orig_rgb_dwq.data(), orig_rgb_dwq.size(), png_rgb, true);
    ASSERT_EQ(ok_rgb.size(), kDwqHeaderSize + png_rgb.size());
    EXPECT_EQ(std::memcmp(ok_rgb.data() + kDwqHeaderSize, png_rgb.data(), png_rgb.size()), 0);

    EXPECT_THROW(
        exm::menu::encode_dwq_from_png_bytes(
            orig_rgba_dwq.data(), orig_rgba_dwq.size(), png_rgb, true),
        std::runtime_error);
    EXPECT_THROW(
        exm::menu::encode_dwq_from_png_bytes(
            orig_rgb_dwq.data(), orig_rgb_dwq.size(), png_rgba, true),
        std::runtime_error);

    std::vector<std::uint8_t> non_png_matching_color(kPngMinHeaderBytes + kByteIndex2, 0u);
    non_png_matching_color[kByteIndex0] = 'B';
    non_png_matching_color[kByteIndex1] = 'M';
    non_png_matching_color[kPngIhdrColorTypeOffset] = kPngColorTypeRgba;
    EXPECT_THROW(
        exm::menu::encode_dwq_from_png_bytes(
            orig_rgba_dwq.data(), orig_rgba_dwq.size(), non_png_matching_color, true),
        std::runtime_error);

    std::vector<std::uint8_t> bad_chunk_tag = png_rgba;
    bad_chunk_tag[kPngIhdrTagOffset] = 'X';
    ASSERT_EQ(bad_chunk_tag[kPngIhdrColorTypeOffset], kPngColorTypeRgba);
    EXPECT_THROW(
        exm::menu::encode_dwq_from_png_bytes(
            orig_rgba_dwq.data(), orig_rgba_dwq.size(), bad_chunk_tag, true),
        std::runtime_error);

    const std::vector<std::uint8_t> truncated_png(
        png_rgba.begin(),
        png_rgba.begin() + static_cast<std::ptrdiff_t>(kPngMinHeaderBytes - kByteIndex1));
    EXPECT_THROW(
        exm::menu::encode_dwq_from_png_bytes(
            orig_rgba_dwq.data(), orig_rgba_dwq.size(), truncated_png, true),
        std::runtime_error);
}

}  // namespace
