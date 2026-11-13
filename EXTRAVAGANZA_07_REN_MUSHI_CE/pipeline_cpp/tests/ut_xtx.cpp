// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Component tests for src/xtx/xtx.{h,cpp} and src/xtx/xtx_tables.cpp:
// Ren_mushi_CE charaname.xtx CP932 in-place replacement with backup.
#include "xtx/xtx.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;
namespace xtx = exm::xtx;
using namespace exm;

namespace {

constexpr int kExpectedReplacedLineCount = 10;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
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

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

}  // namespace

TEST(Xtx, translate_xtx_file_rewrites_cp932_entries_and_creates_backup) {
    const ScopedTempDir tmp("ren_mushi_ut_xtx_dir");
    const fs::path& temp_dir = tmp.path();
    fs::create_directories(temp_dir / "spt");

    const std::string filepath = (temp_dir / "spt" / "charaname.xtx").u8string();
    const std::string backup_dir = (temp_dir / "xtx_backup").u8string();
    const std::string backup_file = (temp_dir / "xtx_backup" / "charaname.xtx").u8string();

    const std::string initial_utf8 =
        "no,名前なし\r\n"
        "aa,夢美\r\n"
        "bb,アゲハ\r\n"
        "cc,ユーリア\r\n"
        "dd,サユリ\r\n"
        "ee,杏子\r\n"
        "ff,美弥香\r\n"
        "gg,唯\r\n"
        "hh,綾佳\r\n"
        "ii,遥\r\n";
    write_file(filepath, utf8_to_cp932_replace(initial_utf8));

    const int changes =
        xtx::translate_xtx_file(filepath, xtx::charaname_translations(), backup_dir);
    EXPECT_EQ(changes, kExpectedReplacedLineCount);
    EXPECT_TRUE(fs::exists(fs::u8path(backup_file)));

    const auto decoded = cp932_to_utf8_strict(read_file(filepath));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded,
              "no,\r\n"
              "aa,Ｙｕｍｅｍｉ\r\n"
              "bb,Ａｇｅｈａ\r\n"
              "cc,Ｙｕｌｉａ\r\n"
              "dd,Ｓａｙｕｒｉ\r\n"
              "ee,Ａｎｚｕ\r\n"
              "ff,Ｍｉｙａｋａ\r\n"
              "gg,Ｙｕｉ\r\n"
              "hh,Ａｙａｋａ\r\n"
              "ii,Ｈａｒｕｋａ\r\n");

    const auto backup_decoded = cp932_to_utf8_strict(read_file(backup_file));
    ASSERT_TRUE(backup_decoded.has_value());
    EXPECT_EQ(*backup_decoded, initial_utf8);
}
