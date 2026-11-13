// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: two-pass project root discovery,
// UTF-8 Japanese path preservation, CommandLineToArgvW round-trips, and child
// process execution.
#include "apps/paths.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace apps = fav::apps;

constexpr int kNestedEmptyLevels = 8;
constexpr int kChildSuccessExitCode = 0;
constexpr int kChildFailureExitCode = 7;
constexpr std::size_t kLongOptionPrefixLen = 2;
constexpr std::size_t kFirstArgOptionIndex = 1;

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* name)
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

fs::path MakeIsolatedRoot(const fs::path& base, const char* leaf_utf8) {
    fs::path dir = base;
    for (int level = 0; level < kNestedEmptyLevels; ++level) {
        dir /= "p";
    }
    dir /= fs::u8path(leaf_utf8);
    return dir;
}

void WriteDummyFile(const fs::path& file_path) {
    fs::create_directories(file_path.parent_path());
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    out << "# build script\n";
}

std::optional<int> RunParse(std::initializer_list<const char*> args,
                            const apps::po::options_description& desc,
                            apps::po::variables_map& vm) {
    std::vector<std::string> storage(args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (std::string& s : storage) {
        argv.push_back(s.data());
    }
    return apps::parse_command_line(
        static_cast<int>(argv.size()), argv.data(), desc, vm);
}

void RegisterActiveTestRunnerFlags(const std::vector<std::string>& proc_args,
                                   apps::po::options_description& desc) {
    std::unordered_set<std::string> registered;
    for (std::size_t i = kFirstArgOptionIndex; i < proc_args.size(); ++i) {
        const std::string& token = proc_args[i];
        if (token.rfind("--", 0) != 0) {
            continue;
        }
        const std::size_t eq = token.find('=');
        const std::string name =
            eq == std::string::npos
                ? token.substr(kLongOptionPrefixLen)
                : token.substr(kLongOptionPrefixLen, eq - kLongOptionPrefixLen);
        if (name.empty() || !registered.insert(name).second) {
            continue;
        }
        if (eq == std::string::npos) {
            desc.add_options()(name.c_str(), apps::po::bool_switch());
            continue;
        }
        desc.add_options()(name.c_str(), apps::po::value<std::string>());
    }
}

}  // namespace

TEST(Paths, ReleaseZipLayoutResolvesRootWithoutScriptOutput) {
    ScopedTempDir temp("vn_ut_paths_sakuramoyu_zip");
    const fs::path root = MakeIsolatedRoot(temp.path(), "release_root");
    fs::create_directories(root / "bin");
    fs::create_directories(root / "game");
    ASSERT_FALSE(fs::exists(root / "script_output"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, CheckoutPriorityBeatsNestedBuildInstallStaging) {
    ScopedTempDir temp("vn_ut_paths_sakuramoyu_checkout");
    const fs::path checkout = MakeIsolatedRoot(temp.path(), "checkout");
    fs::create_directories(checkout / "pipeline_cpp");
    WriteDummyFile(checkout / "build.py");

    const fs::path staged_bin = checkout / "build" / "install" / "bin";
    const fs::path staged_game = checkout / "build" / "install" / "game";
    fs::create_directories(staged_bin);
    fs::create_directories(staged_game);

    const std::string found = apps::find_project_dir_from(staged_bin);
    EXPECT_EQ(fs::u8path(found), checkout);
}

TEST(Paths, LegacyStagedLayoutWithScriptOutputResolvesRoot) {
    ScopedTempDir temp("vn_ut_paths_sakuramoyu_legacy");
    const fs::path root = MakeIsolatedRoot(temp.path(), "legacy_staged");
    fs::create_directories(root / "bin");
    fs::create_directories(root / "script_output");
    ASSERT_FALSE(fs::exists(root / "game"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, UnrelatedDeepDirectoryReturnsEmpty) {
    ScopedTempDir temp("vn_ut_paths_sakuramoyu_empty");
    fs::path deep = temp.path();
    for (int level = 0; level < kNestedEmptyLevels; ++level) {
        deep /= "empty";
    }
    fs::create_directories(deep);

    EXPECT_TRUE(apps::find_project_dir_from(deep).empty());
    EXPECT_TRUE(apps::find_project_dir_from(fs::path{}).empty());
}

TEST(Paths, JapaneseDirectoryResolvesUtf8RootForCheckoutAndReleaseZip) {
    ScopedTempDir temp("vn_ut_paths_sakuramoyu_unicode");
    const fs::path jp_parent =
        MakeIsolatedRoot(temp.path(), "さくら、もゆ。");

    const fs::path checkout = jp_parent / "checkout";
    fs::create_directories(checkout / "pipeline_cpp");
    WriteDummyFile(checkout / "build.py");
    const fs::path staged_bin = checkout / "build" / "install" / "bin";
    fs::create_directories(staged_bin);

    const std::string found_checkout = apps::find_project_dir_from(staged_bin);
    ASSERT_FALSE(found_checkout.empty());
    EXPECT_NE(found_checkout.find("さくら、もゆ。"), std::string::npos);
    EXPECT_EQ(fs::u8path(found_checkout), checkout);
    EXPECT_TRUE(fs::exists(fs::u8path(found_checkout)));

    const fs::path release_root =
        jp_parent / fs::u8path("FAVORITE") /
        fs::u8path("さくら、もゆ。-as the Night's, Reincarnation-");
    fs::create_directories(release_root / "bin");
    fs::create_directories(release_root / "game");

    const std::string found_release =
        apps::find_project_dir_from(release_root / "bin");
    ASSERT_FALSE(found_release.empty());
    EXPECT_NE(found_release.find("さくら、もゆ。-as the Night's, Reincarnation-"),
              std::string::npos);
    EXPECT_EQ(fs::u8path(found_release), release_root);
    EXPECT_TRUE(fs::exists(fs::u8path(found_release)));
}

TEST(Paths, ParseCommandLineHandlesHelpValidationAndOverrides) {
    std::string project;
    std::string game;
    apps::po::options_description desc("test_cli");
    desc.add(apps::common_options(project, game));

    {
        apps::po::variables_map vm;
        const auto rc = RunParse({"test_cli", "--help"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitHelp);
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunParse({"test_cli", "--dir", "E:\\Project"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunParse(
            {"test_cli", "--dir", "E:\\Project", "--game-dir", "E:\\Game"}, desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, "E:\\Project");
        EXPECT_EQ(game, "E:\\Game");
    }
}

TEST(Paths, GameOnlyOptionsAcceptsGameDirAndRejectsProjectDir) {
    std::string game;
    apps::po::options_description desc("test_game_only");
    desc.add(apps::game_only_options(game));

    {
        apps::po::variables_map vm;
        const auto rc = RunParse({"test_game_only", "--help"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitHelp);
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunParse({"test_game_only", "--game-dir", "E:\\Game"}, desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(game, "E:\\Game");
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunParse(
            {"test_game_only", "--dir", "E:\\Project", "--game-dir", "E:\\Game"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }
}

TEST(Paths, ParseUtf8ArgsPreservesJapaneseGameAndProjectDirs) {
    const std::string jp_game =
        "C:\\FAVORITE\\さくら、もゆ。-as the Night's, Reincarnation-";
    const std::string jp_project = "E:\\Translations\\さくら、もゆ。";

    {
        std::string game;
        apps::po::options_description desc("03_patch_ui");
        desc.add(apps::game_only_options(game));

        apps::po::variables_map vm;
        const auto rc = apps::parse_utf8_args({"--game-dir", jp_game}, desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(game, jp_game);
        EXPECT_EQ(fs::u8path(game).u8string(), jp_game);
    }

    {
        std::string project;
        std::string game;
        apps::po::options_description desc("01_extract");
        desc.add(apps::common_options(project, game));

        apps::po::variables_map vm;
        const auto rc = apps::parse_utf8_args(
            {"--dir", jp_project, "--game-dir", jp_game}, desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, jp_project);
        EXPECT_EQ(game, jp_game);
        EXPECT_EQ(fs::u8path(project).u8string(), jp_project);
        EXPECT_EQ(fs::u8path(game).u8string(), jp_game);
    }

    {
        std::string game;
        apps::po::options_description desc("03_patch_ui");
        desc.add(apps::game_only_options(game));

        apps::po::variables_map vm;
        const auto rc =
            RunParse({"03_patch_ui", "--game-dir", jp_game.c_str()}, desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(game, jp_game);
        EXPECT_EQ(fs::u8path(game).u8string(), jp_game);
    }
}

TEST(Paths, ParseUtf8ArgsRejectsLeadingArgv0AndUnrecognizedPositionalTokens) {
    const std::string jp_game =
        "C:\\FAVORITE\\さくら、もゆ。-as the Night's, Reincarnation-";

    {
        std::string game;
        apps::po::options_description desc("03_patch_ui");
        desc.add(apps::game_only_options(game));

        apps::po::variables_map vm;
        const auto rc =
            apps::parse_utf8_args({"03_patch_ui", "--game-dir", jp_game}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }

    {
        std::string game;
        apps::po::options_description desc("03_patch_ui");
        desc.add(apps::game_only_options(game));

        apps::po::variables_map vm;
        const auto rc = apps::parse_utf8_args(
            {"--game-dir", jp_game, "unexpected_positional_token"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }
}

TEST(Paths, ProcessUtf8ArgsReturnsValidExecutablePathInArgv0) {
    const std::vector<std::string> args = apps::process_utf8_args();
    ASSERT_FALSE(args.empty());
    EXPECT_NE(args.front().find("ut_paths"), std::string::npos) << args.front();
    EXPECT_TRUE(fs::exists(fs::u8path(args.front()))) << args.front();
}

TEST(Paths, ParseProcessCommandLineResolvesProjectDirAndHandlesRunnerFlags) {
    const std::vector<std::string> proc_args = apps::process_utf8_args();
    ASSERT_FALSE(proc_args.empty());

    std::string project_dir;
    const std::string expected_default = apps::default_project_dir();
    ASSERT_FALSE(expected_default.empty());

    apps::po::options_description desc("ut_paths_proc_cli");
    desc.add_options()
        ("help,h", "show help")
        ("dir", apps::po::value(&project_dir)->default_value(expected_default),
         "project folder");
    RegisterActiveTestRunnerFlags(proc_args, desc);

    apps::po::variables_map vm;
    const auto rc = apps::parse_process_command_line(desc, vm);
    EXPECT_FALSE(rc.has_value());
    EXPECT_EQ(project_dir, expected_default);

    if (proc_args.size() > kFirstArgOptionIndex) {
        std::string strict_project;
        apps::po::options_description strict_desc("strict_cli");
        strict_desc.add_options()
            ("dir", apps::po::value(&strict_project)->default_value(expected_default),
             "project folder");
        apps::po::variables_map strict_vm;
        const auto strict_rc = apps::parse_process_command_line(strict_desc, strict_vm);
        ASSERT_TRUE(strict_rc.has_value());
        EXPECT_EQ(*strict_rc, apps::kExitError);
    }
}

TEST(Paths, RunChildProcessReturnsExitCodesAndThrowsOnMissingExecutable) {
    EXPECT_EQ(apps::run_child_process("cmd.exe", {"/c", "exit", "0"}),
              kChildSuccessExitCode);
    EXPECT_EQ(apps::run_child_process("cmd.exe", {"/c", "exit", "7"}),
              kChildFailureExitCode);

    const fs::path missing_abs =
        fs::temp_directory_path() / "vn_ut_paths_missing_dir" / "missing_tool.exe";
    EXPECT_THROW(apps::run_child_process(missing_abs, {}), std::runtime_error);
    EXPECT_THROW(apps::run_child_process("vn_missing_sakuramoyu_tool_99999.exe", {}),
                 std::runtime_error);
}

TEST(Paths, QuoteWindowsArgEscapesSpacesQuotesAndTrailingBackslashes) {
    EXPECT_EQ(apps::quote_windows_arg("--game-dir"), "--game-dir");
    EXPECT_EQ(apps::quote_windows_arg(""), "\"\"");
    EXPECT_EQ(
        apps::quote_windows_arg(
            "C:\\FAVORITE\\さくら、もゆ。-as the Night's, Reincarnation-"),
        "\"C:\\FAVORITE\\さくら、もゆ。-as the Night's, Reincarnation-\"");
    EXPECT_EQ(apps::quote_windows_arg("C:\\FAVORITE\\さくら、もゆ。\\"),
              "C:\\FAVORITE\\さくら、もゆ。\\");
    EXPECT_EQ(apps::quote_windows_arg("C:\\FAVORITE\\さくら もゆ\\"),
              "\"C:\\FAVORITE\\さくら もゆ\\\\\"");
    EXPECT_EQ(apps::quote_windows_arg("a\\\"b"), "\"a\\\\\\\"b\"");
}

TEST(Paths, FormatAndSplitWindowsCommandLineRoundTripsEdgeCases) {
    EXPECT_TRUE(apps::split_windows_command_line("").empty());

    {
        const std::string exe = "C:\\dir\\";
        const std::vector<std::string> args = {
            "--game-dir",
            "C:\\FAVORITE\\さくら、もゆ。-as the Night's, Reincarnation-\\",
            "say \"さくら、もゆ。\" now",
            "",
            "trailing\\\\",
        };
        const std::string cmdline = apps::format_windows_command_line(exe, args);
        const std::vector<std::string> round_trip =
            apps::split_windows_command_line(cmdline);

        std::vector<std::string> expected = {exe};
        expected.insert(expected.end(), args.begin(), args.end());
        EXPECT_EQ(round_trip, expected);
    }

    {
        const std::string jp_exe = "C:\\FAVORITE\\さくら、もゆ。\\";
        const std::vector<std::string> args = {
            "--dir",
            "E:\\Projects\\さくら、もゆ。\\",
            "embedded \"quote\" and \\\"slash-quote\\\"",
        };
        const std::string cmdline = apps::format_windows_command_line(jp_exe, args);
        const std::vector<std::string> round_trip =
            apps::split_windows_command_line(cmdline);

        std::vector<std::string> expected = {jp_exe};
        expected.insert(expected.end(), args.begin(), args.end());
        EXPECT_EQ(round_trip, expected);
    }

    EXPECT_THROW(apps::format_windows_command_line("C:\\bad\"quote\\tool.exe", {}),
                 std::runtime_error);
    EXPECT_THROW(apps::format_windows_command_line("", {}), std::runtime_error);
}
