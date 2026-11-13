// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// System tests verifying live EXTRAVAGANZA_MATSURO_CE archive decoding and
// MATMENU1 v1 menu_patch.bin reconstruction when VN_GAME_DIR is provided.
#include "common/util.h"
#include "menu/menu_patcher.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using exm::menu::ComputeMurmurHash2;
using exm::menu::MenuArchivePatcher;
using exm::menu::decode_dwq_to_rgba;
using exm::menu::encode_dwq_from_rgba;
using exm::menu::parse_patch_bin;

constexpr std::size_t kExpectedSys0EntryCount = 83;
constexpr std::size_t kSamplePack5Index = 4;
constexpr std::size_t kSamplePack3aIndex = 39;
constexpr std::size_t kSamplePack2aIndex = 0;

fs::path ResolveLiveGameDir() {
    const wchar_t* wide_env = _wgetenv(L"VN_GAME_DIR");
    if (wide_env == nullptr || *wide_env == L'\0') {
        return {};
    }
    const std::wstring_view wide_view(wide_env);
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide_view.data(),
        static_cast<int>(wide_view.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide_view.data(),
        static_cast<int>(wide_view.size()), utf8.data(), needed, nullptr, nullptr);
    if (written != needed) {
        return {};
    }
    return fs::u8path(utf8);
}

fs::path FindCommittedPatchBin() {
    std::error_code ec;
    const fs::path roots[] = {fs::current_path(ec), fs::u8path(__FILE__).parent_path()};
    for (const fs::path& base : roots) {
        for (fs::path cur = base; !cur.empty(); cur = cur.parent_path()) {
            const fs::path direct = cur / "menu_patch.bin";
            if (fs::is_regular_file(direct, ec)) {
                return direct;
            }
            const fs::path nested = cur / "EXTRAVAGANZA_03_MATSURO_CE" / "menu_patch.bin";
            if (fs::is_regular_file(nested, ec)) {
                return nested;
            }
            if (cur == cur.root_path()) {
                break;
            }
        }
    }
    return {};
}

TEST(MenuPatcherSystem, LiveSys0GpkAndFxfVerificationWhenGameDirPresent) {
    const fs::path game_dir = ResolveLiveGameDir();
    const fs::path gpk_path = game_dir / "dwq" / "sys0.gpk";
    const fs::path gtb_path = game_dir / "dwq" / "sys0.gtb";
    if (game_dir.empty() || !fs::exists(gpk_path) || !fs::exists(gtb_path)) {
        GTEST_SKIP()
            << "Live EXTRAVAGANZA_MATSURO_CE game directory not present on this machine.";
    }

    MenuArchivePatcher patcher;
    patcher.load(gpk_path, gtb_path);
    ASSERT_EQ(patcher.entry_count(), kExpectedSys0EntryCount);
    EXPECT_EQ(patcher.entry_offset(kExpectedSys0EntryCount),
              static_cast<std::uint32_t>(fs::file_size(gpk_path)));

    for (std::size_t idx = 0; idx < kExpectedSys0EntryCount; ++idx) {
        auto img = patcher.decode_entry_rgba(idx);
        EXPECT_GT(img.width, 0u) << "entry index=" << idx;
        EXPECT_GT(img.height, 0u) << "entry index=" << idx;
    }

    const auto& raw_5 = patcher.entry_raw_bytes(kSamplePack5Index);
    const auto img_5 = patcher.decode_entry_rgba(kSamplePack5Index);
    const auto reenc_5 =
        encode_dwq_from_rgba(raw_5.data(), raw_5.size(), img_5, true);
    const auto redec_5 = decode_dwq_to_rgba(reenc_5.data(), reenc_5.size());
    EXPECT_EQ(redec_5.width, img_5.width);
    EXPECT_EQ(redec_5.height, img_5.height);

    const auto& raw_3a = patcher.entry_raw_bytes(kSamplePack3aIndex);
    const auto img_3a = patcher.decode_entry_rgba(kSamplePack3aIndex);
    const auto reenc_3a =
        encode_dwq_from_rgba(raw_3a.data(), raw_3a.size(), img_3a, true);
    const auto redec_3a = decode_dwq_to_rgba(reenc_3a.data(), reenc_3a.size());
    EXPECT_EQ(redec_3a.pixels, img_3a.pixels);

    const auto& raw_2a = patcher.entry_raw_bytes(kSamplePack2aIndex);
    const auto img_2a = patcher.decode_entry_rgba(kSamplePack2aIndex);
    const auto reenc_2a =
        encode_dwq_from_rgba(raw_2a.data(), raw_2a.size(), img_2a, true);
    const auto redec_2a = decode_dwq_to_rgba(reenc_2a.data(), reenc_2a.size());
    EXPECT_EQ(redec_2a.pixels, img_2a.pixels);
}

TEST(MenuPatcherSystem, CommittedMenuPatchBinReconstructsLiveArchiveBitForBit) {
    const fs::path game_dir = ResolveLiveGameDir();
    if (game_dir.empty() || !fs::exists(game_dir / "dwq" / "sys0.gpk") ||
        !fs::exists(game_dir / "dwq" / "sys0.gtb")) {
        GTEST_SKIP()
            << "Live EXTRAVAGANZA_MATSURO_CE game directory not present on this machine.";
    }
    const fs::path patch_bin_path = FindCommittedPatchBin();
    ASSERT_FALSE(patch_bin_path.empty());

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

    const auto patch_bin_bytes = exm::read_file(patch_bin_path.u8string());
    const auto orig_gtb_bytes = exm::read_file(orig_gtb_path.u8string());
    const auto orig_gpk_bytes = exm::read_file(orig_gpk_path.u8string());

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
