// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// System tests for pipeline_cpp/src/menu/menu_patcher.{h,cpp} requiring a live
// AGEHA_CE installation via VN_GAME_DIR.
#include "menu/menu_patcher.h"

#include "common/util.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using exm::menu::ComputeMurmurHash2;
using exm::menu::MenuArchivePatcher;
using exm::menu::decrypt_fxf;
using exm::menu::parse_patch_bin;

constexpr std::size_t kDwqHeaderSize = 64;
constexpr std::size_t kDwqDataSizeOffset = 0x20;
constexpr std::uint32_t kByteShift1 = 8;
constexpr std::uint32_t kByteShift2 = 16;
constexpr std::uint32_t kByteShift3 = 24;
constexpr std::size_t kByteIndex0 = 0;
constexpr std::size_t kByteIndex1 = 1;
constexpr std::size_t kByteIndex2 = 2;
constexpr std::size_t kByteIndex3 = 3;

constexpr std::size_t kExpectedSys0EntryCount = 83;
constexpr std::size_t kExpectedTranslatedEntryCount = 21;
constexpr int kMaxWalkLevels = 6;

constexpr std::size_t kTranslatedSpriteIdx02 = 2;
constexpr std::size_t kTranslatedSpriteIdx03 = 3;
constexpr std::size_t kTranslatedSpriteIdx05 = 5;
constexpr std::size_t kTranslatedSpriteIdx39 = 39;
constexpr std::size_t kTranslatedSpriteIdx40 = 40;
constexpr std::size_t kTranslatedSpriteIdx41 = 41;
constexpr std::size_t kTranslatedSpriteIdx42 = 42;
constexpr std::size_t kTranslatedSpriteIdx47 = 47;
constexpr std::size_t kTranslatedSpriteIdx48 = 48;
constexpr std::size_t kTranslatedSpriteIdx49 = 49;
constexpr std::size_t kTranslatedSpriteIdx50 = 50;
constexpr std::size_t kTranslatedSpriteIdx56 = 56;
constexpr std::size_t kTranslatedSpriteIdx65 = 65;
constexpr std::size_t kTranslatedSpriteIdx66 = 66;
constexpr std::size_t kTranslatedSpriteIdx70 = 70;
constexpr std::size_t kTranslatedSpriteIdx71 = 71;
constexpr std::size_t kTranslatedSpriteIdx78 = 78;
constexpr std::size_t kTranslatedSpriteIdx79 = 79;
constexpr std::size_t kTranslatedSpriteIdx80 = 80;
constexpr std::size_t kTranslatedSpriteIdx81 = 81;
constexpr std::size_t kTranslatedSpriteIdx82 = 82;

constexpr std::array<std::size_t, kExpectedTranslatedEntryCount> kTranslatedIndices = {
    kTranslatedSpriteIdx02, kTranslatedSpriteIdx03, kTranslatedSpriteIdx05,
    kTranslatedSpriteIdx39, kTranslatedSpriteIdx40, kTranslatedSpriteIdx41,
    kTranslatedSpriteIdx42, kTranslatedSpriteIdx47, kTranslatedSpriteIdx48,
    kTranslatedSpriteIdx49, kTranslatedSpriteIdx50, kTranslatedSpriteIdx56,
    kTranslatedSpriteIdx65, kTranslatedSpriteIdx66, kTranslatedSpriteIdx70,
    kTranslatedSpriteIdx71, kTranslatedSpriteIdx78, kTranslatedSpriteIdx79,
    kTranslatedSpriteIdx80, kTranslatedSpriteIdx81, kTranslatedSpriteIdx82,
};

std::uint32_t read_u32_le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[kByteIndex0]) |
           (static_cast<std::uint32_t>(p[kByteIndex1]) << kByteShift1) |
           (static_cast<std::uint32_t>(p[kByteIndex2]) << kByteShift2) |
           (static_cast<std::uint32_t>(p[kByteIndex3]) << kByteShift3);
}

fs::path ResolveLiveGameDir() {
    const wchar_t* env_dir = _wgetenv(L"VN_GAME_DIR");
    if (env_dir == nullptr || *env_dir == L'\0') {
        return {};
    }
    const std::wstring_view wide(env_dir);
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
    if (written != needed) {
        return {};
    }
    return fs::u8path(utf8);
}

fs::path FindCommittedPatchBin() {
    fs::path cur = fs::current_path();
    for (int i = 0; i < kMaxWalkLevels; ++i) {
        if (fs::exists(cur / "menu_patch.bin")) {
            return cur / "menu_patch.bin";
        }
        if (fs::exists(cur / "EXTRAVAGANZA_04_AGEHA_CE" / "menu_patch.bin")) {
            return cur / "EXTRAVAGANZA_04_AGEHA_CE" / "menu_patch.bin";
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) {
            break;
        }
        cur = cur.parent_path();
    }
    return {};
}

}  // namespace

TEST(MenuPatcherSystem, LiveSys0GpkAndFxfVerificationWhenGameDirPresent) {
    const fs::path game_dir = ResolveLiveGameDir();
    const fs::path gpk_path = game_dir / "dwq" / "sys0.gpk";
    const fs::path gtb_path = game_dir / "dwq" / "sys0.gtb";
    if (game_dir.empty() || !fs::exists(gpk_path) || !fs::exists(gtb_path)) {
        GTEST_SKIP() << "Live AGEHA_CE game directory not present on this machine.";
    }

    MenuArchivePatcher patcher;
    patcher.load(gpk_path, gtb_path);
    ASSERT_EQ(patcher.entry_count(), kExpectedSys0EntryCount);
    EXPECT_EQ(patcher.entry_offset(kExpectedSys0EntryCount),
              static_cast<std::uint32_t>(fs::file_size(gpk_path)));

    for (std::size_t idx : kTranslatedIndices) {
        const auto& raw = patcher.entry_raw_bytes(idx);
        ASSERT_GT(raw.size(), kDwqHeaderSize);
        const std::uint32_t dsize = read_u32_le(raw.data() + kDwqDataSizeOffset);
        EXPECT_EQ(kDwqHeaderSize + dsize, raw.size()) << "entry index=" << idx;
        const auto img = patcher.decode_entry_rgba(idx);
        EXPECT_GT(img.width, 0u);
        EXPECT_GT(img.height, 0u);
    }

    const fs::path main_fxf_path = game_dir / "init2" / "mainControl.fxf";
    if (fs::exists(main_fxf_path)) {
        const auto dec_main = decrypt_fxf(exm::read_file(main_fxf_path.u8string()));
        EXPECT_NE(dec_main.find("windowTitle AGEHA ~EXTRAVAGANZA~ CE"),
                  std::string::npos);
        EXPECT_NE(dec_main.find("windowTitleTaiken AGEHA ~EXTRAVAGANZA~ CE Trial"),
                  std::string::npos);
        EXPECT_NE(dec_main.find("bppErrorMessage Please set display to 16-bit color or higher."),
                  std::string::npos);
    }

    const fs::path backlog_fxf_path = game_dir / "init2" / "backlog.fxf";
    if (fs::exists(backlog_fxf_path)) {
        const auto dec_backlog = decrypt_fxf(exm::read_file(backlog_fxf_path.u8string()));
        const std::string expected_cp932_backlog = exm::utf8_to_cp932("Ｂａｃｋｌｏｇ");
        EXPECT_NE(dec_backlog.find(expected_cp932_backlog), std::string::npos);
    }
}

TEST(MenuPatcherSystem, CommittedMenuPatchBinReconstructsLiveArchiveBitForBit) {
    const fs::path game_dir = ResolveLiveGameDir();
    if (game_dir.empty() || !fs::exists(game_dir / "dwq" / "sys0.gpk") ||
        !fs::exists(game_dir / "dwq" / "sys0.gtb")) {
        GTEST_SKIP() << "Live AGEHA_CE game directory not present on this machine.";
    }
    const fs::path patch_bin_path = FindCommittedPatchBin();
    ASSERT_FALSE(patch_bin_path.empty())
        << "Committed EXTRAVAGANZA_04_AGEHA_CE/menu_patch.bin not found";

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
