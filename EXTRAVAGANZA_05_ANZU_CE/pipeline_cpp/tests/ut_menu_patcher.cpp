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

}  // namespace
