// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Functional-contract and component tests for XTX two-table byte substitution
// round-trip and WelcomeToAnzU!_CE charaname.xtx translation.
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "common/util.h"
#include "xtx/xtx.h"

namespace fs = std::filesystem;
namespace xtx = exm::xtx;
using namespace exm;

namespace {

constexpr int kExpectedAnzUCharanameChanges = 5;

}  // namespace

TEST(XtxContract, TwoTableCipherRoundTripsAll256ByteValues) {
    std::vector<std::uint8_t> all_bytes(xtx::kXtxByteTableSize);
    for (std::size_t i = 0; i < xtx::kXtxByteTableSize; ++i) {
        all_bytes[i] = static_cast<std::uint8_t>(i);
    }
    const auto encoded = xtx::encode_xtx(all_bytes);
    EXPECT_NE(encoded, all_bytes);
    const auto decoded = xtx::decode_xtx(encoded);
    EXPECT_EQ(decoded, all_bytes);
}

TEST(XtxComponent, TranslateCharanameXtxRewritesFullwidthNamesIdempotently) {
    const fs::path temp_dir = fs::temp_directory_path() / "anzu_ce_ut_xtx_dir";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir / "spt");

    const std::string filepath = (temp_dir / "spt" / "charaname.xtx").u8string();
    const std::string backup_dir = (temp_dir / "xtx_backup").u8string();

    const std::string initial_utf8 =
        "no,名前なし\r\n"
        "aa,夢美\r\n"
        "bb,アゲハ\r\n"
        "za,夢美\r\n"
        "zb,アゲハ\r\n";
    write_file(filepath, utf8_to_cp932_replace(initial_utf8));

    const int changes1 =
        xtx::translate_xtx_file(filepath, xtx::charaname_translations(), backup_dir);
    EXPECT_EQ(changes1, kExpectedAnzUCharanameChanges);
    EXPECT_TRUE(fs::exists(temp_dir / "xtx_backup" / "charaname.xtx"));
    EXPECT_TRUE(fs::exists(temp_dir / "xtx_backup" / "spt" / "charaname.xtx"));

    const auto decoded = cp932_to_utf8_strict(read_file(filepath));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded,
              "no,\r\n"
              "aa,Ｙｕｍｅｍｉ\r\n"
              "bb,Ａｇｅｈａ\r\n"
              "za,Ｙｕｍｅｍｉ\r\n"
              "zb,Ａｇｅｈａ\r\n");

    const int changes2 =
        xtx::translate_xtx_file(filepath, xtx::charaname_translations(), backup_dir);
    EXPECT_EQ(changes2, kExpectedAnzUCharanameChanges);

    fs::remove_all(temp_dir, ec);
}
