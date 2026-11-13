// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: UTF-8 project root discovery, CLI
// argument parsing, and Windows wide command-line round-trips.
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

constexpr std::string_view kTempRootName = "vn_ut_paths_ext06";
constexpr int kNestedEmptyLevels = 8;
constexpr int kExitHelp = 0;
constexpr int kExitError = 1;
constexpr std::size_t kArgv0Index = 0;
constexpr std::size_t kFirstUserArgOffset = 1;

struct LocalArgvDeleter {
    void operator()(wchar_t** ptr) const noexcept {
        if (ptr != nullptr) {
            LocalFree(ptr);
        }
    }
};
using ScopedLocalArgv = std::unique_ptr<wchar_t*, LocalArgvDeleter>;

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

std::string wide_to_utf8_checked(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int byte_len = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (byte_len <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed in test");
    }
    std::string out(static_cast<std::size_t>(byte_len), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), out.data(), byte_len, nullptr, nullptr);
    if (written != byte_len) {
        throw std::runtime_error("WideCharToMultiByte length mismatch in test");
    }
    return out;
}

std::vector<std::string> split_command_line_utf8(const std::wstring& cmd_line) {
    int argc = 0;
    const ScopedLocalArgv argv(CommandLineToArgvW(cmd_line.c_str(), &argc));
    if (!argv || argc <= 0) {
        throw std::runtime_error("CommandLineToArgvW failed in test");
    }
    std::vector<std::string> tokens;
    tokens.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        tokens.push_back(wide_to_utf8_checked(argv.get()[i]));
    }
    return tokens;
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
    const fs::path padded = append_empty_levels(temp.path(), kNestedEmptyLevels);

    const fs::path jp_checkout = padded / fs::u8path("日本語チェックアウト_夢美");
    fs::create_directories(jp_checkout / "pipeline_cpp");
    write_dummy_file(jp_checkout / "build.py");
    fs::create_directories(jp_checkout / "build" / "install" / "bin");

    EXPECT_EQ(apps::find_project_dir_from(jp_checkout / "build" / "install" / "bin"),
              jp_checkout.u8string());

    const fs::path jp_release = padded / fs::u8path("日本語リリース_夢美");
    fs::create_directories(jp_release / "bin");
    fs::create_directories(jp_release / "game");

    EXPECT_EQ(apps::find_project_dir_from(jp_release / "bin"),
              jp_release.u8string());
}

TEST(Paths, UnrelatedDeepDirectoryReturnsEmpty) {
    const ScopedTempDir temp(kTempRootName);
    const fs::path deep = append_empty_levels(temp.path(), kNestedEmptyLevels);

    EXPECT_TRUE(apps::find_project_dir_from(deep).empty());
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
        const char* argv_missing[] = {"test_common", "--dir", "E:\\Project"};
        const auto rc_missing = apps::parse_command_line(
            static_cast<int>(std::size(argv_missing)),
            const_cast<char**>(argv_missing), desc, vm_missing);
        ASSERT_TRUE(rc_missing.has_value());
        EXPECT_EQ(*rc_missing, kExitError);

        const std::string expected_jp_project = "E:\\プロジェクト\\夢美陵辱_開発";
        const std::string expected_jp_game = "F:\\ゲーム\\夢美陵辱 Complete Edition";
        apps::po::variables_map vm_ok;
        const char* argv_ok[] = {
            "test_common",
            "--dir",
            expected_jp_project.c_str(),
            "--game-dir",
            expected_jp_game.c_str(),
        };
        const auto rc_ok = apps::parse_command_line(
            static_cast<int>(std::size(argv_ok)),
            const_cast<char**>(argv_ok), desc, vm_ok);
        EXPECT_FALSE(rc_ok.has_value());
        EXPECT_EQ(project, expected_jp_project);
        EXPECT_EQ(game, expected_jp_game);
    }

    {
        std::string game;
        apps::po::options_description desc("test_game_only");
        desc.add(apps::game_only_options(game));

        const std::string expected_game = "F:\\夢美ゲーム本体";
        apps::po::variables_map vm_ok;
        const char* argv_ok[] = {"test_game_only", "--game-dir", expected_game.c_str()};
        const auto rc_ok = apps::parse_command_line(
            static_cast<int>(std::size(argv_ok)),
            const_cast<char**>(argv_ok), desc, vm_ok);
        EXPECT_FALSE(rc_ok.has_value());
        EXPECT_EQ(game, expected_game);

        apps::po::variables_map vm_reject_dir;
        const char* argv_reject_dir[] = {
            "test_game_only", "--dir", "E:\\Project", "--game-dir", "F:\\GameOnly"};
        const auto rc_reject_dir = apps::parse_command_line(
            static_cast<int>(std::size(argv_reject_dir)),
            const_cast<char**>(argv_reject_dir), desc, vm_reject_dir);
        ASSERT_TRUE(rc_reject_dir.has_value());
        EXPECT_EQ(*rc_reject_dir, kExitError);
    }
}

TEST(Paths, ProcessUtf8ArgsRunsCleanlyOnCurrentProcess) {
    EXPECT_NO_THROW({
        const std::vector<std::string> args = apps::process_utf8_args();
        (void)args;
    });
}

TEST(Paths, BuildProcessCommandLineRoundTripsJapaneseQuotesAndBackslashes) {
    const std::string exe_path = "C:\\夢美 開発\\bin\\02_build_tsv.exe";
    const std::vector<std::string> expected_args = {
        "--dir",
        "E:\\夢美 プロジェクト\\",
        "--game-dir",
        "F:\\ゲーム 本体\\\\",
        "say \"夢美\"",
        "one_bs\\\"夢美",
        "two_bs\\\\\"夢美",
        "",
        "tab\t夢美\nline",
    };

    const std::wstring cmd_w =
        apps::build_process_command_line(exe_path, expected_args);
    const std::vector<std::string> parsed = split_command_line_utf8(cmd_w);

    ASSERT_EQ(parsed.size(), expected_args.size() + kFirstUserArgOffset);
    EXPECT_EQ(parsed[kArgv0Index], exe_path);
    for (std::size_t i = 0; i < expected_args.size(); ++i) {
        EXPECT_EQ(parsed[i + kFirstUserArgOffset], expected_args[i]) << "arg index=" << i;
    }
}

TEST(Paths, BuildProcessCommandLinePreservesTrailingBackslashInArgv0AndRejectsInvalidExe) {
    const std::string trailing_slash_exe = "C:\\ゲーム フォルダ\\";
    const std::vector<std::string> args = {"--game-dir", "F:\\夢美 フォルダ\\"};

    const std::wstring cmd_w =
        apps::build_process_command_line(trailing_slash_exe, args);
    const std::vector<std::string> parsed = split_command_line_utf8(cmd_w);

    ASSERT_EQ(parsed.size(), args.size() + kFirstUserArgOffset);
    EXPECT_EQ(parsed[kArgv0Index], trailing_slash_exe);
    EXPECT_EQ(parsed[kFirstUserArgOffset], args[0]);
    EXPECT_EQ(parsed[kFirstUserArgOffset + 1], args[1]);

    EXPECT_THROW(apps::build_process_command_line("", {}), std::runtime_error);
    EXPECT_THROW(apps::build_process_command_line("C:\\bad\"exe.exe", {}),
                 std::runtime_error);
}
