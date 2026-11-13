// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: two-pass UTF-8 project-root discovery
// across checkout and staged release layouts (including Japanese paths), plus
// wide UTF-16 -> UTF-8 CLI option parsing.
#include "apps/paths.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace apps = ksh::apps;

namespace {

constexpr int kNestedEmptyLevels = 8;
constexpr std::uint16_t kLoneHighSurrogate = 0xD800u;

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* name)
        : path_(fs::temp_directory_path() / name) {
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
    const ScopedTempDir temp("vn_ut_paths_kishin_hisho");
    const fs::path root = CreateNestedEmptyPad(temp.path()) / "release_root";
    fs::create_directories(root / "bin");
    fs::create_directories(root / "game");
    ASSERT_FALSE(fs::exists(root / "script_output"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(found, root.u8string());
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, CheckoutPriorityBeatsNestedBuildInstallStaging) {
    const ScopedTempDir temp("vn_ut_paths_kishin_hisho");
    const fs::path checkout = CreateNestedEmptyPad(temp.path()) / "checkout_root";
    fs::create_directories(checkout / "pipeline_cpp");
    WriteDummyBuildPy(checkout);

    const fs::path staged_bin = checkout / "build" / "install" / "bin";
    const fs::path staged_game = checkout / "build" / "install" / "game";
    fs::create_directories(staged_bin);
    fs::create_directories(staged_game);

    const std::string found = apps::find_project_dir_from(staged_bin);
    EXPECT_EQ(found, checkout.u8string());
    EXPECT_EQ(fs::u8path(found), checkout);
}

TEST(Paths, LegacyStagedLayoutWithScriptOutputResolvesRoot) {
    const ScopedTempDir temp("vn_ut_paths_kishin_hisho");
    const fs::path root = CreateNestedEmptyPad(temp.path()) / "legacy_root";
    fs::create_directories(root / "bin");
    fs::create_directories(root / "script_output");
    ASSERT_FALSE(fs::exists(root / "game"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(found, root.u8string());
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, JapaneseCheckoutAndReleaseLayoutsResolveUtf8Root) {
    {
        const ScopedTempDir temp("vn_ut_paths_kishin_hisho_jp_checkout");
        const fs::path checkout =
            CreateNestedEmptyPad(temp.path()) / fs::u8path("機神飛翔_チェックアウト");
        fs::create_directories(checkout / "pipeline_cpp");
        WriteDummyBuildPy(checkout);

        const fs::path staged_bin = checkout / "build" / "install" / "bin";
        const fs::path staged_game = checkout / "build" / "install" / "game";
        fs::create_directories(staged_bin);
        fs::create_directories(staged_game);

        const std::string found = apps::find_project_dir_from(staged_bin);
        EXPECT_EQ(found, checkout.u8string());
        EXPECT_EQ(fs::u8path(found), checkout);
        EXPECT_NE(found.find("機神飛翔_チェックアウト"), std::string::npos);
    }

    {
        const ScopedTempDir temp("vn_ut_paths_kishin_hisho_jp_release");
        const fs::path release_root =
            CreateNestedEmptyPad(temp.path()) / fs::u8path("機神飛翔_リリース");
        fs::create_directories(release_root / "bin");
        fs::create_directories(release_root / "game");

        const std::string found = apps::find_project_dir_from(release_root / "bin");
        EXPECT_EQ(found, release_root.u8string());
        EXPECT_EQ(fs::u8path(found), release_root);
        EXPECT_NE(found.find("機神飛翔_リリース"), std::string::npos);
    }
}

TEST(Paths, UnrelatedDeepDirectoryReturnsEmpty) {
    const ScopedTempDir temp("vn_ut_paths_kishin_hisho");
    const fs::path deep = CreateNestedEmptyPad(temp.path());

    EXPECT_TRUE(apps::find_project_dir_from(deep).empty());
    EXPECT_TRUE(apps::find_project_dir_from(fs::path{}).empty());
}

TEST(Paths, ParseCommandLineHandlesHelpAndOptionsAndUtf8Directories) {
    std::string project;
    std::string game;
    apps::po::options_description desc("test_cli");
    desc.add(apps::common_options(project));
    desc.add_options()
        ("game-dir", apps::po::value(&game)->required(), "game installation folder");

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
        const char* argv[] = {"test_cli", "--dir", "X:\\Synthetic\\Project"};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }

    {
        apps::po::variables_map vm;
        const char* argv[] = {"test_cli", "--dir", "X:\\Synthetic\\Project",
                              "--game-dir", "X:\\Synthetic\\Game", "stray_token"};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }

    {
        apps::po::variables_map vm;
        const char* argv[] = {
            "test_cli", "--dir", "X:\\機神飛翔_チェックアウト",
            "--game-dir", "X:\\機神飛翔_リリース\\デモンベイン"};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, "X:\\機神飛翔_チェックアウト");
        EXPECT_EQ(game, "X:\\機神飛翔_リリース\\デモンベイン");
    }
}

TEST(Paths, WideCommandLineAndProcessUtf8ArgsSkipProgramNameAndEncodeJapanese) {
    EXPECT_TRUE(apps::command_line_to_utf8_args(nullptr).empty());
    EXPECT_TRUE(apps::command_line_to_utf8_args(L"").empty());
    EXPECT_TRUE(apps::command_line_to_utf8_args(L"\"ut_paths.exe\"").empty());

    const std::vector<std::string> parsed = apps::command_line_to_utf8_args(
        L"\"ut_paths.exe\" --dir \"X:\\機神飛翔_チェックアウト\" "
        L"--game-dir \"X:\\機神飛翔_リリース\"");
    const std::vector<std::string> expected{
        "--dir", "X:\\機神飛翔_チェックアウト",
        "--game-dir", "X:\\機神飛翔_リリース",
    };
    EXPECT_EQ(parsed, expected);

    const wchar_t invalid_surrogate_cmd[] = {
        L'a', L' ', static_cast<wchar_t>(kLoneHighSurrogate), L'\0'};
    EXPECT_THROW(apps::command_line_to_utf8_args(invalid_surrogate_cmd), std::runtime_error);

    const std::vector<std::string> live_args = apps::process_utf8_args();
    for (const auto& token : live_args) {
        EXPECT_EQ(token.find("ut_paths.exe"), std::string::npos);
    }
}
