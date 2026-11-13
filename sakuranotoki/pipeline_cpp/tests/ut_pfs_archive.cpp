// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Artemis PF8 archive unit tests using synthetic archives built in memory and
// temporary files under std::filesystem::temp_directory_path().
#include "artemis/pfs_archive.h"
#include "common/util.h"
#include "pfs_fixture.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;
using namespace snt;
using snt::test_fixture::make_pf8;

constexpr std::uint8_t kKeyStride = 7U;
constexpr std::uint8_t kKeyBias = 1U;
constexpr std::uint8_t kOffsetKeyBias = 3U;
constexpr std::size_t kWholePayloadSize = 45;
constexpr std::size_t kSplitOffset = 25;
constexpr std::size_t kExpectedTwoEntries = 2;
constexpr std::size_t kExpectedSingleEntry = 1;
constexpr std::uint8_t kSampleHighByte0 = 250U;
constexpr std::uint8_t kSampleHighByte1 = 251U;
constexpr std::uint8_t kSampleHighByte2 = 252U;
constexpr std::uint8_t kPngLeadByte = 0x89U;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view utf8_name)
        : path_(fs::temp_directory_path() / fs::u8path(utf8_name)) {
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

}  // namespace

TEST(PfsArchive, xor_is_its_own_inverse) {
    pfs::Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(i * kKeyStride + kKeyBias);
    }
    Bytes data{1, 2, 3, 4, 5, kSampleHighByte0, kSampleHighByte1, kSampleHighByte2};
    const Bytes orig = data;
    pfs::xor_decrypt(data, key, 0);
    EXPECT_NE(data, orig);
    pfs::xor_decrypt(data, key, 0);
    EXPECT_EQ(data, orig);
}

TEST(PfsArchive, xor_start_offset_keeps_key_alignment) {
    pfs::Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(i + kOffsetKeyBias);
    }
    Bytes whole(kWholePayloadSize);
    for (std::size_t i = 0; i < whole.size(); ++i) {
        whole[i] = static_cast<std::uint8_t>(i);
    }
    Bytes expect = whole;
    pfs::xor_decrypt(expect, key, 0);

    Bytes head(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(kSplitOffset));
    Bytes tail(whole.begin() + static_cast<std::ptrdiff_t>(kSplitOffset), whole.end());
    pfs::xor_decrypt(head, key, 0);
    pfs::xor_decrypt(tail, key, kSplitOffset);
    head.insert(head.end(), tail.begin(), tail.end());
    EXPECT_EQ(head, expect);
}

TEST(PfsArchive, round_trips_entries) {
    const Bytes payload_a{'a', 's', 't', 'v', 'e', 'r', ' ', '=', ' ', '2'};
    const Bytes payload_b{kPngLeadByte, 'P', 'N', 'G'};
    const Bytes arc = make_pf8(
        {{"script\\01_01.ast", payload_a}, {"image\\ev\\a.png", payload_b}});

    ScopedTempDir temp("snt_ut_pfs_round_trip");
    const std::string path =
        (temp.path() / fs::u8path("snt_ut.pfs")).u8string();
    write_file(path, arc);

    const auto entries = pfs::list_pfs(path);
    ASSERT_EQ(entries.size(), kExpectedTwoEntries);
    EXPECT_EQ(entries[0].name, "script\\01_01.ast");
    EXPECT_EQ(entries[0].size, payload_a.size());
    EXPECT_EQ(entries[1].name, "image\\ev\\a.png");

    const pfs::Key key = pfs::archive_key(path);
    EXPECT_EQ(pfs::read_entry(path, 0, key), payload_a);
    EXPECT_EQ(pfs::read_entry(path, 1, key), payload_b);
}

TEST(PfsArchive, rejects_bad_magic) {
    ScopedTempDir temp("snt_ut_pfs_bad_magic");
    const std::string path =
        (temp.path() / fs::u8path("snt_ut_bad.pfs")).u8string();
    write_file(path, Bytes{'x', 'f', '8', 0, 0, 0, 0});
    EXPECT_THROW(pfs::list_pfs(path), std::runtime_error);
}

TEST(PfsArchive, overlay_prefers_later_archives) {
    ScopedTempDir temp("snt_ut_pfs_overlay");
    const fs::path& dir = temp.path();

    write_file((dir / fs::u8path("sakuranotoki.pfs")).u8string(),
               make_pf8({{"script\\a.ast", Bytes{'b', 'a', 's', 'e'}}}));
    write_file((dir / fs::u8path("sakuranotoki.pfs.010")).u8string(),
               make_pf8({{"script\\a.ast", Bytes{'p', 'a', 't', 'c', 'h'}}}));

    const pfs::Overlay ov = pfs::build_overlay(dir.u8string());
    ASSERT_EQ(ov.entries.size(), kExpectedSingleEntry);
    const auto& [arc_path, idx] = ov.entries.at("script\\a.ast");
    EXPECT_NE(arc_path.find(".pfs.010"), std::string::npos) << "patch must win";
    EXPECT_EQ(pfs::read_entry(arc_path, idx, ov.keys.at(arc_path)),
              (Bytes{'p', 'a', 't', 'c', 'h'}));
}
