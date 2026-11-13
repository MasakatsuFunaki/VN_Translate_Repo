// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "apps/paths.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace apps = oretsuba::apps;

constexpr int kNestedEmptyLevels = 8;

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

fs::path MakeIsolatedRoot(const fs::path& base, std::string_view utf8_leaf) {
    fs::path dir = base;
    for (int level = 0; level < kNestedEmptyLevels; ++level) {
        dir /= "p";
    }
    dir /= fs::u8path(utf8_leaf);
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
    return apps::ParseCommandLine(
        static_cast<int>(argv.size()), argv.data(), desc, vm);
}

}  // namespace

TEST(PathsTest, ReleaseZipLayoutResolvesRootWithoutScriptOutput) {
    const ScopedTempDir temp("vn_ut_paths_oretsuba");
    const fs::path root = MakeIsolatedRoot(temp.path(), "release_root");
    fs::create_directories(root / "bin");
    fs::create_directories(root / "game");
    ASSERT_FALSE(fs::exists(root / "script_output"));

    const std::string found = apps::FindProjectDirectoryFrom(root / "bin");
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(PathsTest, CheckoutPriorityBeatsNestedBuildInstallStaging) {
    const ScopedTempDir temp("vn_ut_paths_oretsuba");
    const fs::path checkout = MakeIsolatedRoot(temp.path(), "checkout");
    fs::create_directories(checkout / "pipeline_cpp");
    WriteDummyFile(checkout / "build.py");

    const fs::path staged_bin = checkout / "build" / "install" / "bin";
    const fs::path staged_game = checkout / "build" / "install" / "game";
    fs::create_directories(staged_bin);
    fs::create_directories(staged_game);

    const std::string found = apps::FindProjectDirectoryFrom(staged_bin);
    EXPECT_EQ(fs::u8path(found), checkout);
}

TEST(PathsTest, LegacyStagedLayoutWithScriptOutputResolvesRoot) {
    const ScopedTempDir temp("vn_ut_paths_oretsuba");
    const fs::path root = MakeIsolatedRoot(temp.path(), "legacy_staged");
    fs::create_directories(root / "bin");
    fs::create_directories(root / "script_output");
    ASSERT_FALSE(fs::exists(root / "game"));

    const std::string found = apps::FindProjectDirectoryFrom(root / "bin");
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(PathsTest, JapaneseDirectoryUtf8RootDiscoveryCheckoutAndReleaseLayouts) {
    const ScopedTempDir temp("vn_ut_paths_jp_俺つば");

    const fs::path jp_checkout =
        MakeIsolatedRoot(temp.path(), "俺たちに翼はない_チェックアウト");
    fs::create_directories(jp_checkout / "pipeline_cpp");
    WriteDummyFile(jp_checkout / "build.py");
    const fs::path staged_bin = jp_checkout / "build" / "install" / "bin";
    fs::create_directories(staged_bin);

    const std::string found_checkout = apps::FindProjectDirectoryFrom(staged_bin);
    EXPECT_EQ(found_checkout, jp_checkout.u8string());
    EXPECT_NE(found_checkout.find("俺たちに翼はない_チェックアウト"), std::string::npos);
    EXPECT_EQ(fs::u8path(found_checkout), jp_checkout);

    const fs::path jp_release =
        MakeIsolatedRoot(temp.path(), "俺たちに翼はない_リリース");
    fs::create_directories(jp_release / "bin");
    fs::create_directories(jp_release / "game");

    const std::string found_release = apps::FindProjectDirectoryFrom(jp_release / "bin");
    EXPECT_EQ(found_release, jp_release.u8string());
    EXPECT_NE(found_release.find("俺たちに翼はない_リリース"), std::string::npos);
    EXPECT_EQ(fs::u8path(found_release), jp_release);
}

TEST(PathsTest, UnrelatedDeepDirectoryReturnsEmpty) {
    const ScopedTempDir temp("vn_ut_paths_oretsuba");
    fs::path deep = temp.path();
    for (int level = 0; level < kNestedEmptyLevels; ++level) {
        deep /= "empty";
    }
    fs::create_directories(deep);

    EXPECT_TRUE(apps::FindProjectDirectoryFrom(deep).empty());
    EXPECT_TRUE(apps::FindProjectDirectoryFrom(fs::path{}).empty());
}

TEST(PathsTest, ParseCommandLineHandlesHelpValidationAndOverrides) {
    std::string project_dir;
    std::string game_dir;
    const apps::po::options_description desc =
        apps::BuildCommonOptions(project_dir, game_dir);

    {
        apps::po::variables_map vm;
        const auto rc = RunParse({"test_cli", "--help"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitSuccess);
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunParse({"test_cli", "unexpected_positional"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitCommandLineError);
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunParse({"test_cli", "--dir", "D:\\Project"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitCommandLineError);
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunParse(
            {"test_cli", "--dir", "D:\\Project", "--game-dir", "E:\\Game"}, desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project_dir, "D:\\Project");
        EXPECT_EQ(game_dir, "E:\\Game");
    }
}

TEST(PathsTest, Utf8DirAndGameDirCommandLineParsingPreservesJapanesePaths) {
    std::string project_dir;
    std::string game_dir;
    const apps::po::options_description desc =
        apps::BuildCommonOptions(project_dir, game_dir);

    constexpr const char* kJpProjectDir = "D:\\作業フォルダ\\俺たちに翼はない_チェックアウト";
    constexpr const char* kJpGameDir = "E:\\ゲーム\\俺たちに翼はない";

    apps::po::variables_map vm;
    const auto rc = RunParse(
        {"test_cli", "--dir", kJpProjectDir, "--game-dir", kJpGameDir}, desc, vm);
    EXPECT_FALSE(rc.has_value());
    EXPECT_EQ(project_dir, kJpProjectDir);
    EXPECT_EQ(game_dir, kJpGameDir);
    EXPECT_EQ(fs::u8path(project_dir).filename().u8string(),
              "俺たちに翼はない_チェックアウト");
    EXPECT_EQ(fs::u8path(game_dir).filename().u8string(),
              "俺たちに翼はない");
}

TEST(PathsTest, ProcessUtf8ArgsSkipsProgramNameAndMatchesAlias) {
    const std::vector<std::string> snake_args = apps::process_utf8_args();
    const std::vector<std::string> pascal_args = apps::ProcessUtf8Args();
    EXPECT_EQ(snake_args, pascal_args);
    for (const std::string& arg : snake_args) {
        EXPECT_EQ(arg.find("ut_paths.exe"), std::string::npos);
    }
}

TEST(PathsTest, ProjectOnlyOptionsAcceptsProjectDirAndRejectsGameDir) {
    std::string project_dir;
    const apps::po::options_description desc =
        apps::BuildProjectOnlyOptions(project_dir);

    {
        apps::po::variables_map vm;
        const auto rc = RunParse({"test_cli", "--help"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitSuccess);
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunParse({"test_cli", "--dir", "D:\\Project"}, desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project_dir, "D:\\Project");
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunParse(
            {"test_cli", "--dir", "D:\\Project", "--game-dir", "E:\\Game"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitCommandLineError);
    }
}
