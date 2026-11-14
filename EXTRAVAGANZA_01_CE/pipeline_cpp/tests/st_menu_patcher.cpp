// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// End-to-end system test verifying that a clean retail EXTRAVAGANZA_CE
// installation (29 pristine retail files, zero .bak files, zero *_backup
// developer folders) is patched cleanly and idempotently by the real proxy_dll
// runtime consumer menu_autopatch::EnsureGameFilesPatchedInDir inside an
// isolated RAII temporary sandbox without mutating the live game folder.
#include "common/util.h"
#include "menus/menu_patcher.h"
#include "menu_autopatch.h"
#include "spt/spt_script.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

constexpr std::size_t kExpectedArchiveEntryCount = 183u;
constexpr std::size_t kExpectedTranslatedSpriteCount = 64u;
constexpr std::size_t kExpectedUntouchedSpriteCount = 119u;
constexpr std::uint32_t kExpectedPristine01SptSize = 1250316u;
constexpr std::uint32_t kExpectedPristineSys0GtbSize = 4429u;
constexpr std::uint32_t kExpectedPristineSys0GpkSize = 87436798u;
constexpr std::size_t kExpectedPristineCharanameSize = 100u;
constexpr std::size_t kExpectedPatchedCharanameSize = 142u;
constexpr std::size_t kExpectedTotalRetailFiles = 29u;
constexpr std::size_t kSentinelExpectedBytes = 20u;
constexpr std::size_t kSentinelMagicBytes = 8u;
constexpr char kExpectedSentinelMagic[kSentinelMagicBytes] = {
    'E', 'X', 'C', 'E', 'S', 'N', 'T', '1'};

constexpr std::string_view kDefaultDevGameDir =
    "d:\\Kits\\my_downloadedGames\\EXTRAVAGANZA\\EXTRAVAGANZA Complete Edition\\EXTRAVAGANZA_CE";

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1u, std::memory_order_relaxed);
        const std::string dir_name =
            std::string(prefix) + "_" + std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(id);
        path_ = fs::temp_directory_path() / fs::u8path(dir_name);
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

struct ResolvedGameEnv {
    fs::path path;
    bool from_explicit_env = false;
};

ResolvedGameEnv ResolveInstalledGameDir() {
    const wchar_t* wide_env = _wgetenv(L"VN_GAME_DIR");
    if (wide_env != nullptr && *wide_env != L'\0') {
        const std::wstring_view wide_view(wide_env);
        const int needed = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide_view.data(),
            static_cast<int>(wide_view.size()), nullptr, 0, nullptr, nullptr);
        if (needed > 0) {
            std::string utf8(static_cast<std::size_t>(needed), '\0');
            const int written = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wide_view.data(),
                static_cast<int>(wide_view.size()), utf8.data(), needed, nullptr, nullptr);
            if (written == needed) {
                return {fs::u8path(utf8), true};
            }
        }
    }

    std::error_code ec;
    const fs::path fallback = fs::u8path(kDefaultDevGameDir);
    if (fs::is_directory(fallback, ec)) {
        return {fallback, false};
    }
    return {{}, false};
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

// Resolves a pristine retail source file strictly from <rel_path>.bak (if the
// source install was previously patched) or <rel_path> (if unpatched), NEVER
// consulting legacy developer *_backup folders.
fs::path ResolvePristineRetailSource(const fs::path& game_root, std::string_view rel_path) {
    std::error_code ec;
    const fs::path bak = game_root / fs::u8path(std::string(rel_path) + ".bak");
    if (fs::is_regular_file(bak, ec)) {
        return bak;
    }
    return game_root / fs::u8path(rel_path);
}

void CopyPristineFileToCleanSandbox(const fs::path& src_file, const fs::path& dst_file) {
    std::error_code ec;
    fs::create_directories(dst_file.parent_path(), ec);
    ASSERT_FALSE(ec) << "Failed creating parent directory for " << dst_file.u8string();
    fs::copy_file(src_file, dst_file, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "Failed copying " << src_file.u8string() << " -> " << dst_file.u8string();
}

std::size_t CountFilesWithSuffix(const fs::path& root, std::string_view suffix) {
    std::size_t count = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().u8string();
        if (name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(),
                         suffix.data(), suffix.size()) == 0) {
            ++count;
        }
    }
    return count;
}

bool HasAnyLegacyBackupDirectory(const fs::path& root) {
    constexpr std::string_view kLegacyDirs[] = {
        "spt_backup", "dwq_backup", "xtx_backup", "chart_backup", "init2_backup"};
    std::error_code ec;
    for (std::string_view dir_name : kLegacyDirs) {
        if (fs::exists(root / dir_name, ec)) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(MenuPatcherSystem, CleanRetailInstallPatchedEndToEndViaRealAutopatchConsumer) {
    const fs::path patch_bin_path = ResolvePatchBinPath();
    std::error_code ec;
    ASSERT_FALSE(patch_bin_path.empty())
        << "Committed extravaganza_patch.bin could not be located in repository.";
    ASSERT_TRUE(fs::is_regular_file(patch_bin_path, ec))
        << "Missing extravaganza_patch.bin at " << patch_bin_path.u8string();

    const ResolvedGameEnv game_env = ResolveInstalledGameDir();
    if (game_env.path.empty()) {
        GTEST_SKIP() << "VN_GAME_DIR is unset and licensed EXTRAVAGANZA_CE retail assets are "
                        "not installed on this host.";
    }
    ASSERT_TRUE(fs::is_directory(game_env.path, ec))
        << "Configured VN_GAME_DIR does not exist: " << game_env.path.u8string();

    const Bytes patch_bin = read_file(patch_bin_path.u8string());
    const mn::ParsedPatchBin parsed = mn::parse_patch_bin(patch_bin);
    ASSERT_EQ(parsed.standalone_records.size(), mn::kExpectedStandaloneFileCount);
    ASSERT_EQ(parsed.gpk_records.size(), kExpectedTranslatedSpriteCount);
    EXPECT_EQ(parsed.orig_gtb_size, kExpectedPristineSys0GtbSize);
    EXPECT_EQ(parsed.orig_gpk_size, kExpectedPristineSys0GpkSize);

    // Stage a 100% clean retail install sandbox (ZERO .bak files, ZERO *_backup
    // developer folders) from the 29 pristine retail files + extravaganza_patch.bin.
    ScopedTempDir sandbox("exc_st_clean_retail");
    for (const mn::StandalonePatchRecord& rec : parsed.standalone_records) {
        const fs::path src = ResolvePristineRetailSource(game_env.path, rec.rel_path);
        ASSERT_TRUE(fs::is_regular_file(src, ec))
            << "Missing pristine retail file for " << rec.rel_path << " at " << src.u8string();
        CopyPristineFileToCleanSandbox(src, sandbox.path() / fs::u8path(rec.rel_path));
    }

    const fs::path pristine_gtb_src = ResolvePristineRetailSource(game_env.path, "dwq\\sys0.gtb");
    const fs::path pristine_gpk_src = ResolvePristineRetailSource(game_env.path, "dwq\\sys0.gpk");
    ASSERT_TRUE(fs::is_regular_file(pristine_gtb_src, ec));
    ASSERT_TRUE(fs::is_regular_file(pristine_gpk_src, ec));
    CopyPristineFileToCleanSandbox(pristine_gtb_src, sandbox.path() / "dwq" / "sys0.gtb");
    CopyPristineFileToCleanSandbox(pristine_gpk_src, sandbox.path() / "dwq" / "sys0.gpk");
    CopyPristineFileToCleanSandbox(patch_bin_path, sandbox.path() / "extravaganza_patch.bin");

    ASSERT_EQ(fs::file_size(sandbox.path() / "spt" / "01.spt", ec), kExpectedPristine01SptSize);
    ASSERT_EQ(fs::file_size(sandbox.path() / "dwq" / "sys0.gtb", ec), kExpectedPristineSys0GtbSize);
    ASSERT_EQ(fs::file_size(sandbox.path() / "dwq" / "sys0.gpk", ec), kExpectedPristineSys0GpkSize);
    ASSERT_EQ(CountFilesWithSuffix(sandbox.path(), ".bak"), 0u);
    ASSERT_FALSE(HasAnyLegacyBackupDirectory(sandbox.path()));
    ASSERT_FALSE(fs::exists(sandbox.path() / "extravaganza_patch.bin.applied", ec));

    // Run 1: Invoke the real proxy_dll menu_autopatch consumer on the clean retail sandbox.
    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));

    const fs::path sentinel_path = sandbox.path() / "extravaganza_patch.bin.applied";
    ASSERT_TRUE(fs::is_regular_file(sentinel_path, ec));
    const Bytes sentinel_bytes = read_file(sentinel_path.u8string());
    ASSERT_EQ(sentinel_bytes.size(), kSentinelExpectedBytes);
    EXPECT_EQ(std::memcmp(sentinel_bytes.data(), kExpectedSentinelMagic, kSentinelMagicBytes), 0);
    EXPECT_EQ(CountFilesWithSuffix(sandbox.path(), ".tmp"), 0u);
    EXPECT_EQ(CountFilesWithSuffix(sandbox.path(), ".bak"), kExpectedTotalRetailFiles);
    EXPECT_FALSE(HasAnyLegacyBackupDirectory(sandbox.path()));

    for (const mn::StandalonePatchRecord& rec : parsed.standalone_records) {
        const fs::path active_path = sandbox.path() / fs::u8path(rec.rel_path);
        const fs::path bak_path = sandbox.path() / fs::u8path(rec.rel_path + ".bak");

        ASSERT_TRUE(fs::is_regular_file(active_path, ec)) << rec.rel_path;
        ASSERT_TRUE(fs::is_regular_file(bak_path, ec)) << rec.rel_path;

        const Bytes bak_bytes = read_file(bak_path.u8string());
        EXPECT_EQ(bak_bytes.size(), rec.orig_file_size) << rec.rel_path;
        EXPECT_EQ(menu_autopatch::ComputeMurmurHash2(bak_bytes.data(), bak_bytes.size()),
                  rec.orig_data_hash)
            << rec.rel_path;

        const Bytes active_bytes = read_file(active_path.u8string());
        EXPECT_EQ(active_bytes.size(), rec.new_file_size) << rec.rel_path;
        EXPECT_EQ(menu_autopatch::ComputeMurmurHash2(active_bytes.data(), active_bytes.size()),
                  rec.new_data_hash)
            << rec.rel_path;
    }

    EXPECT_EQ(fs::file_size(sandbox.path() / "spt" / "01.spt.bak", ec), kExpectedPristine01SptSize);
    const Bytes patched_01_spt = read_file((sandbox.path() / "spt" / "01.spt").u8string());
    EXPECT_TRUE(exc::spt::is_valid(exc::spt::decrypt(patched_01_spt)));

    const Bytes bak_main = read_file((sandbox.path() / "init2" / "mainControl.fxf.bak").u8string());
    const Bytes active_main = read_file((sandbox.path() / "init2" / "mainControl.fxf").u8string());
    EXPECT_EQ(active_main, mn::patch_main_control_fxf(bak_main));

    const Bytes bak_backlog = read_file((sandbox.path() / "init2" / "backlog.fxf.bak").u8string());
    const Bytes active_backlog = read_file((sandbox.path() / "init2" / "backlog.fxf").u8string());
    EXPECT_EQ(active_backlog, mn::patch_backlog_fxf(bak_backlog));

    const Bytes bak_xtx = read_file((sandbox.path() / "spt" / "charaname.xtx.bak").u8string());
    const Bytes active_xtx = read_file((sandbox.path() / "spt" / "charaname.xtx").u8string());
    EXPECT_EQ(bak_xtx.size(), kExpectedPristineCharanameSize);
    EXPECT_EQ(active_xtx.size(), kExpectedPatchedCharanameSize);
    EXPECT_NE(active_xtx, bak_xtx);

    const Bytes bak_gtb = read_file((sandbox.path() / "dwq" / "sys0.gtb.bak").u8string());
    const Bytes bak_gpk = read_file((sandbox.path() / "dwq" / "sys0.gpk.bak").u8string());
    const Bytes active_gtb = read_file((sandbox.path() / "dwq" / "sys0.gtb").u8string());
    const Bytes active_gpk = read_file((sandbox.path() / "dwq" / "sys0.gpk").u8string());

    EXPECT_EQ(bak_gtb.size(), parsed.orig_gtb_size);
    EXPECT_EQ(menu_autopatch::ComputeMurmurHash2(bak_gtb.data(), bak_gtb.size()),
              parsed.orig_gtb_hash);
    EXPECT_EQ(bak_gpk.size(), parsed.orig_gpk_size);
    EXPECT_EQ(active_gtb.size(), parsed.new_gtb_size);
    EXPECT_EQ(menu_autopatch::ComputeMurmurHash2(active_gtb.data(), active_gtb.size()),
              parsed.new_gtb_hash);
    EXPECT_EQ(active_gpk.size(), parsed.new_gpk_size);

    const mn::MenuArchivePatcher bak_arch =
        mn::MenuArchivePatcher::load_from_memory(bak_gtb, bak_gpk);
    const mn::MenuArchivePatcher active_arch =
        mn::MenuArchivePatcher::load_from_memory(active_gtb, active_gpk);
    ASSERT_EQ(bak_arch.entry_count(), kExpectedArchiveEntryCount);
    ASSERT_EQ(active_arch.entry_count(), kExpectedArchiveEntryCount);

    const auto [roundtrip_gtb, roundtrip_gpk] = bak_arch.serialize();
    EXPECT_EQ(roundtrip_gtb, bak_gtb);
    EXPECT_EQ(roundtrip_gpk, bak_gpk);

    std::vector<bool> is_patched_sprite(kExpectedArchiveEntryCount, false);
    for (const mn::GpkEntryPatchRecord& rec : parsed.gpk_records) {
        ASSERT_LT(rec.entry_index, kExpectedArchiveEntryCount);
        is_patched_sprite[rec.entry_index] = true;
        const Bytes dwq_bytes = active_arch.raw_entry_bytes(rec.entry_index);
        EXPECT_EQ(dwq_bytes.size(), rec.new_size);
        EXPECT_EQ(menu_autopatch::ComputeMurmurHash2(dwq_bytes.data(), dwq_bytes.size()),
                  rec.new_data_hash);
        const mn::RgbaImage decoded_img =
            mn::decode_dwq_to_rgba(dwq_bytes.data(), dwq_bytes.size());
        EXPECT_GT(decoded_img.width, 0);
        EXPECT_GT(decoded_img.height, 0);
    }

    std::size_t untouched_count = 0;
    for (std::size_t i = 0; i < kExpectedArchiveEntryCount; ++i) {
        if (!is_patched_sprite[i]) {
            EXPECT_EQ(active_arch.raw_entry_bytes(i), bak_arch.raw_entry_bytes(i))
                << "Untouched sprite entry " << i << " was altered";
            ++untouched_count;
        }
    }
    EXPECT_EQ(untouched_count, kExpectedUntouchedSpriteCount);

    // Run 2: Verify sentinel fast-path idempotence leaves .bak files untouched.
    const auto spt01_bak_mtime = fs::last_write_time(sandbox.path() / "spt" / "01.spt.bak", ec);
    const auto gpk_bak_mtime = fs::last_write_time(sandbox.path() / "dwq" / "sys0.gpk.bak", ec);
    const auto sentinel_mtime = fs::last_write_time(sentinel_path, ec);

    ASSERT_TRUE(menu_autopatch::EnsureGameFilesPatchedInDir(sandbox.path().wstring()));

    EXPECT_EQ(fs::last_write_time(sandbox.path() / "spt" / "01.spt.bak", ec), spt01_bak_mtime);
    EXPECT_EQ(fs::last_write_time(sandbox.path() / "dwq" / "sys0.gpk.bak", ec), gpk_bak_mtime);
    EXPECT_EQ(fs::last_write_time(sentinel_path, ec), sentinel_mtime);
    EXPECT_EQ(fs::file_size(sandbox.path() / "spt" / "01.spt.bak", ec), kExpectedPristine01SptSize);
    EXPECT_EQ(read_file((sandbox.path() / "spt" / "01.spt").u8string()), patched_01_spt);
}
