// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System tests verifying live game archive and standalone file reconstruction
// against VN_GAME_DIR when available.
#include "common/util.h"
#include "menus/menu_patcher.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace mn = exc::menus;
namespace fs = std::filesystem;
using namespace exc;

namespace {

constexpr std::size_t kExpectedEntryCount = 183;
constexpr std::size_t kExpectedTranslatedCount = 64;
constexpr std::size_t kExpectedUntouchedCount = 119;
constexpr std::size_t kExpectedBackupCharanameSize = 100;
constexpr std::size_t kExpectedPatchedCharanameSize = 142;

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

fs::path ResolvePatchBinPath() {
    std::error_code ec;
    const fs::path roots[] = {fs::current_path(ec), fs::u8path(__FILE__).parent_path()};
    for (const fs::path& base : roots) {
        for (fs::path cur = base; !cur.empty(); cur = cur.parent_path()) {
            const fs::path direct = cur / "extravaganza_patch.bin";
            if (fs::is_regular_file(direct, ec)) {
                return direct;
            }
            const fs::path nested = cur / "EXTRAVAGANZA_01_CE" / "extravaganza_patch.bin";
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

}  // namespace

TEST(MenuPatcherSystem, LiveBackupZeroOverrideRoundTripIsByteIdentical) {
    const fs::path game_root = ResolveLiveGameDir();
    const fs::path gtb_bak = game_root / "dwq_backup" / "sys0.gtb";
    const fs::path gpk_bak = game_root / "dwq_backup" / "sys0.gpk";
    std::error_code ec;
    if (game_root.empty() || !fs::is_regular_file(gtb_bak, ec) ||
        !fs::is_regular_file(gpk_bak, ec)) {
        GTEST_SKIP() << "VN_GAME_DIR backup directory not present on this machine.";
    }

    const Bytes orig_gtb = read_file(gtb_bak.u8string());
    const Bytes orig_gpk = read_file(gpk_bak.u8string());

    const mn::MenuArchivePatcher arch =
        mn::MenuArchivePatcher::load_from_memory(orig_gtb, orig_gpk);
    EXPECT_EQ(arch.entry_count(), kExpectedEntryCount);

    const auto [rebuilt_gtb, rebuilt_gpk] = arch.serialize();
    EXPECT_EQ(rebuilt_gtb, orig_gtb);
    EXPECT_EQ(rebuilt_gpk, orig_gpk);
}

TEST(MenuPatcherSystem, LiveArchiveRoundTripAndUntouchedEntriesBitExact) {
    const fs::path game_root = ResolveLiveGameDir();
    std::error_code ec;
    if (game_root.empty() || !fs::is_directory(game_root / "dwq_backup", ec)) {
        GTEST_SKIP() << "VN_GAME_DIR live game directory not present on this machine.";
    }

    const fs::path patch_bin_path = ResolvePatchBinPath();
    ASSERT_FALSE(patch_bin_path.empty());
    ASSERT_TRUE(fs::is_regular_file(patch_bin_path, ec))
        << "Missing extravaganza_patch.bin at " << patch_bin_path.u8string();

    const Bytes patch_bin = read_file(patch_bin_path.u8string());
    const mn::ParsedPatchBin parsed = mn::parse_patch_bin(patch_bin);

    const Bytes orig_gtb = read_file((game_root / "dwq_backup" / "sys0.gtb").u8string());
    const Bytes orig_gpk = read_file((game_root / "dwq_backup" / "sys0.gpk").u8string());
    const Bytes live_gtb = read_file((game_root / "dwq" / "sys0.gtb").u8string());
    const Bytes live_gpk = read_file((game_root / "dwq" / "sys0.gpk").u8string());

    const auto [rebuilt_gtb, rebuilt_gpk] =
        mn::MenuArchivePatcher::reconstruct_archive_from_patch_bin(
            patch_bin, orig_gtb, orig_gpk);
    EXPECT_EQ(rebuilt_gtb, live_gtb);
    EXPECT_EQ(rebuilt_gpk, live_gpk);

    const mn::MenuArchivePatcher bak_arch =
        mn::MenuArchivePatcher::load_from_memory(orig_gtb, orig_gpk);
    const mn::MenuArchivePatcher rebuilt_arch =
        mn::MenuArchivePatcher::load_from_memory(rebuilt_gtb, rebuilt_gpk);
    ASSERT_EQ(bak_arch.entry_count(), kExpectedEntryCount);
    ASSERT_EQ(rebuilt_arch.entry_count(), kExpectedEntryCount);

    std::vector<bool> is_patched(kExpectedEntryCount, false);
    for (const mn::GpkEntryPatchRecord& rec : parsed.gpk_records) {
        ASSERT_LT(rec.entry_index, kExpectedEntryCount);
        is_patched[rec.entry_index] = true;
        const Bytes dwq_bytes = rebuilt_arch.raw_entry_bytes(rec.entry_index);
        const mn::RgbaImage decoded_img =
            mn::decode_dwq_to_rgba(dwq_bytes.data(), dwq_bytes.size());
        EXPECT_GT(decoded_img.width, 0);
        EXPECT_GT(decoded_img.height, 0);
    }

    std::size_t untouched_count = 0;
    for (std::size_t i = 0; i < kExpectedEntryCount; ++i) {
        if (!is_patched[i]) {
            EXPECT_EQ(rebuilt_arch.raw_entry_bytes(i), bak_arch.raw_entry_bytes(i))
                << "Untouched entry " << i << " modified!";
            ++untouched_count;
        }
    }
    EXPECT_EQ(untouched_count, kExpectedUntouchedCount);

    const mn::MenuBuildSummary summary =
        mn::MenuArchivePatcher::apply_patch_bin_to_game_dir(
            game_root.u8string(), patch_bin_path.u8string(), true);
    EXPECT_EQ(summary.total_archive_entries, kExpectedEntryCount);
    EXPECT_EQ(summary.patched_sprite_entries, kExpectedTranslatedCount);
    EXPECT_EQ(summary.untouched_sprite_entries, kExpectedUntouchedCount);
    EXPECT_EQ(summary.patched_standalone_files, mn::kExpectedStandaloneFileCount);

    for (const mn::StandalonePatchRecord& rec : parsed.standalone_records) {
        const Bytes live_bytes = read_file((game_root / fs::u8path(rec.rel_path)).u8string());
        EXPECT_EQ(live_bytes.size(), rec.new_file_size) << rec.rel_path;
        EXPECT_EQ(mn::ComputeMurmurHash2(live_bytes.data(), live_bytes.size()),
                  rec.new_data_hash)
            << rec.rel_path;
    }

    const Bytes exp_main = mn::patch_main_control_fxf(
        read_file((game_root / "init2_backup" / "mainControl.fxf").u8string()));
    const Bytes live_main = read_file((game_root / "init2" / "mainControl.fxf").u8string());
    EXPECT_EQ(live_main, exp_main);

    const Bytes exp_backlog = mn::patch_backlog_fxf(
        read_file((game_root / "init2_backup" / "backlog.fxf").u8string()));
    const Bytes live_backlog = read_file((game_root / "init2" / "backlog.fxf").u8string());
    EXPECT_EQ(live_backlog, exp_backlog);

    const Bytes live_xtx = read_file((game_root / "spt" / "charaname.xtx").u8string());
    const Bytes bak_xtx = read_file((game_root / "xtx_backup" / "charaname.xtx").u8string());
    EXPECT_EQ(bak_xtx.size(), kExpectedBackupCharanameSize);
    EXPECT_EQ(live_xtx.size(), kExpectedPatchedCharanameSize);
    EXPECT_NE(live_xtx, bak_xtx);
}
