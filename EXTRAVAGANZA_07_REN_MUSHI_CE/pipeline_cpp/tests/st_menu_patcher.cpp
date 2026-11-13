// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// System tests verifying live Ren_mushi_CE game archives and bit-exact
// menu_patch.bin reconstruction when VN_GAME_DIR is set.
#include "common/util.h"
#include "menu/menu_patcher.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>

namespace {

namespace fs = std::filesystem;
using exm::menu::ComputeMurmurHash2;
using exm::menu::MenuArchivePatcher;
using exm::menu::decode_dwq_to_rgba;
using exm::menu::encode_dwq_from_rgba;
using exm::menu::parse_patch_bin;

constexpr std::size_t kDwqHeaderSize = 64;
constexpr std::size_t kExpectedSys0EntryCount = 78;
constexpr std::size_t kFirst24BitEntryIndex = 4;
constexpr int kMaxWalkLevels = 6;

fs::path ResolveLiveGameDir() {
    const wchar_t* env_dir = _wgetenv(L"VN_GAME_DIR");
    if (env_dir == nullptr || *env_dir == L'\0') {
        return {};
    }
    return fs::path(env_dir);
}

fs::path FindCommittedPatchBin() {
    fs::path cur = fs::current_path();
    for (int i = 0; i < kMaxWalkLevels; ++i) {
        if (fs::exists(cur / "menu_patch.bin")) {
            return cur / "menu_patch.bin";
        }
        if (fs::exists(cur / "EXTRAVAGANZA_07_REN_MUSHI_CE" / "menu_patch.bin")) {
            return cur / "EXTRAVAGANZA_07_REN_MUSHI_CE" / "menu_patch.bin";
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) {
            break;
        }
        cur = cur.parent_path();
    }
    return {};
}

}  // namespace

TEST(MenuPatcherTest, LiveSys0GpkAndFxfVerificationWhenGameDirPresent) {
    const fs::path game_dir = ResolveLiveGameDir();
    const fs::path gpk_path = game_dir / "dwq" / "sys0.gpk";
    const fs::path gtb_path = game_dir / "dwq" / "sys0.gtb";
    if (game_dir.empty() || !fs::exists(gpk_path) || !fs::exists(gtb_path)) {
        GTEST_SKIP() << "Live Ren_mushi_CE game directory not present on this machine.";
    }

    MenuArchivePatcher patcher;
    patcher.load(gpk_path, gtb_path);
    ASSERT_EQ(patcher.entry_count(), kExpectedSys0EntryCount);
    EXPECT_EQ(patcher.entry_offset(kExpectedSys0EntryCount),
              static_cast<std::uint32_t>(fs::file_size(gpk_path)));

    for (std::size_t idx = 0; idx < patcher.entry_count(); ++idx) {
        const auto& raw = patcher.entry_raw_bytes(idx);
        ASSERT_GT(raw.size(), kDwqHeaderSize) << "entry index=" << idx;
        auto img = patcher.decode_entry_rgba(idx);
        EXPECT_GT(img.width, 0u) << "entry index=" << idx;
        EXPECT_GT(img.height, 0u) << "entry index=" << idx;

        if (idx >= kFirst24BitEntryIndex) {
            auto reencoded = encode_dwq_from_rgba(raw.data(), raw.size(), img, true);
            auto redecoded = decode_dwq_to_rgba(reencoded.data(), reencoded.size());
            EXPECT_EQ(redecoded.width, img.width) << "entry index=" << idx;
            EXPECT_EQ(redecoded.height, img.height) << "entry index=" << idx;
            EXPECT_EQ(redecoded.pixels, img.pixels) << "entry index=" << idx;
        }
    }
}

TEST(MenuPatcherTest, CommittedMenuPatchBinReconstructsLiveArchiveBitForBit) {
    const fs::path game_dir = ResolveLiveGameDir();
    if (game_dir.empty() || !fs::exists(game_dir / "dwq" / "sys0.gpk") ||
        !fs::exists(game_dir / "dwq" / "sys0.gtb")) {
        GTEST_SKIP() << "Live Ren_mushi_CE game directory not present on this machine.";
    }
    const fs::path patch_bin_path = FindCommittedPatchBin();
    ASSERT_FALSE(patch_bin_path.empty())
        << "EXTRAVAGANZA_07_REN_MUSHI_CE/menu_patch.bin not found.";

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
    const auto live_gtb_bytes =
        exm::read_file((game_dir / "dwq" / "sys0.gtb").u8string());
    const auto live_gpk_bytes =
        exm::read_file((game_dir / "dwq" / "sys0.gpk").u8string());

    const auto [built_gtb, built_gpk] =
        MenuArchivePatcher::reconstruct_archive_from_patch_bin(
            patch_bin_bytes, orig_gtb_bytes, orig_gpk_bytes);

    const auto parsed = parse_patch_bin(patch_bin_bytes);
    EXPECT_EQ(built_gpk.size(), parsed.new_gpk_size);
    EXPECT_EQ(built_gtb.size(), parsed.new_gtb_size);
    EXPECT_EQ(ComputeMurmurHash2(built_gtb.data(), built_gtb.size()),
              parsed.new_gtb_hash);
    EXPECT_EQ(built_gtb, live_gtb_bytes);
    EXPECT_EQ(built_gpk, live_gpk_bytes);
}
