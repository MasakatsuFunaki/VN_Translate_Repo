// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: UTF-8 project root discovery across
// repository checkouts and staged layouts, UTF-8 CLI option parsing, and
// Windows wide command-line quoting round-trips.
#include "apps/paths.h"

#include "scoped_temp_dir.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace apps = mgi::apps;
using mgi::test_util::ScopedTempDir;

namespace {

constexpr int kExitHelp = 0;
constexpr int kExitError = 1;
constexpr int kNestedEmptyLevels = 8;

fs::path u8p(std::string_view utf8) {
    return fs::u8path(utf8);
}

fs::path make_isolated_root(const fs::path& base, std::string_view leaf_name) {
    fs::path cur = base;
    for (int i = 0; i < kNestedEmptyLevels; ++i) {
        cur /= u8p("pad_" + std::to_string(i));
    }
    cur /= u8p(leaf_name);
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
    const ScopedTempDir tmp("vn_ut_paths_release_zip");
    const fs::path root = make_isolated_root(tmp.path(), "release_zip");
    fs::create_directories(root / u8p("bin"));
    fs::create_directories(root / u8p("game"));
    ASSERT_FALSE(fs::exists(root / u8p("script_output")));

    EXPECT_EQ(apps::find_project_dir_from(root / u8p("bin")), root.u8string());
}

TEST(Paths, CheckoutPriorityBeatsNestedBuildInstallStaging) {
    const ScopedTempDir tmp("vn_ut_paths_checkout_priority");
    const fs::path checkout = make_isolated_root(tmp.path(), "checkout");
    fs::create_directories(checkout / u8p("pipeline_cpp"));
    write_dummy_file(checkout / u8p("build.py"));
    fs::create_directories(checkout / u8p("build") / u8p("install") / u8p("bin"));
    fs::create_directories(checkout / u8p("build") / u8p("install") / u8p("game"));

    EXPECT_EQ(apps::find_project_dir_from(
                  checkout / u8p("build") / u8p("install") / u8p("bin")),
              checkout.u8string());
}

TEST(Paths, LegacyStagedLayoutWithScriptOutputResolvesRoot) {
    const ScopedTempDir tmp("vn_ut_paths_legacy_staged");
    const fs::path root = make_isolated_root(tmp.path(), "legacy_staged");
    fs::create_directories(root / u8p("bin"));
    fs::create_directories(root / u8p("script_output"));

    EXPECT_EQ(apps::find_project_dir_from(root / u8p("bin")), root.u8string());
}

TEST(Paths, JapaneseDirectoryRootDiscoveryReturnsExactUtf8) {
    const ScopedTempDir tmp("vn_ut_paths_jp_utf8");
    const fs::path parent = make_isolated_root(tmp.path(), "EXTRAVAGANZA～蟲狂編～");
    const fs::path checkout = parent / u8p("蟲狂い_HD10");
    fs::create_directories(checkout / u8p("pipeline_cpp"));
    write_dummy_file(checkout / u8p("build.py"));
    fs::create_directories(checkout / u8p("build") / u8p("install") / u8p("bin"));

    const std::string resolved_checkout = apps::find_project_dir_from(
        checkout / u8p("build") / u8p("install") / u8p("bin"));
    EXPECT_EQ(resolved_checkout, checkout.u8string());
    EXPECT_NE(resolved_checkout.find("EXTRAVAGANZA～蟲狂編～"), std::string::npos);
    EXPECT_NE(resolved_checkout.find("蟲狂い_HD10"), std::string::npos);

    const fs::path staged = parent / u8p("EXTRAVAGANZA～蟲狂編～_CE版");
    fs::create_directories(staged / u8p("bin"));
    fs::create_directories(staged / u8p("game"));
    EXPECT_EQ(apps::find_project_dir_from(staged / u8p("bin")), staged.u8string());
}

TEST(Paths, UnrelatedDeepDirectoryReturnsEmpty) {
    const ScopedTempDir tmp("vn_ut_paths_unrelated_deep");
    fs::path deep = make_isolated_root(tmp.path(), "unrelated");
    for (int i = 0; i < kNestedEmptyLevels; ++i) {
        deep /= u8p("empty_" + std::to_string(i));
    }
    fs::create_directories(deep);

    EXPECT_TRUE(apps::find_project_dir_from(deep).empty());
    EXPECT_TRUE(apps::find_project_dir_from(fs::path{}).empty());
}

TEST(Paths, ParseCommandLineHandlesCommonGameOnlyAndProjectOnlyOptions) {
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
            const char* argv[] = {"test_common", "--dir", "D:\\Project",
                                  "--game-dir", "E:\\Game"};
            const auto rc = apps::parse_command_line(
                static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
            EXPECT_FALSE(rc.has_value());
            EXPECT_EQ(project, "D:\\Project");
            EXPECT_EQ(game, "E:\\Game");
        }
    }

    {
        std::string game;
        apps::po::options_description desc("test_game_only");
        desc.add(apps::game_only_options(game));

        {
            apps::po::variables_map vm;
            const char* argv[] = {"test_game_only", "--game-dir", "E:\\Game"};
            const auto rc = apps::parse_command_line(
                static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
            EXPECT_FALSE(rc.has_value());
            EXPECT_EQ(game, "E:\\Game");
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

    {
        std::string project;
        apps::po::options_description desc("test_project_only");
        desc.add(apps::project_only_options(project));

        {
            apps::po::variables_map vm;
            const char* argv[] = {"test_project_only", "--dir", "D:\\Project"};
            const auto rc = apps::parse_command_line(
                static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
            EXPECT_FALSE(rc.has_value());
            EXPECT_EQ(project, "D:\\Project");
        }
        {
            apps::po::variables_map vm;
            const char* argv[] = {"test_project_only", "--dir", "D:\\Project",
                                  "--game-dir", "E:\\Game"};
            const auto rc = apps::parse_command_line(
                static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
            ASSERT_TRUE(rc.has_value());
            EXPECT_EQ(*rc, kExitError);
        }
    }
}

TEST(Paths, ParseCommandLineArgsPreservesJapaneseUtf8Paths) {
    std::string project;
    std::string game;
    apps::po::options_description desc("test_utf8");
    desc.add(apps::common_options(project, game));

    const std::string expected_project = "E:\\VN\\EXTRAVAGANZA～蟲狂編～\\project";
    const std::string expected_game = "E:\\Games\\EXTRAVAGANZA～蟲狂編～\\蟲狂い_HD10";

    apps::po::variables_map vm;
    const std::vector<std::string> args = {
        "--dir", expected_project, "--game-dir", expected_game};
    const auto rc = apps::parse_command_line_args(args, desc, vm);
    EXPECT_FALSE(rc.has_value());
    EXPECT_EQ(project, expected_project);
    EXPECT_EQ(game, expected_game);
}

TEST(Paths, ProcessUtf8ArgsDecodesLiveCommandLineWithoutThrowing) {
    EXPECT_NO_THROW({
        const std::vector<std::string> live_args = apps::process_utf8_args();
        (void)live_args;
    });
}

TEST(Paths, WindowsCommandLineQuotingRoundTripsThroughCommandLineToArgvW) {
    {
        const std::string exe_path =
            "E:\\Tools\\EXTRAVAGANZA～蟲狂編～\\bin\\01_extract.exe";
        const std::vector<std::string> args = {
            "--dir",
            "E:\\Projects\\蟲狂い_HD10\\",
            "--game-dir",
            "E:\\Games\\EXTRAVAGANZA～蟲狂編～\\蟲狂い_HD10\\",
            "quote\"inside",
            "slash_before_quote\\\"tail",
            "double_slash_quote\\\\\"tail",
            ""};

        const std::wstring cmd = apps::build_windows_command_line(exe_path, args);
        const std::vector<std::string> decoded =
            apps::split_windows_command_line_utf8(cmd);
        ASSERT_EQ(decoded.size(), args.size() + 1);
        EXPECT_EQ(decoded[0], exe_path);
        for (std::size_t i = 0; i < args.size(); ++i) {
            EXPECT_EQ(decoded[i + 1], args[i]) << "Mismatch at arg index " << i;
        }
    }

    {
        const std::string trailing_slash_exe = "E:\\Tools\\蟲狂い_HD10\\";
        const std::vector<std::string> args = {"--game-dir", "E:\\Games\\蟲狂い_HD10\\"};
        const std::wstring cmd =
            apps::build_windows_command_line(trailing_slash_exe, args);
        const std::vector<std::string> decoded =
            apps::split_windows_command_line_utf8(cmd);
        ASSERT_EQ(decoded.size(), args.size() + 1);
        EXPECT_EQ(decoded[0], trailing_slash_exe);
        EXPECT_EQ(decoded[1], args[0]);
        EXPECT_EQ(decoded[2], args[1]);
    }

    EXPECT_THROW(apps::build_windows_command_line("", {"--help"}), std::invalid_argument);
    EXPECT_THROW(apps::build_windows_command_line("E:\\bad\"exe.exe", {"--help"}),
                 std::invalid_argument);
}
