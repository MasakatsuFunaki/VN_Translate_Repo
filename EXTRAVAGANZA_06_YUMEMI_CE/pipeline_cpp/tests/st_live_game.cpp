// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// System tests (SYSTEM) verifying live YUMEMIRYOJOKU_CE game archives and SPT
// scripts when VN_GAME_DIR is configured.
#include "common/util.h"
#include "menu/menu_patcher.h"
#include "spt/spt_script.h"

#include <gtest/gtest.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;

constexpr int kMaxWalkLevels = 6;
constexpr std::size_t kExpectedSys0EntryCount = 184;
constexpr std::size_t kSampleEntryCount = 3;
constexpr std::size_t kSamplePack0Index = 0;
constexpr std::size_t kSamplePack2aIndex = 1;
constexpr std::size_t kSamplePack3aIndex = 89;
constexpr std::uint32_t kExpectedTokutenHeaderStrings = 689u;
constexpr std::size_t kMinTokutenParsedStrings = 1000u;

fs::path resolve_live_game_dir() {
    const wchar_t* wide_env = _wgetenv(L"VN_GAME_DIR");
    if (wide_env == nullptr || *wide_env == L'\0') {
        return {};
    }
    const std::wstring_view wide(wide_env);
    const int byte_len = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (byte_len <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(byte_len), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), utf8.data(), byte_len, nullptr, nullptr);
    if (written != byte_len) {
        return {};
    }
    return fs::u8path(utf8);
}

fs::path find_committed_patch_bin() {
    fs::path cur = fs::current_path();
    for (int i = 0; i < kMaxWalkLevels; ++i) {
        if (fs::exists(cur / "menu_patch.bin")) {
            return cur / "menu_patch.bin";
        }
        if (fs::exists(cur / "EXTRAVAGANZA_06_YUMEMI_CE" / "menu_patch.bin")) {
            return cur / "EXTRAVAGANZA_06_YUMEMI_CE" / "menu_patch.bin";
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) {
            break;
        }
        cur = cur.parent_path();
    }
    return {};
}

TEST(LiveGameSystemTest, LiveSys0GpkAndFxfVerificationWhenGameDirPresent) {
    const fs::path game_dir = resolve_live_game_dir();
    const fs::path gpk_path = game_dir / "dwq" / "sys0.gpk";
    const fs::path gtb_path = game_dir / "dwq" / "sys0.gtb";
    if (game_dir.empty() || !fs::exists(gpk_path) || !fs::exists(gtb_path)) {
        GTEST_SKIP() << "Live YUMEMIRYOJOKU_CE game directory not present on this machine.";
    }

    exm::menu::MenuArchivePatcher patcher;
    patcher.load(gpk_path, gtb_path);
    ASSERT_EQ(patcher.entry_count(), kExpectedSys0EntryCount);
    EXPECT_EQ(patcher.entry_offset(kExpectedSys0EntryCount),
              static_cast<std::uint32_t>(fs::file_size(gpk_path)));

    const std::array<std::size_t, kSampleEntryCount> kSampleIndices = {
        kSamplePack0Index,
        kSamplePack2aIndex,
        kSamplePack3aIndex,
    };
    for (std::size_t idx : kSampleIndices) {
        auto img = patcher.decode_entry_rgba(idx);
        EXPECT_GT(img.width, 0u) << "entry index=" << idx;
        EXPECT_GT(img.height, 0u) << "entry index=" << idx;
    }

    const auto& raw_logo = patcher.entry_raw_bytes(kSamplePack2aIndex);
    const auto img_logo = patcher.decode_entry_rgba(kSamplePack2aIndex);
    const auto reenc_logo = exm::menu::encode_dwq_from_rgba(
        raw_logo.data(), raw_logo.size(), img_logo, true);
    const auto redec_logo =
        exm::menu::decode_dwq_to_rgba(reenc_logo.data(), reenc_logo.size());
    EXPECT_EQ(redec_logo.pixels, img_logo.pixels);
}

TEST(LiveGameSystemTest, CommittedMenuPatchBinReconstructsLiveArchiveBitForBit) {
    const fs::path game_dir = resolve_live_game_dir();
    if (game_dir.empty() || !fs::exists(game_dir / "dwq" / "sys0.gpk") ||
        !fs::exists(game_dir / "dwq" / "sys0.gtb")) {
        GTEST_SKIP() << "Live YUMEMIRYOJOKU_CE game directory not present on this machine.";
    }
    const fs::path patch_bin_path = find_committed_patch_bin();
    if (patch_bin_path.empty()) {
        GTEST_SKIP() << "EXTRAVAGANZA_06_YUMEMI_CE/menu_patch.bin not generated yet.";
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

    const auto patch_bin_bytes = exm::read_file(patch_bin_path.u8string());
    const auto orig_gtb_bytes = exm::read_file(orig_gtb_path.u8string());
    const auto orig_gpk_bytes = exm::read_file(orig_gpk_path.u8string());

    const auto [built_gtb, built_gpk] =
        exm::menu::MenuArchivePatcher::reconstruct_archive_from_patch_bin(
            patch_bin_bytes, orig_gtb_bytes, orig_gpk_bytes);

    const auto parsed = exm::menu::parse_patch_bin(patch_bin_bytes);
    EXPECT_EQ(built_gpk.size(), parsed.new_gpk_size);
    EXPECT_EQ(built_gtb.size(), parsed.new_gtb_size);
    EXPECT_EQ(exm::menu::ComputeMurmurHash2(built_gtb.data(), built_gtb.size()),
              parsed.new_gtb_hash);
}

TEST(LiveGameSystemTest, ParsesRealGameSptWhenAvailable) {
    const fs::path game_dir = resolve_live_game_dir();
    if (game_dir.empty()) {
        GTEST_SKIP() << "VN_GAME_DIR not set; skipping live game SPT check.";
    }
    const fs::path real_spt = game_dir / "spt" / "m_tokuten.spt";
    if (!fs::exists(real_spt)) {
        GTEST_SKIP() << "Installed game m_tokuten.spt not present on this machine.";
    }
    const exm::spt::SptScript script(exm::read_file(real_spt.u8string()));
    ASSERT_TRUE(script.is_valid());
    EXPECT_EQ(script.header_string_count(), kExpectedTokutenHeaderStrings);
    EXPECT_GT(script.strings().size(), kMinTokutenParsedStrings);
}

}  // namespace
