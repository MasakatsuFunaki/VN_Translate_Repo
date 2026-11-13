// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: two-pass UTF-8 project root discovery,
// wide UTF-16 -> UTF-8 CLI argument decoding, and Windows child-process command
// line quoting.
#include "apps/paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace apps = shin::apps;

constexpr int kNestedEmptyLevels = 8;
constexpr int kFirstArgOffset = 1;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name_utf8)
        : path_(fs::temp_directory_path() / fs::u8path(name_utf8)) {
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

    LPWSTR* get() const noexcept { return argv_; }

private:
    LPWSTR* argv_;
};

std::string WideArgToUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed in test");
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        utf8.data(), needed, nullptr, nullptr);
    if (written <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed in test");
    }
    return utf8;
}

std::vector<std::string> ParseWindowsCommandLineUtf8(const std::wstring& cmd_line) {
    int argc = 0;
    const ScopedLocalArgv argv(CommandLineToArgvW(cmd_line.c_str(), &argc));
    if (argv.get() == nullptr) {
        throw std::runtime_error("CommandLineToArgvW returned null");
    }
    std::vector<std::string> tokens;
    tokens.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        tokens.push_back(WideArgToUtf8(argv.get()[i]));
    }
    return tokens;
}

fs::path MakeIsolatedRoot(const fs::path& base, std::string_view leaf_utf8) {
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

}  // namespace

TEST(Paths, ReleaseZipLayoutResolvesRootWithoutScriptOutput) {
    ScopedTempDir temp("vn_ut_paths_shingakkou");
    const fs::path root = MakeIsolatedRoot(temp.path(), "release_root");
    fs::create_directories(root / "bin");
    fs::create_directories(root / "game");
    ASSERT_FALSE(fs::exists(root / "script_output"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(found, root.u8string());
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, CheckoutPriorityBeatsNestedBuildInstallStaging) {
    ScopedTempDir temp("vn_ut_paths_shingakkou");
    const fs::path checkout = MakeIsolatedRoot(temp.path(), "checkout");
    fs::create_directories(checkout / "pipeline_cpp");
    WriteDummyFile(checkout / "build.py");

    const fs::path staged_bin = checkout / "build" / "install" / "bin";
    const fs::path staged_game = checkout / "build" / "install" / "game";
    fs::create_directories(staged_bin);
    fs::create_directories(staged_game);

    const std::string found = apps::find_project_dir_from(staged_bin);
    EXPECT_EQ(found, checkout.u8string());
    EXPECT_EQ(fs::u8path(found), checkout);
}

TEST(Paths, LegacyStagedLayoutWithScriptOutputResolvesRoot) {
    ScopedTempDir temp("vn_ut_paths_shingakkou");
    const fs::path root = MakeIsolatedRoot(temp.path(), "legacy_staged");
    fs::create_directories(root / "bin");
    fs::create_directories(root / "script_output");
    ASSERT_FALSE(fs::exists(root / "game"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(found, root.u8string());
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, JapaneseDirectoryNamesResolveCleanUtf8RootForCheckoutAndRelease) {
    ScopedTempDir temp("vn_ut_paths_jp_shingakkou");

    const fs::path jp_checkout =
        MakeIsolatedRoot(temp.path(), "日本語チェックアウト_神学校");
    fs::create_directories(jp_checkout / "pipeline_cpp");
    WriteDummyFile(jp_checkout / "build.py");
    const fs::path jp_checkout_bin = jp_checkout / "build" / "install" / "bin";
    fs::create_directories(jp_checkout_bin);
    EXPECT_EQ(apps::find_project_dir_from(jp_checkout_bin), jp_checkout.u8string());

    const fs::path jp_release =
        MakeIsolatedRoot(temp.path(), "日本語リリース_神学校");
    fs::create_directories(jp_release / "bin");
    fs::create_directories(jp_release / "game");
    EXPECT_EQ(apps::find_project_dir_from(jp_release / "bin"), jp_release.u8string());
}

TEST(Paths, UnrelatedDeepDirectoryReturnsEmpty) {
    ScopedTempDir temp("vn_ut_paths_shingakkou");
    fs::path deep = temp.path();
    for (int level = 0; level < kNestedEmptyLevels; ++level) {
        deep /= "empty";
    }
    fs::create_directories(deep);

    EXPECT_TRUE(apps::find_project_dir_from(deep).empty());
    EXPECT_TRUE(apps::find_project_dir_from(fs::path{}).empty());
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
        const auto rc = RunParse({"test_cli", "--dir", "D:\\Project"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunParse(
            {"test_cli", "stray_token", "--dir", "D:\\Project", "--game-dir", "E:\\Game"},
            desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunParse(
            {"test_cli", "--dir", "D:\\Project", "--game-dir", "E:\\Game"}, desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, "D:\\Project");
        EXPECT_EQ(game, "E:\\Game");
    }

    {
        apps::po::variables_map vm;
        const std::string jp_project = "D:\\日本語プロジェクト\\神学校";
        const std::string jp_game = "E:\\ゲーム置き場\\『神学校-Noli me tangere-』";
        const auto rc = RunParse(
            {"test_cli", "--dir", jp_project.c_str(), "--game-dir", jp_game.c_str()},
            desc, vm);
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
        const auto rc = RunParse(
            {"test_game_only", "--dir", "D:\\Project", "--game-dir", "E:\\Game"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunParse({"test_game_only", "--game-dir", "E:\\Game"}, desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(game, "E:\\Game");
    }
}

TEST(Paths, ProcessUtf8ArgsDecodesCurrentTestProcessArgumentsWithoutThrowing) {
    EXPECT_NO_THROW({
        const std::vector<std::string> current_args = apps::process_utf8_args();
        (void)current_args;
    });
}

TEST(Paths, BuildProcessCommandLineRoundTripsJapaneseSpacesQuotesAndBackslashes) {
    const std::string exe_path = "D:\\日本語 ツール\\神学校\\01_extract.exe";
    const std::vector<std::string> args = {
        "--dir",
        "D:\\プロジェクト フォルダ\\神学校\\",
        "--game-dir",
        "E:\\ゲーム\\『神学校 -Noli me tangere-』\\\\",
        "empty=",
        "",
        "odd_slash_quote=a\\\"b",
        "even_slash_quote=a\\\\\"b",
        "tab\tand\nnewline\vvtab",
    };

    const std::wstring cmd_w = apps::build_process_command_line(exe_path, args);
    const std::vector<std::string> parsed = ParseWindowsCommandLineUtf8(cmd_w);

    ASSERT_EQ(parsed.size(), args.size() + static_cast<std::size_t>(kFirstArgOffset));
    EXPECT_EQ(parsed[0], exe_path);
    for (std::size_t i = 0; i < args.size(); ++i) {
        EXPECT_EQ(parsed[i + static_cast<std::size_t>(kFirstArgOffset)], args[i])
            << "mismatch at arg index " << i;
    }
}

TEST(Paths, BuildProcessCommandLinePreservesSingleTrailingBackslashInArgv0) {
    const std::string trailing_slash_exe = "C:\\ゲーム フォルダ\\";
    const std::vector<std::string> args = {"--game-dir", "D:\\神学校\\"};

    const std::wstring cmd_w =
        apps::build_process_command_line(trailing_slash_exe, args);
    const std::vector<std::string> parsed = ParseWindowsCommandLineUtf8(cmd_w);

    ASSERT_EQ(parsed.size(), args.size() + static_cast<std::size_t>(kFirstArgOffset));
    EXPECT_EQ(parsed[0], trailing_slash_exe);
    EXPECT_EQ(parsed[1], args[0]);
    EXPECT_EQ(parsed[2], args[1]);
}

TEST(Paths, BuildProcessCommandLineRejectsEmptyOrQuotedExecutablePath) {
    EXPECT_THROW(apps::build_process_command_line("", {"--help"}), std::runtime_error);
    EXPECT_THROW(
        apps::build_process_command_line("C:\\bad\"exe\\01_extract.exe", {"--help"}),
        std::runtime_error);
}
