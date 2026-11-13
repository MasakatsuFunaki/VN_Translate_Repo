// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for output-formatting helpers and synthetic deployment resolution.
#include "common/util.h"
#include "patch/deploy.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using namespace ksh;

namespace {

constexpr int kHexWidth0 = 0;
constexpr int kHexWidth4 = 4;
constexpr int kHexWidth8 = 8;
constexpr std::uint64_t kSampleHexShort = 0x1AFull;
constexpr std::uint64_t kSampleHexWide = 0x123456ull;
constexpr std::uint64_t kSampleHexLower = 0x42896Bull;
constexpr std::int64_t kSampleSignedPos = 0x3C35LL;
constexpr std::int64_t kSampleSignedNegSmall = -5LL;
constexpr std::int64_t kSampleSignedNegWide = -0x12345678LL;
constexpr std::size_t kPadWidth4 = 4;
constexpr std::size_t kEmbeddedNulSpanLen = 3;

class ScopedDeployTempDir {
public:
    explicit ScopedDeployTempDir(const char* leaf)
        : path_(fs::temp_directory_path() / leaf) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedDeployTempDir(const ScopedDeployTempDir&) = delete;
    ScopedDeployTempDir& operator=(const ScopedDeployTempDir&) = delete;

    ~ScopedDeployTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void WriteBinaryPayload(const fs::path& file_path, const std::string& payload) {
    fs::create_directories(file_path.parent_path());
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}

void WriteHelperPair(const fs::path& dir) {
    WriteBinaryPayload(dir / "alpharom_helper.dll", "DLL");
    WriteBinaryPayload(dir / "launch.exe", "EXE");
}

}  // namespace

TEST(Format, hexu_pads_uppercase_to_the_field_width) {
    EXPECT_EQ(hexu(kSampleHexShort, kHexWidth8), "000001AF");
    EXPECT_EQ(hexu(kSampleHexShort, kHexWidth0), "1AF");
    EXPECT_EQ(hexu(0, kHexWidth4), "0000");
    EXPECT_EQ(hexu(kSampleHexWide, kHexWidth4), "123456");
}

TEST(Format, hexl_is_lowercase_with_an_0x_prefix) {
    EXPECT_EQ(hexl(kSampleHexLower), "0x42896b");
    EXPECT_EQ(hexl(0), "0x0");
}

TEST(Format, hexu_signed_pads_the_sign_into_the_field) {
    EXPECT_EQ(hexu_signed(kSampleSignedPos, kHexWidth8), "00003C35");
    EXPECT_EQ(hexu_signed(kSampleSignedNegSmall, kHexWidth8), "-0000005");
    EXPECT_EQ(hexu_signed(kSampleSignedNegWide, kHexWidth8), "-12345678");
}

TEST(Format, hex_bytes_and_hex_lower) {
    const Bytes b{0x00u, 0x1Fu, 0xABu, 0xFFu};
    EXPECT_EQ(hex_bytes(b), "00 1F AB FF");
    EXPECT_EQ(hex_lower(b.data(), b.size()), "001fabff");
    EXPECT_EQ(hex_bytes(Bytes{}), "");
}

TEST(Format, ljust_pads_but_never_truncates) {
    EXPECT_EQ(ljust("ab", kPadWidth4), "ab  ");
    EXPECT_EQ(ljust("abcdef", kPadWidth4), "abcdef");
}

TEST(Format, bytes_repr_escaping_and_quote_choice_are_pinned) {
    auto repr = [](const std::string& s) {
        return bytes_repr(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    };
    EXPECT_EQ(repr("abc"), "b'abc'");
    EXPECT_EQ(repr(std::string("\\\\.\\SICE")), "b'\\\\\\\\.\\\\SICE'");
    EXPECT_EQ(repr(std::string("a\0b", kEmbeddedNulSpanLen)), "b'a\\x00b'");
    EXPECT_EQ(repr("\t\n\r"), "b'\\t\\n\\r'");
    EXPECT_EQ(repr("\x7F\x80\xFF"), "b'\\x7f\\x80\\xff'");
    EXPECT_EQ(repr("it's"), "b\"it's\"");
    EXPECT_EQ(repr("it's \"x\""), "b'it\\'s \"x\"'");
}

TEST(PathJoin, does_not_double_or_invent_separators) {
    using patch::path_join;
    EXPECT_EQ(path_join(R"(X:\Synthetic\KH)", "launch.exe"), R"(X:\Synthetic\KH\launch.exe)");
    EXPECT_EQ(path_join(R"(X:\Synthetic\KH\)", "launch.exe"), R"(X:\Synthetic\KH\launch.exe)");
    EXPECT_EQ(path_join("X:/Synthetic/KH/", "launch.exe"), "X:/Synthetic/KH/launch.exe");
    EXPECT_EQ(path_join("X:", "launch.exe"), "X:launch.exe");
    EXPECT_EQ(path_join("", "launch.exe"), "launch.exe");
}

TEST(PathJoin, base_name_takes_either_separator) {
    EXPECT_EQ(patch::base_name(R"(X:\a\b\launch.exe)"), "launch.exe");
    EXPECT_EQ(patch::base_name("a/b/launch.exe"), "launch.exe");
    EXPECT_EQ(patch::base_name("launch.exe"), "launch.exe");
}

TEST(CanWrite, probes_the_directory_rather_than_trusting_acls) {
    const ScopedDeployTempDir temp("vn_ut_can_write_kishin_hisho");
    EXPECT_TRUE(patch::can_write(temp.path().u8string()));
    EXPECT_FALSE(patch::can_write((temp.path() / "no_such_dir_here").u8string()));

    const fs::path regular_file = temp.path() / "README.md";
    WriteBinaryPayload(regular_file, "synthetic regular file");
    EXPECT_FALSE(patch::can_write(regular_file.u8string()));
}

TEST(DeployResolution, probes_release_zip_before_install_and_dev_trees) {
    const ScopedDeployTempDir temp("vn_ut_deploy_kishin_hisho");
    const fs::path release_root = temp.path() / "release_zip";
    const fs::path release_bin = release_root / "bin";
    const fs::path release_game = release_root / "game";
    fs::create_directories(release_bin);
    WriteHelperPair(release_game);
    WriteBinaryPayload(release_game / "0script.pak", "PAK_DATA");
    WriteBinaryPayload(release_game / "script" / "da0100.nps", "NPS_DATA");

    EXPECT_EQ(fs::u8path(patch::find_helper_artifact_dir(
                  release_root.u8string(), release_bin.u8string())),
              release_game);
    EXPECT_EQ(fs::u8path(patch::find_staged_game_dir(
                  release_root.u8string(), release_bin.u8string())),
              release_game);

    const fs::path target_game = temp.path() / "installed_game";
    fs::create_directories(target_game);
    patch::deploy_script_artifacts(
        release_root.u8string(), target_game.u8string(), release_game.u8string());

    EXPECT_TRUE(fs::is_regular_file(target_game / "0script.pak"));
    EXPECT_TRUE(fs::is_regular_file(target_game / "script" / "da0100.nps"));
}

TEST(DeployResolution, probes_sibling_game_and_dev_helper_release_fallbacks) {
    const ScopedDeployTempDir temp("vn_ut_deploy_kishin_hisho_dev");
    const fs::path checkout = temp.path() / "checkout";
    const fs::path install_bin = checkout / "build" / "install" / "bin";
    const fs::path install_game = checkout / "build" / "install" / "game";
    const fs::path helper_release = checkout / "build" / "helper" / "Release";
    const fs::path pipeline_release = checkout / "build" / "pipeline" / "Release";
    fs::create_directories(install_bin);
    fs::create_directories(pipeline_release);

    WriteHelperPair(helper_release);
    EXPECT_EQ(fs::u8path(patch::find_helper_artifact_dir(
                  checkout.u8string(), pipeline_release.u8string())),
              helper_release);

    WriteHelperPair(install_game);
    WriteBinaryPayload(install_game / "0script.pak", "STAGED_PAK");
    EXPECT_EQ(fs::u8path(patch::find_helper_artifact_dir(
                  checkout.u8string(), install_bin.u8string())),
              install_game);
    EXPECT_EQ(fs::u8path(patch::find_staged_game_dir(
                  checkout.u8string(), install_bin.u8string())),
              install_game);
}
