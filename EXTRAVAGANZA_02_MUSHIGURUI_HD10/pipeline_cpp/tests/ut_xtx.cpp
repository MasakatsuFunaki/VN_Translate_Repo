// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-room unit tests for mgi::xtx backup resolution and idempotent re-runs.
#include "xtx/xtx.h"

#include "common/util.h"
#include "scoped_temp_dir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace mgi;

TEST(XtxTranslate, PrefersXtxBackupOverMutatedLiveAndRevertsRemovedEntries) {
    const test_util::ScopedTempDir temp("mgi_ut_xtx_backup");
    const std::string spt_dir = temp.join_u8("spt");
    const std::string backup_dir = temp.join_u8("xtx_backup");
    fs::create_directories(fs::u8path(spt_dir));

    const std::string live_xtx = spt_dir + "\\charaname.xtx";
    const std::string backup_xtx = backup_dir + "\\charaname.xtx";

    const std::string pristine_utf8 =
        "no,名前なし\r\n"
        "aa,美弥香\r\n"
        "bb,レン\r\n"
        "empty,,tail\r\n";
    const Bytes pristine_cp932 = utf8_to_cp932_replace(pristine_utf8);
    write_file(live_xtx, pristine_cp932);

    // Pass 1: clean install creates xtx_backup/charaname.xtx and translates both.
    const std::vector<std::pair<std::string, std::string>> pass1_table = {
        {"美弥香", "Ｍｉｙａｋａ"},
        {"レン", "Ｒｅｎ"},
    };
    EXPECT_EQ(xtx::translate_xtx_file(live_xtx, pass1_table, backup_dir), 2);
    ASSERT_TRUE(fs::is_regular_file(fs::u8path(backup_xtx)));
    EXPECT_EQ(read_file(backup_xtx), pristine_cp932);

    const std::string expected_pass1 =
        "no,名前なし\r\n"
        "aa,Ｍｉｙａｋａ\r\n"
        "bb,Ｒｅｎ\r\n"
        "empty,,tail\r\n";
    EXPECT_EQ(read_file(live_xtx), utf8_to_cp932_replace(expected_pass1));

    // Pass 2 (shrinking table): remove "レン" from translations.  Must read
    // pristine xtx_backup/charaname.xtx (NOT mutated live) so "レン" reverts
    // back to Japanese while "美弥香" remains translated and backup stays intact.
    const std::vector<std::pair<std::string, std::string>> pass2_table = {
        {"美弥香", "Ｍｉｙａｋａ"},
    };
    EXPECT_EQ(xtx::translate_xtx_file(live_xtx, pass2_table, backup_dir), 1);
    EXPECT_EQ(read_file(backup_xtx), pristine_cp932);

    const std::string expected_pass2 =
        "no,名前なし\r\n"
        "aa,Ｍｉｙａｋａ\r\n"
        "bb,レン\r\n"
        "empty,,tail\r\n";
    EXPECT_EQ(read_file(live_xtx), utf8_to_cp932_replace(expected_pass2));
}

TEST(XtxTranslate, PrefersDotBakOverMutatedLiveAndHandlesMissingFile) {
    const test_util::ScopedTempDir temp("mgi_ut_xtx_dot_bak");
    const std::string spt_dir = temp.join_u8("spt");
    const std::string backup_dir = temp.join_u8("xtx_backup");
    fs::create_directories(fs::u8path(spt_dir));

    const std::string missing_xtx = spt_dir + "\\missing.xtx";
    EXPECT_EQ(xtx::translate_xtx_file(missing_xtx, {{"レン", "Ｒｅｎ"}}, backup_dir), 0);

    const std::string live_xtx = spt_dir + "\\charaname.xtx";
    const std::string bak_xtx = live_xtx + ".bak";
    const std::string backup_xtx = backup_dir + "\\charaname.xtx";

    const Bytes pristine_cp932 = utf8_to_cp932_replace("aa,美弥香\r\nbb,レン\r\n");
    const Bytes already_mutated = utf8_to_cp932_replace("aa,OLD_MUTATED\r\nbb,OLD_REN\r\n");
    write_file(bak_xtx, pristine_cp932);
    write_file(live_xtx, already_mutated);

    EXPECT_EQ(xtx::translate_xtx_file(live_xtx, {{"美弥香", "Ｍｉｙａｋａ"}}, backup_dir), 1);
    EXPECT_EQ(read_file(backup_xtx), pristine_cp932);
    EXPECT_EQ(read_file(live_xtx),
              utf8_to_cp932_replace("aa,Ｍｉｙａｋａ\r\nbb,レン\r\n"));
}
