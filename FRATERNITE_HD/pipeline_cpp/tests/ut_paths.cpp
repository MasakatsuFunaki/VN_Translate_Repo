// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: two-pass UTF-8 project-root discovery
// across checkout and staged release layouts, CLI option parsing, and
// CreateProcessW / CommandLineToArgvW UTF-16 quoting round-trips.
#include "apps/paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace apps = frat::apps;

namespace {

constexpr int kNestedEmptyLevels = 8;

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

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

class ScopedLocalArgv {
public:
    explicit ScopedLocalArgv(LPWSTR* argv) noexcept : argv_(argv) {}
    ScopedLocalArgv(const ScopedLocalArgv&) = delete;
    ScopedLocalArgv& operator=(const ScopedLocalArgv&) = delete;
    ~ScopedLocalArgv() {
        if (argv_ != nullptr) {
            LocalFree(argv_);
        }
    }

private:
    LPWSTR* argv_;
};

std::vector<std::wstring> SplitCommandLineW(const std::wstring& cmd_line) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmd_line.c_str(), &argc);
    const ScopedLocalArgv guard(argv);
    if (argv == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed in test");
    }
    std::vector<std::wstring> out;
    out.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        out.emplace_back(argv[i]);
    }
    return out;
}

// Pads `base` with `kNestedEmptyLevels` empty ancestor directories so the
// bounded ancestor walk cannot escape into any outer directory above `%TEMP%`.
fs::path CreateNestedEmptyPad(const fs::path& base) {
    fs::path cur = base;
    for (int level = 1; level <= kNestedEmptyLevels; ++level) {
        cur /= "pad_" + std::to_string(level);
    }
    fs::create_directories(cur);
    return cur;
}

void WriteDummyBuildPy(const fs::path& dir) {
    std::ofstream out(dir / "build.py", std::ios::binary);
    out << "# dummy build driver\n";
}

}  // namespace

TEST(Paths, ReleaseZipLayoutResolvesRootWithoutScriptOutput) {
    const ScopedTempDir temp("vn_ut_paths_fraternite");
    const fs::path root = CreateNestedEmptyPad(temp.path()) / "release_root";
    fs::create_directories(root / "bin");
    fs::create_directories(root / "game");
    ASSERT_FALSE(fs::exists(root / "script_output"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(found, root.u8string());
}

TEST(Paths, CheckoutPriorityBeatsNestedBuildInstallStaging) {
    const ScopedTempDir temp("vn_ut_paths_fraternite");
    const fs::path checkout = CreateNestedEmptyPad(temp.path()) / "checkout_root";
    fs::create_directories(checkout / "pipeline_cpp");
    WriteDummyBuildPy(checkout);

    const fs::path staged_bin = checkout / "build" / "install" / "bin";
    const fs::path staged_game = checkout / "build" / "install" / "game";
    fs::create_directories(staged_bin);
    fs::create_directories(staged_game);

    const std::string found = apps::find_project_dir_from(staged_bin);
    EXPECT_EQ(found, checkout.u8string());
}

TEST(Paths, LegacyStagedLayoutWithScriptOutputResolvesRoot) {
    const ScopedTempDir temp("vn_ut_paths_fraternite");
    const fs::path root = CreateNestedEmptyPad(temp.path()) / "legacy_root";
    fs::create_directories(root / "bin");
    fs::create_directories(root / "script_output");
    ASSERT_FALSE(fs::exists(root / "game"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(found, root.u8string());
}

TEST(Paths, JapaneseDirectoryCheckoutAndReleaseLayoutsResolveUtf8Root) {
    const ScopedTempDir temp("vn_ut_paths_jp_フラテルニテ");

    const fs::path checkout =
        CreateNestedEmptyPad(temp.path()) / fs::u8path("日本語チェックアウト_フラテルニテ");
    fs::create_directories(checkout / "pipeline_cpp");
    WriteDummyBuildPy(checkout);
    const fs::path staged_bin = checkout / "build" / "install" / "bin";
    fs::create_directories(staged_bin);
    fs::create_directories(checkout / "build" / "install" / "game");
    EXPECT_EQ(apps::find_project_dir_from(staged_bin), checkout.u8string());

    const fs::path release =
        CreateNestedEmptyPad(temp.path()) / fs::u8path("日本語リリース_フラテルニテ");
    fs::create_directories(release / "bin");
    fs::create_directories(release / "game");
    EXPECT_EQ(apps::find_project_dir_from(release / "bin"), release.u8string());
}

TEST(Paths, UnrelatedDeepDirectoryReturnsEmpty) {
    const ScopedTempDir temp("vn_ut_paths_fraternite");
    const fs::path deep = CreateNestedEmptyPad(temp.path());

    EXPECT_TRUE(apps::find_project_dir_from(deep).empty());
    EXPECT_TRUE(apps::find_project_dir_from(fs::path{}).empty());
}

TEST(Paths, ParseCommandLineHandlesHelpOptionsAndJapaneseUtf8Paths) {
    std::string project;
    std::string game;
    apps::po::options_description desc("test_cli");
    desc.add(apps::common_options(project, game));

    {
        apps::po::variables_map vm;
        const char* argv[] = {"test_cli", "--help"};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitHelp);
    }

    {
        apps::po::variables_map vm;
        const char* argv[] = {"test_cli", "--dir", "D:\\Project"};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }

    {
        apps::po::variables_map vm;
        const char* argv[] = {
            "test_cli", "--dir", "D:\\Project", "--game-dir", "E:\\Game", "stray_token"};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }

    {
        apps::po::variables_map vm;
        const char* jp_project = "D:\\開発\\フラテルニテ_プロジェクト";
        const char* jp_game = "E:\\ゲーム\\フラテルニテ HDリマスター";
        const char* argv[] = {"test_cli", "--dir", jp_project, "--game-dir", jp_game};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, jp_project);
        EXPECT_EQ(game, jp_game);
    }
}

TEST(Paths, GameOnlyOptionsAcceptsGameDirAndRejectsDir) {
    std::string game;
    apps::po::options_description desc("test_game_only");
    desc.add(apps::game_only_options(game));

    {
        apps::po::variables_map vm;
        const char* argv[] = {"test_game_only", "--help"};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitHelp);
    }

    {
        apps::po::variables_map vm;
        const char* argv[] = {"test_game_only", "--dir", "D:\\Project", "--game-dir", "E:\\Game"};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }

    {
        apps::po::variables_map vm;
        const char* jp_game = "E:\\インストール先\\フラテルニテ";
        const char* argv[] = {"test_game_only", "--game-dir", jp_game};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(game, jp_game);
    }
}

TEST(Paths, ProcessUtf8ArgsRunsCleanlyOnCurrentProcess) {
    EXPECT_NO_THROW({
        const std::vector<std::string> args = apps::process_utf8_args();
        (void)args;
    });
}

TEST(Paths, BuildProcessCommandLineRoundTripsJapaneseArgsQuotesAndBackslashes) {
    const std::string exe_path =
        "D:\\ツール フォルダ\\フラテルニテ\\01_extract.exe";
    const std::vector<std::string> args = {
        "--dir",
        "D:\\開発 フォルダ\\フラテルニテ\\",
        "--game-dir",
        "E:\\ゲーム\\フラテルニテ HDリマスター\\\\",
        "embedded \"フラテルニテ\" quote",
        "odd_slash\\\"quote",
        "even_slash\\\\\"quote",
        "",
        "plain_arg",
    };

    const std::wstring cmd = apps::build_process_command_line(exe_path, args);
    const std::vector<std::wstring> decoded = SplitCommandLineW(cmd);

    const std::vector<std::wstring> expected = {
        L"D:\\ツール フォルダ\\フラテルニテ\\01_extract.exe",
        L"--dir",
        L"D:\\開発 フォルダ\\フラテルニテ\\",
        L"--game-dir",
        L"E:\\ゲーム\\フラテルニテ HDリマスター\\\\",
        L"embedded \"フラテルニテ\" quote",
        L"odd_slash\\\"quote",
        L"even_slash\\\\\"quote",
        L"",
        L"plain_arg",
    };
    EXPECT_EQ(decoded, expected);
}

TEST(Paths, BuildProcessCommandLinePreservesSingleTrailingBackslashInExePath) {
    const std::wstring cmd = apps::build_process_command_line(
        "C:\\ゲーム フォルダ\\", {"--game-dir", "E:\\フラテルニテ\\"});
    const std::vector<std::wstring> decoded = SplitCommandLineW(cmd);

    const std::vector<std::wstring> expected = {
        L"C:\\ゲーム フォルダ\\",
        L"--game-dir",
        L"E:\\フラテルニテ\\",
    };
    EXPECT_EQ(decoded, expected);
}

TEST(Paths, BuildProcessCommandLineRejectsEmptyOrQuotedExePath) {
    EXPECT_THROW(apps::build_process_command_line("", {"--help"}),
                 std::runtime_error);
    EXPECT_THROW(
        apps::build_process_command_line("C:\\bad\"path\\01_extract.exe", {"--help"}),
        std::runtime_error);
}
