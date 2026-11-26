// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for src/xtx/xtx.{h,cpp} and src/xtx/xtx_tables.cpp: Matsuro CE
// charaname.xtx CP932 replacement, xtx_backup/ preference on shrinking/updated
// re-runs, and .bak preference when xtx_backup/ does not exist yet.
#include "xtx/xtx.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

namespace fs = std::filesystem;
namespace xtx = exm::xtx;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1u, std::memory_order_relaxed);
        const std::string dir_name = std::string(prefix) + "_" +
                                     std::to_string(GetCurrentProcessId()) + "_" +
                                     std::to_string(id);
        path_ = fs::temp_directory_path() / fs::u8path(dir_name);
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

TEST(Xtx, TranslatePrefersXtxBackupOnReRunWithUpdatedAndShrunkTable) {
    const ScopedTempDir tmp("matsuro_ut_xtx_backup");
    fs::create_directories(tmp.path() / "spt");

    const std::string filepath = (tmp.path() / "spt" / "charaname.xtx").u8string();
    const std::string backup_dir = (tmp.path() / "xtx_backup").u8string();
    const std::string backup_file = (tmp.path() / "xtx_backup" / "charaname.xtx").u8string();

    const std::string initial_utf8 =
        "0,名前なし,nobody\r\n"
        "1,美弥香,miyaka\r\n"
        "2,綾佳,ayaka\r\n";
    exm::write_file(filepath, exm::utf8_to_cp932_replace(initial_utf8));

    // Pass 1: translate both characters and capture pristine backup.
    const int pass1_changes =
        xtx::translate_xtx_file(filepath, xtx::charaname_translations(), backup_dir);
    EXPECT_EQ(pass1_changes, 3);
    ASSERT_TRUE(fs::is_regular_file(fs::u8path(backup_file)));
    EXPECT_EQ(exm::cp932_to_utf8_strict(exm::read_file(backup_file)), initial_utf8);
    EXPECT_EQ(exm::cp932_to_utf8_strict(exm::read_file(filepath)),
              "0,,nobody\r\n"
              "1,Ｍｉｙａｋａ,miyaka\r\n"
              "2,Ａｙａｋａ,ayaka\r\n");

    // Pass 2: update 美弥香 and drop 綾佳 -> must read pristine xtx_backup/ so
    // 美弥香 gets the updated spelling and 綾佳 reverts to original Japanese.
    const std::vector<std::pair<std::string, std::string>> pass2_table = {
        {"美弥香", "Ｍｉｙａｋａ　Ｋｏｔａｋｅ"},
    };
    const int pass2_changes = xtx::translate_xtx_file(filepath, pass2_table, backup_dir);
    EXPECT_EQ(pass2_changes, 1);
    EXPECT_EQ(exm::cp932_to_utf8_strict(exm::read_file(filepath)),
              "0,名前なし,nobody\r\n"
              "1,Ｍｉｙａｋａ　Ｋｏｔａｋｅ,miyaka\r\n"
              "2,綾佳,ayaka\r\n");
    EXPECT_EQ(exm::cp932_to_utf8_strict(exm::read_file(backup_file)), initial_utf8);
}

TEST(Xtx, TranslateSeedsXtxBackupFromDotBakWhenLiveAlreadyPatched) {
    const ScopedTempDir tmp("matsuro_ut_xtx_dotbak");
    fs::create_directories(tmp.path() / "spt");

    const std::string filepath = (tmp.path() / "spt" / "charaname.xtx").u8string();
    const std::string bak_path = filepath + ".bak";
    const std::string backup_dir = (tmp.path() / "xtx_backup").u8string();
    const std::string backup_file = (tmp.path() / "xtx_backup" / "charaname.xtx").u8string();

    const std::string pristine_utf8 =
        "0,名前なし,nobody\r\n"
        "1,美弥香,miyaka\r\n";
    const std::string already_patched_utf8 =
        "0,,nobody\r\n"
        "1,Ｓｔａｌｅ,miyaka\r\n";
    exm::write_file(bak_path, exm::utf8_to_cp932_replace(pristine_utf8));
    exm::write_file(filepath, exm::utf8_to_cp932_replace(already_patched_utf8));

    const int changes =
        xtx::translate_xtx_file(filepath, xtx::charaname_translations(), backup_dir);
    EXPECT_EQ(changes, 2);
    EXPECT_EQ(exm::cp932_to_utf8_strict(exm::read_file(backup_file)), pristine_utf8);
    EXPECT_EQ(exm::cp932_to_utf8_strict(exm::read_file(filepath)),
              "0,,nobody\r\n"
              "1,Ｍｉｙａｋａ,miyaka\r\n");
}

TEST(Xtx, TranslateMissingFileReturnsZeroWithoutSideEffects) {
    const ScopedTempDir tmp("matsuro_ut_xtx_missing");
    const std::string missing_file = (tmp.path() / "spt" / "charaname.xtx").u8string();
    const std::string backup_dir = (tmp.path() / "xtx_backup").u8string();
    EXPECT_EQ(xtx::translate_xtx_file(missing_file, xtx::charaname_translations(), backup_dir), 0);
    EXPECT_FALSE(fs::exists(fs::u8path(missing_file)));
    EXPECT_FALSE(fs::exists(fs::u8path(backup_dir)));
}

}  // namespace
