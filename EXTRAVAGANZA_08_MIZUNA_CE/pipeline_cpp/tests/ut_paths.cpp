// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: project root discovery, UTF-8 CLI
// option parsing, and Windows wide command-line quoting/round-tripping.
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
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace apps = exm::apps;

namespace {

constexpr std::string_view kTempRootName = "vn_ut_paths_ext08";
constexpr int kNestedEmptyLevels = 8;
constexpr int kExitHelp = 0;
constexpr int kExitError = 1;
constexpr std::size_t kArgvExeSlot = 0;
constexpr std::size_t kFirstArgOffset = 1;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_);
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

class ScopedTestArgvW {
public:
    explicit ScopedTestArgvW(LPWSTR* argv) noexcept : argv_(argv) {}
    ~ScopedTestArgvW() {
        if (argv_ != nullptr) {
            LocalFree(argv_);
        }
    }

    ScopedTestArgvW(const ScopedTestArgvW&) = delete;
    ScopedTestArgvW& operator=(const ScopedTestArgvW&) = delete;

    LPWSTR* get() const noexcept { return argv_; }
    explicit operator bool() const noexcept { return argv_ != nullptr; }

private:
    LPWSTR* argv_;
};

std::string wide_to_utf8_strict(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("WideCharToMultiByte size failed");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        throw std::runtime_error("WideCharToMultiByte conversion failed");
    }
    return out;
}

std::vector<std::string> split_windows_command_line(const std::wstring& cmd_w) {
    int argc = 0;
    const ScopedTestArgvW argv_w(CommandLineToArgvW(cmd_w.c_str(), &argc));
    if (!argv_w || argc <= 0) {
        throw std::runtime_error("CommandLineToArgvW failed in test");
    }
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        out.push_back(wide_to_utf8_strict(argv_w.get()[i]));
    }
    return out;
}

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

TEST(Paths, ReleaseZipLayoutResolvesRootWithoutScriptOutput) {
    const ScopedTempDir temp(kTempRootName);
    const fs::path root =
        append_empty_levels(temp.path(), kNestedEmptyLevels) / "release_root";
    fs::create_directories(root / "bin");
    fs::create_directories(root / "game");

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(found, root.u8string());
}

TEST(Paths, CheckoutPriorityBeatsNestedBuildInstallStaging) {
    const ScopedTempDir temp(kTempRootName);
    const fs::path checkout =
        append_empty_levels(temp.path(), kNestedEmptyLevels) / "checkout";
    fs::create_directories(checkout / "pipeline_cpp");
    write_dummy_file(checkout / "build.py");
    fs::create_directories(checkout / "build" / "install" / "bin");
    fs::create_directories(checkout / "build" / "install" / "game");

    const std::string found =
        apps::find_project_dir_from(checkout / "build" / "install" / "bin");
    EXPECT_EQ(found, checkout.u8string());
}

TEST(Paths, LegacyStagedLayoutWithScriptOutputResolvesRoot) {
    const ScopedTempDir temp(kTempRootName);
    const fs::path root =
        append_empty_levels(temp.path(), kNestedEmptyLevels) / "legacy_root";
    fs::create_directories(root / "bin");
    fs::create_directories(root / "script_output");

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(found, root.u8string());
}

TEST(Paths, JapaneseDirectoryUtf8RootDiscoveryCheckoutAndRelease) {
    const ScopedTempDir temp(kTempRootName);

    const fs::path checkout =
        append_empty_levels(temp.path(), kNestedEmptyLevels) /
        fs::u8path("日本語チェックアウト_瑞奈");
    fs::create_directories(checkout / "pipeline_cpp");
    write_dummy_file(checkout / "build.py");
    fs::create_directories(checkout / "build" / "install" / "bin");
    fs::create_directories(checkout / "build" / "install" / "game");

    const std::string found_checkout =
        apps::find_project_dir_from(checkout / "build" / "install" / "bin");
    EXPECT_EQ(found_checkout, checkout.u8string());
    EXPECT_NE(found_checkout.find("日本語チェックアウト_瑞奈"), std::string::npos);

    const fs::path release_root =
        append_empty_levels(temp.path(), kNestedEmptyLevels) /
        fs::u8path("日本語リリース_瑞奈");
    fs::create_directories(release_root / "bin");
    fs::create_directories(release_root / "game");

    const std::string found_release =
        apps::find_project_dir_from(release_root / "bin");
    EXPECT_EQ(found_release, release_root.u8string());
    EXPECT_NE(found_release.find("日本語リリース_瑞奈"), std::string::npos);
}

TEST(Paths, UnrelatedDeepDirectoryReturnsEmpty) {
    const ScopedTempDir temp(kTempRootName);
    const fs::path deep = append_empty_levels(temp.path(), kNestedEmptyLevels);

    EXPECT_TRUE(apps::find_project_dir_from(deep).empty());
    EXPECT_TRUE(apps::find_project_dir_from(fs::path{}).empty());
}

TEST(Paths, ParseCommandLineHandlesCommonAndGameOnlyOptions) {
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
        const char* argv_ok[] = {"test_common", "--dir", "D:\\Project",
                                 "--game-dir", "E:\\Game"};
        const auto rc_ok = apps::parse_command_line(
            static_cast<int>(std::size(argv_ok)),
            const_cast<char**>(argv_ok), desc, vm_ok);
        EXPECT_FALSE(rc_ok.has_value());
        EXPECT_EQ(project, "D:\\Project");
        EXPECT_EQ(game, "E:\\Game");
    }

    {
        std::string game;
        apps::po::options_description desc("test_game_only");
        desc.add(apps::game_only_options(game));

        apps::po::variables_map vm_ok;
        const char* argv_ok[] = {"test_game_only", "--game-dir", "E:\\GameOnly"};
        const auto rc_ok = apps::parse_command_line(
            static_cast<int>(std::size(argv_ok)),
            const_cast<char**>(argv_ok), desc, vm_ok);
        EXPECT_FALSE(rc_ok.has_value());
        EXPECT_EQ(game, "E:\\GameOnly");

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

TEST(Paths, ParseCommandLinePreservesJapaneseUtf8Paths) {
    const std::string expected_project = "D:\\プロジェクト\\瑞奈CE";
    const std::string expected_game = "E:\\ゲーム\\EXTRA_VA_瑞奈";

    std::string project;
    std::string game;
    apps::po::options_description desc("test_utf8");
    desc.add(apps::common_options(project, game));

    apps::po::variables_map vm;
    const char* argv[] = {
        "test_utf8",
        "--dir", expected_project.c_str(),
        "--game-dir", expected_game.c_str(),
    };
    const auto rc = apps::parse_command_line(
        static_cast<int>(std::size(argv)),
        const_cast<char**>(argv), desc, vm);
    EXPECT_FALSE(rc.has_value());
    EXPECT_EQ(project, expected_project);
    EXPECT_EQ(game, expected_game);
}

TEST(Paths, ProcessUtf8ArgsRunsCleanlyOnCurrentProcess) {
    EXPECT_NO_THROW({
        const std::vector<std::string> args = apps::process_utf8_args();
        for (const std::string& arg : args) {
            EXPECT_FALSE(arg.empty());
        }
    });
}

TEST(Paths, BuildProcessCommandLineRoundTripsJapaneseQuotesAndBackslashes) {
    const std::string exe_path = "C:\\プログラム\\瑞奈 ツール\\01_extract.exe";
    const std::vector<std::string> args = {
        "--dir",
        "D:\\ゲーム データ\\瑞奈\\",
        "--game-dir",
        "E:\\インストール 先\\\\",
        "",
        "水菜 \"史郎\" テスト",
        "slash\\\"quote",
        "two\\\\\"quotes",
        "tab\tand\nnewline_瑞奈",
    };

    const std::wstring cmd_w = apps::build_process_command_line(exe_path, args);
    const std::vector<std::string> parsed = split_windows_command_line(cmd_w);

    ASSERT_EQ(parsed.size(), args.size() + kFirstArgOffset);
    EXPECT_EQ(parsed[kArgvExeSlot], exe_path);
    const std::vector<std::string> parsed_args(
        parsed.begin() + static_cast<std::ptrdiff_t>(kFirstArgOffset),
        parsed.end());
    EXPECT_EQ(parsed_args, args);
}

TEST(Paths, BuildProcessCommandLinePreservesSingleTrailingBackslashInExePath) {
    const std::string exe_path = "C:\\ゲーム フォルダ\\";
    const std::vector<std::string> args = {"--game-dir", "D:\\瑞奈\\"};

    const std::wstring cmd_w = apps::build_process_command_line(exe_path, args);
    const std::vector<std::string> parsed = split_windows_command_line(cmd_w);

    ASSERT_EQ(parsed.size(), args.size() + kFirstArgOffset);
    EXPECT_EQ(parsed[kArgvExeSlot], exe_path);
    const std::vector<std::string> parsed_args(
        parsed.begin() + static_cast<std::ptrdiff_t>(kFirstArgOffset),
        parsed.end());
    EXPECT_EQ(parsed_args, args);
}

TEST(Paths, BuildProcessCommandLineRejectsEmptyOrQuotedExePath) {
    EXPECT_THROW(
        apps::build_process_command_line("", {"--help"}),
        std::runtime_error);
    EXPECT_THROW(
        apps::build_process_command_line("C:\\bad\"quote\\tool.exe", {"--help"}),
        std::runtime_error);
}
