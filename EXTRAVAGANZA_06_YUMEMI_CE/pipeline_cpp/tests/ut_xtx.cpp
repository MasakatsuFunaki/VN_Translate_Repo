// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Component tests for src/xtx/xtx.{h,cpp} and src/xtx/xtx_tables.cpp:
// YUMEMIRYOJOKU_CE charaname.xtx table entries and CP932 in-place replacement
// with backup.
#include "xtx/xtx.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;
namespace xtx = exm::xtx;
using namespace exm;

namespace {

constexpr std::size_t kExpectedCharanameEntryCount = 3;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_);
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

}  // namespace

TEST(Xtx, charaname_translations_table_matches_yumemi_contract) {
    const auto& table = xtx::charaname_translations();
    ASSERT_EQ(table.size(), kExpectedCharanameEntryCount);
    EXPECT_EQ(table[0].first, "名前なし");
    EXPECT_EQ(table[0].second, "");
    EXPECT_EQ(table[1].first, "夢美１");
    EXPECT_EQ(table[1].second, "Ｙｕｍｅｍｉ　１");
    EXPECT_EQ(table[2].first, "夢美");
    EXPECT_EQ(table[2].second, "Ｙｕｍｅｍｉ");
}

TEST(Xtx, translate_xtx_file_rewrites_cp932_entries_and_creates_backup) {
    const ScopedTempDir temp_dir("yumemi_ut_xtx_dir");
    fs::create_directories(temp_dir.path() / "spt");

    const std::string filepath = (temp_dir.path() / "spt" / "charaname.xtx").u8string();
    const std::string backup_dir = (temp_dir.path() / "xtx_backup").u8string();
    const std::string backup_file = (temp_dir.path() / "xtx_backup" / "charaname.xtx").u8string();

    const std::string initial_utf8 =
        "no,名前なし\r\n"
        "aa,夢美１\r\n"
        "ya,夢美";
    write_file(filepath, utf8_to_cp932_replace(initial_utf8));

    const int changes =
        xtx::translate_xtx_file(filepath, xtx::charaname_translations(), backup_dir);
    EXPECT_EQ(changes, static_cast<int>(kExpectedCharanameEntryCount));
    EXPECT_TRUE(fs::exists(fs::u8path(backup_file)));

    const auto decoded = cp932_to_utf8_strict(read_file(filepath));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded,
              "no,\r\n"
              "aa,Ｙｕｍｅｍｉ　１\r\n"
              "ya,Ｙｕｍｅｍｉ");

    const auto backup_decoded = cp932_to_utf8_strict(read_file(backup_file));
    ASSERT_TRUE(backup_decoded.has_value());
    EXPECT_EQ(*backup_decoded, initial_utf8);
}
