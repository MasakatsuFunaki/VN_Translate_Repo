// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Functional-contract and component tests for FXF bitwise-NOT, MurmurHash2 +
// XOR-delta masking, RLE-XOR mask round-trip, and live WelcomeToAnzU!_CE
// dwq\sys0.gpk (184 entries) + ANZMENU1 verification.
#include "menu/menu_patcher.h"

#include "apps/paths.h"
#include "common/util.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using exm::menu::ComputeMurmurHash2;
using exm::menu::MenuArchivePatcher;
using exm::menu::XorMaskPayload;
using exm::menu::decode_rle_xor_mask;
using exm::menu::decrypt_fxf;
using exm::menu::encode_dwq_from_rgba;
using exm::menu::encode_rle_xor_mask;
using exm::menu::encrypt_fxf;
using exm::menu::kAnzUSys0EntryCount;
using exm::menu::kExpectedModifiedGpkEntryCount;
using exm::menu::kExpectedStandaloneFileCount;
using exm::menu::kPatchFormatVersion;
using exm::menu::kPatchMagic;
using exm::menu::kPatchMagicSize;
using exm::menu::parse_patch_bin;
using exm::menu::patch_backlog_fxf;
using exm::menu::patch_main_control_fxf;

constexpr int kMaskTestWidth = 16;
constexpr int kMaskTestHeight = 12;
constexpr std::size_t kMaskZeroModulus = 3;
constexpr std::uint32_t kMaskPatternMul = 37u;
constexpr std::uint32_t kMaskPatternAdd = 11u;
constexpr std::size_t kBacklogSpacesCount = 8;

constexpr std::size_t kIdxBgMinigame601 = 0;
constexpr std::size_t kIdxChartBack11 = 4;
constexpr std::size_t kIdxChartBack41 = 10;
constexpr std::size_t kIdxTaChartmapBt71 = 85;
constexpr std::size_t kIdxTaXtgCgcur02 = 109;
constexpr std::size_t kIdxTaXtgMeswin02 = 159;
constexpr std::size_t kIdxTaXtgTitbt04 = 177;

constexpr const char kPlainDwqSignature[] = "PACKBMP";
constexpr std::size_t kPlainDwqSignatureLen = 7;
constexpr std::uint8_t kPngSignature[4] = {0x89u, 'P', 'N', 'G'};
constexpr std::size_t kPngSignatureLen = 4;

bool ContainsSubsequence(const std::vector<std::uint8_t>& haystack,
                         const std::uint8_t* needle,
                         std::size_t needle_len) {
    if (needle_len == 0 || haystack.size() < needle_len) {
        return false;
    }
    for (std::size_t i = 0; i + needle_len <= haystack.size(); ++i) {
        if (std::memcmp(haystack.data() + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

TEST(MenuPatcherContract, FxfAndXorMaskAndRleXorMaskRoundTrips) {
    const std::string fxf_src = "name mainControl\r\nwindowTitle Welcome to AnzU! CE\r\n";
    EXPECT_EQ(decrypt_fxf(encrypt_fxf(fxf_src)), fxf_src);

    const std::vector<std::uint8_t> orig = {0x10, 0x20, 0x30, 0x40, 0x55, 0xAA};
    const std::vector<std::uint8_t> patched = {0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32};
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

fs::path ResolveLiveGameDir() {
    const char* env_dir = std::getenv("VN_GAME_DIR");
    return (env_dir && *env_dir) ? fs::u8path(env_dir) : fs::path();
}

TEST(MenuPatcherComponent, LiveSys0GpkDecodesAndReencodes24BitAnd2ASprites) {
    const fs::path game_dir = ResolveLiveGameDir();
    const fs::path gpk_path = game_dir / "dwq" / "sys0.gpk";
    const fs::path gtb_path = game_dir / "dwq" / "sys0.gtb";
    if (game_dir.empty() || !fs::exists(gpk_path) || !fs::exists(gtb_path)) {
        GTEST_SKIP() << "Live WelcomeToAnzU!_CE dwq\\sys0.{gpk,gtb} not present.";
    }

    MenuArchivePatcher patcher;
    patcher.load(gpk_path, gtb_path);
    ASSERT_EQ(patcher.entry_count(), kAnzUSys0EntryCount);
    EXPECT_EQ(patcher.entry_offset(kAnzUSys0EntryCount),
              static_cast<std::uint32_t>(fs::file_size(gpk_path)));
    EXPECT_EQ(patcher.entry_name(kIdxBgMinigame601), "bg_minigame6_01");
    EXPECT_EQ(patcher.entry_name(kIdxChartBack11), "chart_back1_1");
    EXPECT_EQ(patcher.entry_name(kIdxChartBack41), "chart_back4_1");
    EXPECT_EQ(patcher.entry_name(kIdxTaChartmapBt71), "ta_chartmap_bt71");

    // Decode all 184 entries (PACKTYPE=0 24bpp/8bpp, PACKTYPE=2A, PACKTYPE=3A)
    // and verify round-trip re-encoding on representative PACKTYPE=0 and 2A sprites.
    for (std::size_t idx = 0; idx < patcher.entry_count(); ++idx) {
        const auto img = patcher.decode_entry_rgba(idx);
        EXPECT_GT(img.width, 0u) << "entry " << idx;
        EXPECT_GT(img.height, 0u) << "entry " << idx;
        if (idx == kIdxBgMinigame601 || idx == kIdxChartBack41 ||
            idx == kIdxTaChartmapBt71 || idx == kIdxTaXtgCgcur02 ||
            idx == kIdxTaXtgMeswin02 || idx == kIdxTaXtgTitbt04) {
            const auto& raw = patcher.entry_raw_bytes(idx);
            const auto reenc = encode_dwq_from_rgba(raw.data(), raw.size(), img, true);
            const auto redec = exm::menu::decode_dwq_to_rgba(reenc.data(), reenc.size());
            EXPECT_EQ(redec.pixels, img.pixels) << "entry " << idx;
        }
    }
}

TEST(MenuPatcherComponent, AnzMenu1PatchBinObfuscationAndBitExactReconstruction) {
    const fs::path proj_root = fs::path(__FILE__).parent_path().parent_path().parent_path();
    const fs::path patch_bin_path = proj_root / "menu_patch.bin";
    ASSERT_TRUE(fs::exists(patch_bin_path))
        << "Expected menu_patch.bin at " << patch_bin_path.u8string();

    const auto patch_bytes = exm::read_file(patch_bin_path.u8string());
    ASSERT_GE(patch_bytes.size(), kPatchMagicSize);
    EXPECT_EQ(std::memcmp(patch_bytes.data(), kPatchMagic, kPatchMagicSize), 0);
    EXPECT_FALSE(ContainsSubsequence(
        patch_bytes,
        reinterpret_cast<const std::uint8_t*>(kPlainDwqSignature),
        kPlainDwqSignatureLen));
    EXPECT_FALSE(ContainsSubsequence(patch_bytes, kPngSignature, kPngSignatureLen));

    const auto parsed = parse_patch_bin(patch_bytes);
    EXPECT_EQ(parsed.version, kPatchFormatVersion);
    EXPECT_EQ(parsed.standalone_file_count, kExpectedStandaloneFileCount);
    EXPECT_EQ(parsed.gpk_entry_count, kExpectedModifiedGpkEntryCount);

    const fs::path game_dir = ResolveLiveGameDir();
    const fs::path orig_gtb_path = game_dir / "dwq_backup" / "sys0.gtb";
    const fs::path orig_gpk_path = game_dir / "dwq_backup" / "sys0.gpk";
    if (game_dir.empty() || !fs::exists(orig_gtb_path) || !fs::exists(orig_gpk_path)) {
        GTEST_SKIP() << "Live WelcomeToAnzU!_CE dwq_backup not present.";
    }

    const auto orig_gtb = exm::read_file(orig_gtb_path.u8string());
    const auto orig_gpk = exm::read_file(orig_gpk_path.u8string());
    const auto [rebuilt_gtb, rebuilt_gpk] =
        MenuArchivePatcher::reconstruct_archive_from_patch_bin(
            patch_bytes, orig_gtb, orig_gpk);
    EXPECT_EQ(rebuilt_gtb.size(), parsed.new_gtb_size);
    EXPECT_EQ(rebuilt_gpk.size(), parsed.new_gpk_size);

    const auto summary =
        MenuArchivePatcher::apply_patch_bin_to_game_dir(game_dir, patch_bin_path, true);
    EXPECT_EQ(summary.total_archive_entries, kAnzUSys0EntryCount);
    EXPECT_EQ(summary.patched_sprite_entries, kExpectedModifiedGpkEntryCount);
    EXPECT_EQ(summary.untouched_sprite_entries,
              kAnzUSys0EntryCount - kExpectedModifiedGpkEntryCount);
    EXPECT_EQ(summary.patched_standalone_files, kExpectedStandaloneFileCount);
}

}  // namespace
