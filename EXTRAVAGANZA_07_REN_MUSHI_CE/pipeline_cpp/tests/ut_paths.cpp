// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: UTF-8 project root discovery, CLI
// argument parsing, and Win32 wide command-line escaping.
#include "apps/paths.h"

#include <gtest/gtest.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace apps = exm::apps;

namespace {

constexpr std::string_view kTempRootName = "vn_ut_paths_ext07";
constexpr int kNestedEmptyLevels = 8;
constexpr int kExitHelp = 0;
constexpr int kExitError = 1;
constexpr std::size_t kExeSlotCount = 1;

struct LocalFreeDeleter {
    void operator()(wchar_t** ptr) const noexcept {
        if (ptr != nullptr) {
            LocalFree(ptr);
        }
    }
};
using ScopedArgvW = std::unique_ptr<wchar_t*, LocalFreeDeleter>;

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

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int byte_len = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (byte_len <= 0) {
        throw std::runtime_error("WideCharToMultiByte(UTF-8) failed");
    }
    std::string out(static_cast<std::size_t>(byte_len), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), out.data(), byte_len, nullptr, nullptr);
    if (written != byte_len) {
        throw std::runtime_error("WideCharToMultiByte(UTF-8) length mismatch");
    }
    return out;
}

std::vector<std::string> split_command_line_utf8(const std::wstring& cmd_line) {
    int argc = 0;
    wchar_t** argv_w = CommandLineToArgvW(cmd_line.c_str(), &argc);
    if (argv_w == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed");
    }
    const ScopedArgvW guard(argv_w);
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        out.push_back(wide_to_utf8(argv_w[i]));
    }
    return out;
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

TEST(Paths, JapaneseDirectoryUtf8RootDiscoveryForCheckoutAndRelease) {
    const ScopedTempDir temp(kTempRootName);
    const fs::path base = append_empty_levels(temp.path(), kNestedEmptyLevels);

    const fs::path checkout = base / fs::u8path("日本語チェックアウト_戀蟲");
    fs::create_directories(checkout / "pipeline_cpp");
    write_dummy_file(checkout / "build.py");
    fs::create_directories(checkout / "build" / "install" / "bin");
    fs::create_directories(checkout / "build" / "install" / "game");
    EXPECT_EQ(apps::find_project_dir_from(checkout / "build" / "install" / "bin"),
              checkout.u8string());

    const fs::path release = base / fs::u8path("日本語リリース_戀蟲");
    fs::create_directories(release / "bin");
    fs::create_directories(release / "game");
    EXPECT_EQ(apps::find_project_dir_from(release / "bin"), release.u8string());
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

TEST(Paths, ParseCommandLinePreservesUtf8JapanesePaths) {
    const std::string expected_project = "D:\\日本語プロジェクト\\戀蟲_CE";
    const std::string expected_game = "E:\\ゲーム置き場\\レンと蟲クンのいちにち";

    std::string project;
    std::string game;
    apps::po::options_description desc("test_jp_paths");
    desc.add(apps::common_options(project, game));

    apps::po::variables_map vm;
    const char* argv[] = {
        "test_jp_paths",
        "--dir",
        expected_project.c_str(),
        "--game-dir",
        expected_game.c_str(),
    };
    const auto rc = apps::parse_command_line(
        static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
    EXPECT_FALSE(rc.has_value());
    EXPECT_EQ(project, expected_project);
    EXPECT_EQ(game, expected_game);
    EXPECT_EQ(fs::u8path(project).u8string(), expected_project);
    EXPECT_EQ(fs::u8path(game).u8string(), expected_game);
}

TEST(Paths, ProcessUtf8ArgsRunsCleanlyOnCurrentTestProcess) {
    EXPECT_NO_THROW({
        const std::vector<std::string> args = apps::process_utf8_args();
        (void)args;
    });
}

TEST(Paths, BuildProcessCommandLineRoundTripsThroughCommandLineToArgvW) {
    const std::string exe_path =
        "C:\\ゲーム フォルダ\\戀蟲 ツール\\01_extract.exe";
    const std::vector<std::string> args = {
        "--dir",
        "D:\\日本語 フォルダ\\戀蟲_CE",
        "--game-dir",
        "E:\\ゲーム フォルダ\\末尾スラッシュ\\",
        "",
        "embedded\"quote",
        "odd_slash\\\"quote",
        "even_slashes\\\\\"quote",
        "trailing double\\\\",
        "--rebuild-table",
    };

    const std::wstring cmd_line = apps::build_process_command_line(exe_path, args);
    const std::vector<std::string> parsed = split_command_line_utf8(cmd_line);
    ASSERT_EQ(parsed.size(), kExeSlotCount + args.size());
    EXPECT_EQ(parsed[0], exe_path);
    for (std::size_t i = 0; i < args.size(); ++i) {
        EXPECT_EQ(parsed[kExeSlotCount + i], args[i]) << "arg index=" << i;
    }

    const std::string trailing_backslash_exe = "C:\\ゲーム フォルダ\\";
    const std::vector<std::string> single_arg = {"--help"};
    const std::wstring trailing_cmd =
        apps::build_process_command_line(trailing_backslash_exe, single_arg);
    const std::vector<std::string> trailing_parsed =
        split_command_line_utf8(trailing_cmd);
    ASSERT_EQ(trailing_parsed.size(), kExeSlotCount + single_arg.size());
    EXPECT_EQ(trailing_parsed[0], trailing_backslash_exe);
    EXPECT_EQ(trailing_parsed[kExeSlotCount], single_arg[0]);
}

TEST(Paths, BuildProcessCommandLineRejectsEmptyOrQuotedExePath) {
    EXPECT_THROW(apps::build_process_command_line("", {"--help"}),
                 std::runtime_error);
    EXPECT_THROW(apps::build_process_command_line("C:\\bad\"path\\01_extract.exe",
                                                  {"--help"}),
                 std::runtime_error);
}
