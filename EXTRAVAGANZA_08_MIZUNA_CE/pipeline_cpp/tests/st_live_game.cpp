// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// System tests (SYSTEM) against a live installed EXTRA_VA_MIZUNA_CE directory
// pointed to by VN_GAME_DIR: DWQ/GPK/GTB decoding, MIZMENU1 archive
// reconstruction, and live tantai3.spt decryption/parsing.
#include "common/util.h"
#include "menu/menu_patcher.h"
#include "spt/spt_script.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr int kMaxWalkLevels = 8;
constexpr std::size_t kBufferGrowthFactor = 2;
constexpr std::size_t kExpectedSys0EntryCount = 54;
constexpr std::size_t kSamplePack5Index = 1;
constexpr std::size_t kSamplePack3aIndex = 25;
constexpr std::size_t kSamplePack2aIndex = 53;
constexpr std::uint32_t kExpectedTantai3HeaderCount = 729;
constexpr std::size_t kMinExpectedTantai3Strings = 1800;

fs::path ResolveLiveGameDir() {
    const wchar_t* env_dir = _wgetenv(L"VN_GAME_DIR");
    if (env_dir == nullptr || *env_dir == L'\0') {
        return {};
    }
    const std::wstring_view wide(env_dir);
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        utf8.data(), needed, nullptr, nullptr);
    if (written <= 0) {
        return {};
    }
    return fs::u8path(utf8);
}

fs::path CurrentTestExecutableDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n =
            GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * kBufferGrowthFactor);
    }
    return fs::path(buf).parent_path();
}

fs::path FindCommittedPatchBinFrom(fs::path cur) {
    std::error_code ec;
    for (int i = 0; i < kMaxWalkLevels && !cur.empty(); ++i) {
        if (fs::is_regular_file(cur / "menu_patch.bin", ec)) {
            return cur / "menu_patch.bin";
        }
        const fs::path nested = cur / "EXTRAVAGANZA_08_MIZUNA_CE" / "menu_patch.bin";
        if (fs::is_regular_file(nested, ec)) {
            return nested;
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) {
            break;
        }
        cur = cur.parent_path();
    }
    return {};
}

fs::path FindCommittedPatchBin() {
    const fs::path from_exe = FindCommittedPatchBinFrom(CurrentTestExecutableDir());
    if (!from_exe.empty()) {
        return from_exe;
    }
    return FindCommittedPatchBinFrom(fs::current_path());
}

fs::path ResolveOriginalDwqArchive(const fs::path& game_dir, std::string_view filename) {
    const fs::path backup_dir_candidate = game_dir / "dwq_backup" / filename;
    if (fs::exists(backup_dir_candidate)) {
        return backup_dir_candidate;
    }
    const std::string bak_name = std::string(filename) + ".bak";
    const fs::path bak_candidate = game_dir / "dwq" / bak_name;
    if (fs::exists(bak_candidate)) {
        return bak_candidate;
    }
    return game_dir / "dwq" / filename;
}

TEST(LiveGameSystem, LiveSys0GpkAndFxfVerificationWhenGameDirPresent) {
    const fs::path game_dir = ResolveLiveGameDir();
    const fs::path gpk_path = game_dir / "dwq" / "sys0.gpk";
    const fs::path gtb_path = game_dir / "dwq" / "sys0.gtb";
    if (game_dir.empty() || !fs::exists(gpk_path) || !fs::exists(gtb_path)) {
        GTEST_SKIP() << "Live EXTRA_VA_MIZUNA_CE game directory not present on this machine.";
    }

    exm::menu::MenuArchivePatcher patcher;
    patcher.load(gpk_path, gtb_path);
    ASSERT_EQ(patcher.entry_count(), kExpectedSys0EntryCount);
    EXPECT_EQ(patcher.entry_offset(kExpectedSys0EntryCount),
              static_cast<std::uint32_t>(fs::file_size(gpk_path)));

    for (std::size_t idx = 0; idx < kExpectedSys0EntryCount; ++idx) {
        const auto img = patcher.decode_entry_rgba(idx);
        EXPECT_GT(img.width, 0u) << "entry index=" << idx;
        EXPECT_GT(img.height, 0u) << "entry index=" << idx;
    }

    const auto& raw_5 = patcher.entry_raw_bytes(kSamplePack5Index);
    const auto img_5 = patcher.decode_entry_rgba(kSamplePack5Index);
    const auto reenc_5 =
        exm::menu::encode_dwq_from_rgba(raw_5.data(), raw_5.size(), img_5, true);
    const auto redec_5 = exm::menu::decode_dwq_to_rgba(reenc_5.data(), reenc_5.size());
    EXPECT_EQ(redec_5.width, img_5.width);
    EXPECT_EQ(redec_5.height, img_5.height);

    const auto& raw_3a = patcher.entry_raw_bytes(kSamplePack3aIndex);
    const auto img_3a = patcher.decode_entry_rgba(kSamplePack3aIndex);
    const auto reenc_3a =
        exm::menu::encode_dwq_from_rgba(raw_3a.data(), raw_3a.size(), img_3a, true);
    const auto redec_3a = exm::menu::decode_dwq_to_rgba(reenc_3a.data(), reenc_3a.size());
    EXPECT_EQ(redec_3a.pixels, img_3a.pixels);

    const auto& raw_2a = patcher.entry_raw_bytes(kSamplePack2aIndex);
    const auto img_2a = patcher.decode_entry_rgba(kSamplePack2aIndex);
    const auto reenc_2a =
        exm::menu::encode_dwq_from_rgba(raw_2a.data(), raw_2a.size(), img_2a, true);
    const auto redec_2a = exm::menu::decode_dwq_to_rgba(reenc_2a.data(), reenc_2a.size());
    EXPECT_EQ(redec_2a.pixels, img_2a.pixels);
}

TEST(LiveGameSystem, CommittedMenuPatchBinReconstructsLiveArchiveBitForBit) {
    const fs::path game_dir = ResolveLiveGameDir();
    if (game_dir.empty() || !fs::exists(game_dir / "dwq" / "sys0.gpk") ||
        !fs::exists(game_dir / "dwq" / "sys0.gtb")) {
        GTEST_SKIP() << "Live EXTRA_VA_MIZUNA_CE game directory not present on this machine.";
    }
    const fs::path patch_bin_path = FindCommittedPatchBin();
    if (patch_bin_path.empty()) {
        GTEST_SKIP() << "EXTRAVAGANZA_08_MIZUNA_CE/menu_patch.bin not generated yet.";
    }

    const fs::path orig_gpk_path = ResolveOriginalDwqArchive(game_dir, "sys0.gpk");
    const fs::path orig_gtb_path = ResolveOriginalDwqArchive(game_dir, "sys0.gtb");

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

TEST(LiveGameSystem, ParsesRealGameSptWhenAvailable) {
    const fs::path game_dir = ResolveLiveGameDir();
    if (game_dir.empty()) {
        GTEST_SKIP() << "VN_GAME_DIR not set; skipping live game SPT check";
    }
    const fs::path real_spt = game_dir / "spt" / "tantai3.spt";
    if (!fs::exists(real_spt)) {
        GTEST_SKIP() << "Installed game tantai3.spt not present on this machine";
    }
    const exm::spt::SptScript script(exm::read_file(real_spt.u8string()));
    ASSERT_TRUE(script.is_valid());
    EXPECT_EQ(script.header_string_count(), kExpectedTantai3HeaderCount);
    EXPECT_GT(script.strings().size(), kMinExpectedTantai3Strings);
}

}  // namespace
