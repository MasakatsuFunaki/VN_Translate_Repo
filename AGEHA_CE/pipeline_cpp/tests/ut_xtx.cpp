// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for src/xtx/xtx.{h,cpp} and src/xtx/xtx_tables.cpp: AGEHA_CE
// charaname.xtx table entries and CP932 in-place replacement with backup.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "common/util.h"
#include "xtx/xtx.h"

namespace fs = std::filesystem;
namespace xtx = exm::xtx;
using namespace exm;

namespace {

constexpr std::size_t kExpectedCharanameEntryCount = 3;

}  // namespace

TEST(Xtx, charaname_translations_table_matches_ageha_contract) {
    const auto& table = xtx::charaname_translations();
    ASSERT_EQ(table.size(), kExpectedCharanameEntryCount);
    EXPECT_EQ(table[0].first, "名前なし");
    EXPECT_EQ(table[0].second, "");
    EXPECT_EQ(table[1].first, "アゲハ");
    EXPECT_EQ(table[1].second, "Ａｇｅｈａ");
    EXPECT_EQ(table[2].first, "シュミ");
    EXPECT_EQ(table[2].second, "Ｓｈｕｍｉ");
}

TEST(Xtx, translate_xtx_file_rewrites_cp932_entries_and_creates_backup) {
    const fs::path temp_dir = fs::temp_directory_path() / "ageha_ut_xtx_dir";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir / "spt");

    const std::string filepath = (temp_dir / "spt" / "charaname.xtx").u8string();
    const std::string backup_dir = (temp_dir / "xtx_backup").u8string();
    const std::string backup_file = (temp_dir / "xtx_backup" / "charaname.xtx").u8string();

    const std::string initial_utf8 =
        "0,名前なし,nobody\r\n"
        "1,アゲハ,ageha\r\n"
        "2,シュミ,shumi\r\n";
    write_file(filepath, utf8_to_cp932_replace(initial_utf8));

    const int changes =
        xtx::translate_xtx_file(filepath, xtx::charaname_translations(), backup_dir);
    EXPECT_EQ(changes, static_cast<int>(kExpectedCharanameEntryCount));
    EXPECT_TRUE(fs::exists(fs::u8path(backup_file)));

    const auto decoded = cp932_to_utf8_strict(read_file(filepath));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded,
              "0,,nobody\r\n"
              "1,Ａｇｅｈａ,ageha\r\n"
              "2,Ｓｈｕｍｉ,shumi\r\n");

    const auto backup_decoded = cp932_to_utf8_strict(read_file(backup_file));
    ASSERT_TRUE(backup_decoded.has_value());
    EXPECT_EQ(*backup_decoded, initial_utf8);

    fs::remove_all(temp_dir);
}
