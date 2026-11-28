// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: UTF-8 project root discovery across
// repository checkouts and staged layouts (including Japanese directory names),
// UTF-8 CLI argument parsing, and Windows CommandLineToArgvW quoting round-trips.
#include "apps/paths.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace apps = exm::apps;

namespace {

constexpr std::string_view kTempRootName = "vn_ut_paths_ext05";
constexpr int kNestedEmptyLevels = 8;
constexpr int kExitHelp = 0;
constexpr int kExitError = 1;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
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

fs::path append_empty_levels(const fs::path& base, int count) {
    fs::path current = base;
    for (int i = 0; i < count; ++i) {
        current /= "pad";
    }
    fs::create_directories(current);
    return current;
}

void write_dummy_file(const fs::path& file_path) {
    fs::create_directories(file_path.parent_path());
    std::ofstream out(file_path, std::ios::binary);
    out << "# placeholder\n";
}

}  // namespace

TEST(Paths, FindProjectDirResolvesReleaseCheckoutLegacyJapaneseAndRejectsUnrelated) {
    const ScopedTempDir temp(kTempRootName);
    const fs::path pad_base = append_empty_levels(temp.path(), kNestedEmptyLevels);

    // 1. Release zip layout (bin + game without script_output).
    const fs::path release_root = pad_base / "release_root";
    fs::create_directories(release_root / "bin");
    fs::create_directories(release_root / "game");
    EXPECT_EQ(apps::find_project_dir_from(release_root / "bin"), release_root.u8string());

    // 2. Repo checkout priority beats nested build/install/{bin,game}.
    const fs::path checkout = pad_base / "checkout";
    fs::create_directories(checkout / "pipeline_cpp");
    write_dummy_file(checkout / "build.py");
    fs::create_directories(checkout / "build" / "install" / "bin");
    fs::create_directories(checkout / "build" / "install" / "game");
    EXPECT_EQ(apps::find_project_dir_from(checkout / "build" / "install" / "bin"),
              checkout.u8string());

    // 3. Legacy staged layout (bin + script_output).
    const fs::path legacy_root = pad_base / "legacy_root";
    fs::create_directories(legacy_root / "bin");
    fs::create_directories(legacy_root / "script_output");
    EXPECT_EQ(apps::find_project_dir_from(legacy_root / "bin"), legacy_root.u8string());

    // 4. Japanese UTF-8 directory names in both checkout and staged layouts.
    const fs::path jp_checkout = pad_base / fs::u8path("蟲愛ずる少女_CE");
    fs::create_directories(jp_checkout / "pipeline_cpp");
    write_dummy_file(jp_checkout / "build.py");
    fs::create_directories(jp_checkout / "build" / "install" / "bin");
    EXPECT_EQ(apps::find_project_dir_from(jp_checkout / "build" / "install" / "bin"),
              jp_checkout.u8string());

    const fs::path jp_staged = pad_base / fs::u8path("EXTRAVAGANZA～蟲愛編～");
    fs::create_directories(jp_staged / "bin");
    fs::create_directories(jp_staged / "game");
    EXPECT_EQ(apps::find_project_dir_from(jp_staged / "bin"), jp_staged.u8string());

    // 5. Unrelated deep directory and empty path return empty.
    EXPECT_TRUE(apps::find_project_dir_from(pad_base).empty());
    EXPECT_TRUE(apps::find_project_dir_from(fs::path{}).empty());
}

TEST(Paths, ParseCommandLineHandlesCommonGameOnlyAndJapaneseUtf8Options) {
    {
        std::string project;
        std::string game;
        apps::po::options_description desc("test_common");
        desc.add(apps::common_options(project, game));

        apps::po::variables_map vm_help;
        const char* argv_help[] = {"test_common", "--help"};
        const auto rc_help = apps::parse_command_line(
            static_cast<int>(std::size(argv_help)),
            const_cast<char**>(argv_help), desc, vm_help);
        ASSERT_TRUE(rc_help.has_value());
        EXPECT_EQ(*rc_help, kExitHelp);

        apps::po::variables_map vm_missing;
        const char* argv_missing[] = {"test_common", "--dir", "D:\\Project"};
        const auto rc_missing = apps::parse_command_line(
            static_cast<int>(std::size(argv_missing)),
            const_cast<char**>(argv_missing), desc, vm_missing);
        ASSERT_TRUE(rc_missing.has_value());
        EXPECT_EQ(*rc_missing, kExitError);

        apps::po::variables_map vm_ok;
        const std::vector<std::string> utf8_args = {
            "--dir", "D:\\プロジェクト\\うぇるかむとぅＡｎｚＵ！_CE",
            "--game-dir", "E:\\ゲーム\\EXTRAVAGANZA～蟲愛編～"
        };
        const auto rc_ok = apps::parse_command_line_args(utf8_args, desc, vm_ok);
        EXPECT_FALSE(rc_ok.has_value());
        EXPECT_EQ(project, "D:\\プロジェクト\\うぇるかむとぅＡｎｚＵ！_CE");
        EXPECT_EQ(game, "E:\\ゲーム\\EXTRAVAGANZA～蟲愛編～");
    }

    {
        std::string game;
        apps::po::options_description desc("test_game_only");
        desc.add(apps::game_only_options(game));

        apps::po::variables_map vm_ok;
        const std::vector<std::string> utf8_args = {
            "--game-dir", "E:\\ゲーム\\蟲愛ずる少女_CE"
        };
        const auto rc_ok = apps::parse_command_line_args(utf8_args, desc, vm_ok);
        EXPECT_FALSE(rc_ok.has_value());
        EXPECT_EQ(game, "E:\\ゲーム\\蟲愛ずる少女_CE");

        apps::po::variables_map vm_reject_dir;
        const char* argv_reject_dir[] = {"test_game_only", "--dir", "D:\\Project",
                                         "--game-dir", "E:\\GameOnly"};
        const auto rc_reject_dir = apps::parse_command_line(
            static_cast<int>(std::size(argv_reject_dir)),
            const_cast<char**>(argv_reject_dir), desc, vm_reject_dir);
        ASSERT_TRUE(rc_reject_dir.has_value());
        EXPECT_EQ(*rc_reject_dir, kExitError);
    }
}

TEST(Paths, BuildWindowsCommandLineRoundTripsThroughCommandLineToArgvW) {
    const std::string exe_normal = "C:\\Tools\\うぇるかむとぅＡｎｚＵ！_CE\\01_extract.exe";
    const std::vector<std::string> args = {
        "--dir",
        "D:\\ゲーム\\蟲愛ずる少女_CE\\",
        "--game-dir",
        "E:\\VN\\EXTRAVAGANZA～蟲愛編～\\",
        "quoted \"inner\" value",
        "slash_before_quote\\\"end",
        ""
    };

    const std::wstring built = apps::build_windows_command_line(exe_normal, args);
    const std::vector<std::string> decoded = apps::split_windows_command_line_utf8(built);
    ASSERT_EQ(decoded.size(), args.size() + 1);
    EXPECT_EQ(decoded[0], exe_normal);
    for (std::size_t i = 0; i < args.size(); ++i) {
        EXPECT_EQ(decoded[i + 1], args[i]) << "arg index " << i;
    }

    const std::string exe_trailing_slash = "C:\\Tools\\蟲愛ずる少女_CE\\";
    const std::vector<std::string> simple_args = {"--game-dir", "D:\\うぇるかむとぅＡｎｚＵ！_CE\\"};
    const std::wstring built_trailing =
        apps::build_windows_command_line(exe_trailing_slash, simple_args);
    const std::vector<std::string> decoded_trailing =
        apps::split_windows_command_line_utf8(built_trailing);
    ASSERT_EQ(decoded_trailing.size(), simple_args.size() + 1);
    EXPECT_EQ(decoded_trailing[0], exe_trailing_slash);
    EXPECT_EQ(decoded_trailing[1], simple_args[0]);
    EXPECT_EQ(decoded_trailing[2], simple_args[1]);

    EXPECT_THROW(apps::build_windows_command_line("", simple_args), std::invalid_argument);
    EXPECT_THROW(apps::build_windows_command_line("C:\\bad\"path.exe", simple_args),
                 std::invalid_argument);
}
