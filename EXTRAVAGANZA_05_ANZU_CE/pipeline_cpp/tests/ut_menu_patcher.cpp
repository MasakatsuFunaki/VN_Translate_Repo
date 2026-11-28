// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pure unit tests for FXF bitwise-NOT, canonical MurmurHash2 + XOR-delta
// masking, crop-margin calculation, RLE-XOR mask round-trip, FXF text patching,
// and DWQ PNG validation for WelcomeToAnzU!_CE.
#include "menu/menu_patcher.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using exm::menu::ComputeMurmurHash2;
using exm::menu::XorMaskPayload;
using exm::menu::compute_crop_margins;
using exm::menu::decode_rle_xor_mask;
using exm::menu::decrypt_fxf;
using exm::menu::encode_rle_xor_mask;
using exm::menu::encrypt_fxf;
using exm::menu::patch_backlog_fxf;
using exm::menu::patch_main_control_fxf;

constexpr std::size_t kBacklogSpacesCount = 8;

constexpr std::uint32_t kMurmurEmpty = 0x00000000u;
constexpr std::uint32_t kMurmurOne = 0x00000000u;
constexpr std::uint32_t kMurmurSign80 = 0x9B891BB0u;
constexpr std::uint32_t kMurmurHello = 0xE56129CBu;
constexpr std::uint32_t kMurmurSign4 = 0x26824341u;
constexpr std::uint32_t kMurmurTail3 = 0x3AA63325u;

TEST(MenuPatcherContract, FxfMurmurHash2XorMaskCropMarginsAndRleMaskPinnedVectors) {
    // 1. Pinned bitwise-NOT FXF bytes across sign-bit seam (prevents symmetric-bug pass).
    const std::string fxf_seam("\x00\x41\x7F\x80\xFF", 5);
    const std::vector<std::uint8_t> expected_fxf_enc = {0xFFu, 0xBEu, 0x80u, 0x7Fu, 0x00u};
    const std::vector<std::uint8_t> actual_fxf_enc = encrypt_fxf(fxf_seam);
    EXPECT_EQ(actual_fxf_enc, expected_fxf_enc);
    EXPECT_EQ(decrypt_fxf(actual_fxf_enc), fxf_seam);

    // 2. Canonical Austin Appleby MurmurHash2 (m=0x5BD1E995, r=24, seed=0) vectors
    //    covering empty, 1-byte sign-bit (0x80), 3-byte tail, 4-byte sign-bit, and 5-byte word+tail.
    const std::uint8_t one_byte[] = {0x01u};
    const std::uint8_t sign80[] = {0x80u};
    const std::uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
    const std::uint8_t sign4[] = {0x00u, 0x7Fu, 0x80u, 0xFFu};
    const std::uint8_t tail3[] = {0x01u, 0x02u, 0x03u};
    EXPECT_EQ(ComputeMurmurHash2(nullptr, 0), kMurmurEmpty);
    EXPECT_EQ(ComputeMurmurHash2(one_byte, sizeof(one_byte)), kMurmurOne);
    EXPECT_EQ(ComputeMurmurHash2(sign80, sizeof(sign80)), kMurmurSign80);
    EXPECT_EQ(ComputeMurmurHash2(hello, sizeof(hello)), kMurmurHello);
    EXPECT_EQ(ComputeMurmurHash2(sign4, sizeof(sign4)), kMurmurSign4);
    EXPECT_EQ(ComputeMurmurHash2(tail3, sizeof(tail3)), kMurmurTail3);

    // 3. Pinned 4-byte repeating XOR mask + pinned 5-arg keyless delta XOR vector
    //    (matching proxy_dll ANZMENU1 consumer spec across i >= orig_len wraparound).
    std::vector<std::uint8_t> key_buf = {0x10u, 0x20u, 0x30u, 0x40u, 0x50u};
    XorMaskPayload(key_buf.data(), key_buf.size(), 0x04030201u);
    const std::vector<std::uint8_t> expected_key_xor = {0x11u, 0x22u, 0x33u, 0x44u, 0x51u};
    EXPECT_EQ(key_buf, expected_key_xor);

    const std::vector<std::uint8_t> orig_wrap = {0x10u, 0x80u, 0xFFu};
    const std::vector<std::uint8_t> patched_wrap = {0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu};
    const std::vector<std::uint8_t> expected_keyless_masked = {
        0x8Cu, 0xECu, 0x43u, 0xD8u, 0xC0u};
    std::vector<std::uint8_t> masked = patched_wrap;
    XorMaskPayload(masked.data(), masked.size(), orig_wrap.data(), orig_wrap.size(), 0x04030201u);
    EXPECT_EQ(masked, expected_keyless_masked);
    XorMaskPayload(masked.data(), masked.size(), orig_wrap.data(), orig_wrap.size(), 0x04030201u);
    EXPECT_EQ(masked, patched_wrap);

    // 4. Crop margin calculation: opaque island x in [1..3], y in [2..3] inside 6x5 RGBA.
    constexpr std::uint16_t kCropW = 6u;
    constexpr std::uint16_t kCropH = 5u;
    constexpr std::size_t kRgbaStride = 4u;
    exm::menu::RgbaImage crop_img;
    crop_img.width = kCropW;
    crop_img.height = kCropH;
    crop_img.bpp = 32u;
    crop_img.has_alpha = true;
    crop_img.pixels.assign(static_cast<std::size_t>(kCropW) * kCropH * kRgbaStride, 0u);
    for (int y = 2; y <= 3; ++y) {
        for (int x = 1; x <= 3; ++x) {
            const std::size_t idx =
                (static_cast<std::size_t>(y) * kCropW + static_cast<std::size_t>(x)) * kRgbaStride;
            crop_img.pixels[idx + 3u] = 0xFFu;
        }
    }
    EXPECT_EQ(compute_crop_margins(crop_img), (std::array<std::int32_t, 4>{2, 1, 2, 1}));

    // 5. Pinned RLE-XOR wire bytes (4x2 delta bitmap + uniform zero mask) and error bounds.
    constexpr int kSmallMaskW = 4;
    constexpr int kSmallMaskH = 2;
    const std::vector<std::uint8_t> small_alpha = {
        0x00u, 0x00u, 0xFFu, 0x80u,
        0x00u, 0x00u, 0xFFu, 0x00u,
    };
    const std::vector<std::uint8_t> expected_small_rle = {
        0x00u, 0x02u, 0xFFu, 0x80u,
        0x00u, 0x03u, 0x80u,
    };
    const auto small_rle = encode_rle_xor_mask(small_alpha.data(), kSmallMaskW, kSmallMaskH);
    EXPECT_EQ(small_rle, expected_small_rle);
    EXPECT_EQ(decode_rle_xor_mask(small_rle.data(), small_rle.size(), kSmallMaskW, kSmallMaskH),
              small_alpha);

    const std::vector<std::uint8_t> zero_alpha(
        static_cast<std::size_t>(kSmallMaskW) * kSmallMaskH, 0u);
    const std::vector<std::uint8_t> expected_zero_rle = {0x00u, 0x04u, 0x00u, 0x04u};
    const auto zero_rle = encode_rle_xor_mask(zero_alpha.data(), kSmallMaskW, kSmallMaskH);
    EXPECT_EQ(zero_rle, expected_zero_rle);
    EXPECT_EQ(decode_rle_xor_mask(zero_rle.data(), zero_rle.size(), kSmallMaskW, kSmallMaskH),
              zero_alpha);

    const std::vector<std::uint8_t> truncated_stream = {0x00u, 0x02u, 0xFFu};
    const std::vector<std::uint8_t> truncated_zero_run = {0x00u};
    const std::vector<std::uint8_t> overflow_zero_run = {0x00u, 0x05u};
    EXPECT_THROW(
        decode_rle_xor_mask(truncated_stream.data(), truncated_stream.size(),
                            kSmallMaskW, kSmallMaskH),
        std::runtime_error);
    EXPECT_THROW(
        decode_rle_xor_mask(truncated_zero_run.data(), truncated_zero_run.size(),
                            kSmallMaskW, 1),
        std::runtime_error);
    EXPECT_THROW(
        decode_rle_xor_mask(overflow_zero_run.data(), overflow_zero_run.size(),
                            kSmallMaskW, 1),
        std::runtime_error);
}

TEST(MenuPatcherContract, PatchMainControlAndBacklogFxfProduceExactOutput) {
    const std::string src_main = exm::utf8_to_cp932(
        "windowTitle うぇるかむとぅＡｎｚＵ！_CE版\r\n"
        "windowTitleTaiken うぇるかむとぅＡｎｚＵ！_CE版\r\n"
        "mutexName BCYCWELCOMETOANZU_CE\r\n"
        "mutexErrorMessage ゲームの二重起動はできません\r\n"
        "mutexErrorTitle 二重起動エラー\r\n"
        "className bcycWELCOMETOANZU_CE\r\n"
        "bppErrorMessage 16bitカラー以上に設定してください\r\n"
        "bppErrorTitle Error\r\n");
    const std::string expected_main = exm::utf8_to_cp932(
        "windowTitle Welcome to AnzU! CE\r\n"
        "windowTitleTaiken Welcome to AnzU! CE Trial\r\n"
        "mutexName BCYCWELCOMETOANZU_CE\r\n"
        "mutexErrorMessage The game is already running.\r\n"
        "mutexErrorTitle Already Running\r\n"
        "className bcycWELCOMETOANZU_CE\r\n"
        "bppErrorMessage Please set display to 16-bit color or higher.\r\n"
        "bppErrorTitle Error\r\n");
    EXPECT_EQ(patch_main_control_fxf(src_main), expected_main);

    const std::string src_backlog = exm::utf8_to_cp932(
        "modename backlog\r\n"
        "barMessage #青――――――――――――――――――――――――\r\n"
        "firstMessageNumber 1\r\n"
        "firstMessage1 #青　　　　　　　　　　育成記録\r\n");
    std::string spaces;
    for (std::size_t i = 0; i < kBacklogSpacesCount; ++i) {
        spaces += exm::utf8_to_cp932("　");
    }
    const std::string expected_backlog =
        exm::utf8_to_cp932(
            "modename backlog\r\n"
            "barMessage #青――――――――――――――――――――――――\r\n"
            "firstMessageNumber 1\r\n"
            "firstMessage1 #青") +
        spaces + exm::utf8_to_cp932("Ｂａｃｋｌｏｇ\r\n");
    EXPECT_EQ(patch_backlog_fxf(src_backlog), expected_backlog);
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
