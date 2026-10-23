// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Artemis PF8 archive tests.  The reader is validated against the real .pfs
// files when the game is installed; these tests must run without it, so they
// build a synthetic archive with the same construction (SHA-1-of-index XOR
// key) and round-trip it.
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "artemis/pfs_archive.h"
#include "common/util.h"
#include "pfs_fixture.h"

namespace fs = std::filesystem;
using namespace snt;
using snt::test_fixture::make_pf8;

TEST(PfsArchive, xor_is_its_own_inverse) {
    pfs::Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i * 7 + 1);
    Bytes data{1, 2, 3, 4, 5, 250, 251, 252};
    const Bytes orig = data;
    pfs::xor_decrypt(data, key, 0);
    EXPECT_NE(data, orig);
    pfs::xor_decrypt(data, key, 0);
    EXPECT_EQ(data, orig);
}

// The key cycles modulo 20 from `start_offset`, so a piecewise read stays
// aligned with a whole-buffer read.
TEST(PfsArchive, xor_start_offset_keeps_key_alignment) {
    pfs::Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i + 3);
    Bytes whole(45);
    for (std::size_t i = 0; i < whole.size(); ++i) whole[i] = static_cast<std::uint8_t>(i);
    Bytes expect = whole;
    pfs::xor_decrypt(expect, key, 0);

    Bytes head(whole.begin(), whole.begin() + 25);
    Bytes tail(whole.begin() + 25, whole.end());
    pfs::xor_decrypt(head, key, 0);
    pfs::xor_decrypt(tail, key, 25);
    head.insert(head.end(), tail.begin(), tail.end());
    EXPECT_EQ(head, expect);
}

TEST(PfsArchive, round_trips_entries) {
    const Bytes payload_a{'a', 's', 't', 'v', 'e', 'r', ' ', '=', ' ', '2'};
    const Bytes payload_b{0x89, 'P', 'N', 'G'};
    Bytes arc = make_pf8({{"script\\01_01.ast", payload_a}, {"image\\ev\\a.png", payload_b}});

    const std::string path = (fs::temp_directory_path() / "snt_ut.pfs").u8string();
    write_file(path, arc);

    auto entries = pfs::list_pfs(path);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "script\\01_01.ast");
    EXPECT_EQ(entries[0].size, payload_a.size());
    EXPECT_EQ(entries[1].name, "image\\ev\\a.png");

    const pfs::Key key = pfs::archive_key(path);
    EXPECT_EQ(pfs::read_entry(path, 0, key), payload_a);
    EXPECT_EQ(pfs::read_entry(path, 1, key), payload_b);

    fs::remove(fs::u8path(path));
}

TEST(PfsArchive, rejects_bad_magic) {
    const std::string path = (fs::temp_directory_path() / "snt_ut_bad.pfs").u8string();
    write_file(path, Bytes{'x', 'f', '8', 0, 0, 0, 0});
    EXPECT_THROW(pfs::list_pfs(path), std::runtime_error);
    fs::remove(fs::u8path(path));
}

// Later archives in the load order override matching paths.
TEST(PfsArchive, overlay_prefers_later_archives) {
    const fs::path dir = fs::temp_directory_path() / "snt_ut_overlay";
    fs::remove_all(dir);
    fs::create_directories(dir);

    write_file((dir / "sakuranotoki.pfs").u8string(),
               make_pf8({{"script\\a.ast", Bytes{'b', 'a', 's', 'e'}}}));
    write_file((dir / "sakuranotoki.pfs.010").u8string(),
               make_pf8({{"script\\a.ast", Bytes{'p', 'a', 't', 'c', 'h'}}}));

    pfs::Overlay ov = pfs::build_overlay(dir.u8string());
    ASSERT_EQ(ov.entries.size(), 1u);
    const auto& [arc_path, idx] = ov.entries.at("script\\a.ast");
    EXPECT_NE(arc_path.find(".pfs.010"), std::string::npos) << "patch must win";
    EXPECT_EQ(pfs::read_entry(arc_path, idx, ov.keys.at(arc_path)),
              (Bytes{'p', 'a', 't', 'c', 'h'}));

    fs::remove_all(dir);
}

// Real-archive gate: skips cleanly when the game isn't installed.  The game
// folder differs per machine, so it comes from the environment -- a compiled-in
// path would ship one machine's layout and skip everywhere else in silence.
TEST(PfsArchive, real_archive_opens) {
    const char* game_dir = std::getenv("VN_GAME_DIR");
    if (!game_dir || !*game_dir)
        GTEST_SKIP() << "set VN_GAME_DIR to the game folder to run this check";
    const fs::path base = fs::u8path(game_dir) / "sakuranotoki.pfs";
    if (!fs::exists(base))
        GTEST_SKIP() << "no sakuranotoki.pfs under VN_GAME_DIR (" << game_dir << ")";
    EXPECT_GT(pfs::list_pfs(base.u8string()).size(), 100u);
}
