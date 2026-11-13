// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: UTF-8 project root discovery across
// repository checkouts and staged layouts (including Japanese directory names),
// UTF-8 CLI option parsing, process_utf8_args(), and Windows CommandLineToArgvW
// quoting round-trips.
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

constexpr int kExitHelp = 0;
constexpr int kExitError = 1;
constexpr int kNestedEmptyLevels = 8;
constexpr std::string_view kTempDirName = "vn_ut_paths_ext04";

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

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

fs::path make_isolated_root(const fs::path& base, std::string_view leaf_utf8) {
    fs::path cur = base;
    for (int i = 0; i < kNestedEmptyLevels; ++i) {
        cur /= fs::u8path("pad_" + std::to_string(i));
    }
    cur /= fs::u8path(leaf_utf8);
    fs::create_directories(cur);
    return cur;
}

void write_dummy_file(const fs::path& file_path) {
    fs::create_directories(file_path.parent_path());
    std::ofstream out(file_path, std::ios::binary);
    out << "# placeholder\n";
}

}  // namespace

TEST(Paths, ReleaseZipLayoutResolvesRootWithoutScriptOutput) {
    const ScopedTempDir tmp(kTempDirName);
    const fs::path root = make_isolated_root(tmp.path(), "release_zip");
    fs::create_directories(root / "bin");
    fs::create_directories(root / "game");
    ASSERT_FALSE(fs::exists(root / "script_output"));

    EXPECT_EQ(apps::find_project_dir_from(root / "bin"), root.u8string());
}

TEST(Paths, CheckoutPriorityBeatsNestedBuildInstallStaging) {
    const ScopedTempDir tmp(kTempDirName);
    const fs::path checkout = make_isolated_root(tmp.path(), "checkout");
    fs::create_directories(checkout / "pipeline_cpp");
    write_dummy_file(checkout / "build.py");
    fs::create_directories(checkout / "build" / "install" / "bin");
    fs::create_directories(checkout / "build" / "install" / "game");

    EXPECT_EQ(apps::find_project_dir_from(checkout / "build" / "install" / "bin"),
              checkout.u8string());
}

TEST(Paths, LegacyStagedLayoutWithScriptOutputResolvesRoot) {
    const ScopedTempDir tmp(kTempDirName);
    const fs::path root = make_isolated_root(tmp.path(), "legacy_staged");
    fs::create_directories(root / "bin");
    fs::create_directories(root / "script_output");

    EXPECT_EQ(apps::find_project_dir_from(root / "bin"), root.u8string());
}

TEST(Paths, JapaneseDirectoryCheckoutAndStagedLayoutsReturnUtf8Root) {
    const ScopedTempDir tmp(kTempDirName);

    const fs::path jp_checkout = make_isolated_root(tmp.path(), "蟲愛ずる少女_CE");
    fs::create_directories(jp_checkout / "pipeline_cpp");
    write_dummy_file(jp_checkout / "build.py");
    fs::create_directories(jp_checkout / "build" / "install" / "bin");
    EXPECT_EQ(apps::find_project_dir_from(jp_checkout / "build" / "install" / "bin"),
              jp_checkout.u8string());

    const fs::path jp_staged = make_isolated_root(tmp.path(), "EXTRAVAGANZA～蟲愛編～");
    fs::create_directories(jp_staged / "bin");
    fs::create_directories(jp_staged / "game");
    EXPECT_EQ(apps::find_project_dir_from(jp_staged / "bin"),
              jp_staged.u8string());
}

TEST(Paths, UnrelatedDeepDirectoryReturnsEmpty) {
    const ScopedTempDir tmp(kTempDirName);
    fs::path deep = make_isolated_root(tmp.path(), "unrelated");
    for (int i = 0; i < kNestedEmptyLevels; ++i) {
        deep /= fs::u8path("empty_" + std::to_string(i));
    }
    fs::create_directories(deep);

    EXPECT_TRUE(apps::find_project_dir_from(deep).empty());
    EXPECT_TRUE(apps::find_project_dir_from(fs::path{}).empty());
}

TEST(Paths, ParseCommandLineHandlesCommonGameOnlyAndJapaneseUtf8Options) {
    {
        std::string project;
        std::string game;
        apps::po::options_description desc("test_common");
        desc.add(apps::common_options(project, game));

        {
            apps::po::variables_map vm;
            const char* argv[] = {"test_common", "--help"};
            const auto rc = apps::parse_command_line(
                static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
            ASSERT_TRUE(rc.has_value());
            EXPECT_EQ(*rc, kExitHelp);
        }
        {
            apps::po::variables_map vm;
            const char* argv[] = {"test_common", "--dir", "D:\\Project"};
            const auto rc = apps::parse_command_line(
                static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
            ASSERT_TRUE(rc.has_value());
            EXPECT_EQ(*rc, kExitError);
        }
        {
            apps::po::variables_map vm;
            const std::vector<std::string> utf8_args = {
                "--dir", "D:\\プロジェクト\\アゲハ_CE",
                "--game-dir", "E:\\ゲーム\\EXTRAVAGANZA～蟲愛編～"
            };
            const auto rc = apps::parse_command_line_args(utf8_args, desc, vm);
            EXPECT_FALSE(rc.has_value());
            EXPECT_EQ(project, "D:\\プロジェクト\\アゲハ_CE");
            EXPECT_EQ(game, "E:\\ゲーム\\EXTRAVAGANZA～蟲愛編～");
        }
    }

    {
        std::string game;
        apps::po::options_description desc("test_game_only");
        desc.add(apps::game_only_options(game));

        {
            apps::po::variables_map vm;
            const std::vector<std::string> utf8_args = {
                "--game-dir", "E:\\ゲーム\\蟲愛ずる少女_CE"
            };
            const auto rc = apps::parse_command_line_args(utf8_args, desc, vm);
            EXPECT_FALSE(rc.has_value());
            EXPECT_EQ(game, "E:\\ゲーム\\蟲愛ずる少女_CE");
        }
        {
            apps::po::variables_map vm;
            const char* argv[] = {"test_game_only", "--dir", "D:\\Project",
                                  "--game-dir", "E:\\Game"};
            const auto rc = apps::parse_command_line(
                static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
            ASSERT_TRUE(rc.has_value());
            EXPECT_EQ(*rc, kExitError);
        }
    }
}

TEST(Paths, ProcessUtf8ArgsDecodesWideProcessCommandLine) {
    const std::vector<std::string> args = apps::process_utf8_args();
    for (const std::string& arg : args) {
        EXPECT_NE(arg.find("ut_paths.exe"), 0u);
    }
}

TEST(Paths, BuildWindowsCommandLineRoundTripsThroughCommandLineToArgvW) {
    const std::string exe_normal = "C:\\Tools\\アゲハ_CE\\01_extract.exe";
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
    const std::vector<std::string> simple_args = {"--game-dir", "D:\\アゲハ_CE\\"};
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
