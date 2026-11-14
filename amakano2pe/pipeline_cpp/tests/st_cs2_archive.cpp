// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System test validating KifArchive against the installed game's scene.int.
#include "cs2/cs2_archive.h"
#include "hg3/hg3_codec.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMinExpectedSceneEntries = 100;
constexpr const char kDefaultGameDir[] =
    "C:\\あざらしそふと\\アマカノ2～Perfect Edition～";

fs::path resolve_game_dir() {
    const wchar_t* const wenv = _wgetenv(L"VN_GAME_DIR");
    if (wenv != nullptr && *wenv != L'\0') {
        return fs::path(wenv);
    }
    return fs::u8path(kDefaultGameDir);
}

}  // namespace

TEST(Cs2Archive, real_scene_int_opens) {
    const fs::path scene = resolve_game_dir() / "scene.int";
    if (!fs::exists(scene)) {
        GTEST_SKIP() << "scene.int not installed";
    }
    const ama::cs2::KifArchive arc(scene.u8string());
    EXPECT_GT(arc.entries().size(), kMinExpectedSceneEntries);
}

TEST(Cs2Archive, real_v3_archives_deobfuscate_and_slice_read) {
    const fs::path game_dir = resolve_game_dir();
    const fs::path exe_path = game_dir / "cs2.exe";
    if (!fs::exists(exe_path)) {
        GTEST_SKIP() << "cs2.exe not installed";
    }
    const std::string vcode2 = ama::cs2::read_vcode2_from_exe(exe_path.u8string());
    EXPECT_FALSE(vcode2.empty());
    const ama::cs2::KifNameCipher cipher(vcode2);

    const ama::cs2::KifArchive img00((game_dir / "image00.int").u8string(), cipher);
    EXPECT_NE(img00.find("ex_cgmode.hg3"), nullptr);

    const ama::cs2::KifArchive img01((game_dir / "image01.int").u8string(), cipher);
    const auto* sys_cfg = img01.find("sys_config.hg3");
    const auto* sl_base = img01.find("sl_base.hg3");
    ASSERT_NE(sys_cfg, nullptr);
    ASSERT_NE(sl_base, nullptr);
    const ama::Bytes raw = img01.raw_slice(*sys_cfg);
    const ama::Bytes dec = img01.decrypt_slice(raw);
    ASSERT_GE(dec.size(), 4u);
    EXPECT_EQ(dec[0], 'H');
    EXPECT_EQ(dec[1], 'G');
    EXPECT_EQ(dec[2], '-');
    EXPECT_EQ(dec[3], '3');

    const ama::cs2::KifArchive fes((game_dir / "fes.int").u8string(), cipher);
    const auto* cfg_basic = fes.find("config_basic.fes");
    ASSERT_NE(cfg_basic, nullptr);
    const ama::Bytes fes_bytes = fes.extract(*cfg_basic);
    ASSERT_GE(fes_bytes.size(), 4u);
    EXPECT_EQ(fes_bytes[0], 'F');
    EXPECT_EQ(fes_bytes[1], 'E');
    EXPECT_EQ(fes_bytes[2], 'S');
    EXPECT_EQ(fes_bytes[3], '\0');

    const ama::cs2::KifArchive cfg((game_dir / "config.int").u8string(), cipher);
    EXPECT_NE(cfg.find("adv.xml"), nullptr);

    const ama::cs2::KifArchive kcs((game_dir / "kcs.int").u8string(), cipher);
    const auto* main_kcs = kcs.find("main.kcs");
    ASSERT_NE(main_kcs, nullptr);
    const ama::Bytes kcs_bytes = kcs.extract(*main_kcs);
    ASSERT_GE(kcs_bytes.size(), 4u);
    EXPECT_EQ(kcs_bytes[0], 'K');
    EXPECT_EQ(kcs_bytes[1], 'C');
    EXPECT_EQ(kcs_bytes[2], 'S');
    EXPECT_EQ(kcs_bytes[3], '\0');

    const ama::hg3::Hg3Container boot_hg3 =
        ama::hg3::Hg3Codec::DecodeFile(game_dir / "boot.hg3");
    ASSERT_EQ(boot_hg3.frames.size(), 1u);
    EXPECT_TRUE(boot_hg3.frames[0].top_down_img0000);
}
