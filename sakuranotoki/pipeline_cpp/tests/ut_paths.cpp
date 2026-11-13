// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: UTF-8/UTF-16 conversions, project root
// discovery (including Japanese paths), CLI parsing, CommandLineToArgvW
// round-tripping, and CreateProcessW child execution.
#include "apps/paths.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
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
namespace apps = snt::apps;

constexpr int kNestedEmptyLevels = 8;
constexpr int kNonZeroChildExitCode = 7;
constexpr std::size_t kLongOptionPrefixLen = 2;

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

fs::path make_isolated_root(const fs::path& base, std::string_view utf8_leaf) {
    fs::path dir = base;
    for (int level = 0; level < kNestedEmptyLevels; ++level) {
        dir /= "p";
    }
    dir /= fs::u8path(utf8_leaf);
    return dir;
}

void write_dummy_file(const fs::path& file_path) {
    fs::create_directories(file_path.parent_path());
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    out << "# build script\n";
}

std::optional<int> run_argc_parse(std::initializer_list<const char*> args,
                                  const apps::po::options_description& desc,
                                  apps::po::variables_map& vm) {
    std::vector<std::string> storage(args.begin(), args.end());
    std::vector<const char*> argv;
    argv.reserve(storage.size());
    for (const std::string& s : storage) {
        argv.push_back(s.c_str());
    }
    return apps::parse_command_line(
        static_cast<int>(argv.size()), argv.data(), desc, vm);
}

void register_active_gtest_flags(apps::po::options_description& desc) {
    for (const std::string& arg : apps::process_utf8_args()) {
        if (arg.rfind("--gtest_", 0) != 0) {
            continue;
        }
        const std::size_t eq_pos = arg.find('=');
        if (eq_pos == std::string::npos) {
            const std::string flag_name = arg.substr(kLongOptionPrefixLen);
            if (desc.find_nothrow(flag_name, false) == nullptr) {
                desc.add_options()(flag_name.c_str(), "gtest switch");
            }
            continue;
        }
        const std::string opt_name =
            arg.substr(kLongOptionPrefixLen, eq_pos - kLongOptionPrefixLen);
        if (desc.find_nothrow(opt_name, false) == nullptr) {
            desc.add_options()(
                opt_name.c_str(), apps::po::value<std::string>(), "gtest option");
        }
    }
}

}  // namespace

TEST(Paths, ReleaseZipLayoutResolvesRootWithoutScriptOutput) {
    ScopedTempDir temp("vn_ut_paths_sakuranotoki");
    const fs::path root = make_isolated_root(temp.path(), "release_root");
    fs::create_directories(root / "bin");
    fs::create_directories(root / "game");
    ASSERT_FALSE(fs::exists(root / "script_output"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, CheckoutPriorityBeatsNestedBuildInstallStaging) {
    ScopedTempDir temp("vn_ut_paths_sakuranotoki");
    const fs::path checkout = make_isolated_root(temp.path(), "checkout");
    fs::create_directories(checkout / "pipeline_cpp");
    write_dummy_file(checkout / "build.py");

    const fs::path staged_bin = checkout / "build" / "install" / "bin";
    const fs::path staged_game = checkout / "build" / "install" / "game";
    fs::create_directories(staged_bin);
    fs::create_directories(staged_game);

    const std::string found = apps::find_project_dir_from(staged_bin);
    EXPECT_EQ(fs::u8path(found), checkout);
}

TEST(Paths, LegacyStagedLayoutWithScriptOutputResolvesRoot) {
    ScopedTempDir temp("vn_ut_paths_sakuranotoki");
    const fs::path root = make_isolated_root(temp.path(), "legacy_staged");
    fs::create_directories(root / "bin");
    fs::create_directories(root / "script_output");
    ASSERT_FALSE(fs::exists(root / "game"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, JapaneseDirectoryResolutionPreservesUtf8) {
    ScopedTempDir temp("vn_ut_paths_サクラノ刻");
    const fs::path checkout =
        make_isolated_root(temp.path(), "枕/サクラノ刻 -櫻の森の下を歩む-");
    fs::create_directories(checkout / "pipeline_cpp");
    write_dummy_file(checkout / "build.py");

    const fs::path nested_bin = checkout / "build" / "install" / "bin";
    fs::create_directories(nested_bin);

    const std::string found = apps::find_project_dir_from(nested_bin);
    EXPECT_EQ(found, checkout.u8string());
    EXPECT_NE(found.find("サクラノ刻"), std::string::npos);
    EXPECT_EQ(fs::u8path(found), checkout);
}

TEST(Paths, UnrelatedDeepDirectoryReturnsEmpty) {
    ScopedTempDir temp("vn_ut_paths_sakuranotoki");
    fs::path deep = temp.path();
    for (int level = 0; level < kNestedEmptyLevels; ++level) {
        deep /= "empty";
    }
    fs::create_directories(deep);

    EXPECT_TRUE(apps::find_project_dir_from(deep).empty());
    EXPECT_TRUE(apps::find_project_dir_from(fs::path{}).empty());
}

TEST(Paths, Utf8AndWideConversionsValidateAndRoundTrip) {
    const std::string jp_path = "C:\\枕\\サクラノ刻 -櫻の森の下を歩む-";
    const std::wstring wide = apps::utf8_to_wide(jp_path);
    EXPECT_EQ(apps::wide_to_utf8(wide), jp_path);
    EXPECT_TRUE(apps::utf8_to_wide("").empty());
    EXPECT_TRUE(apps::wide_to_utf8(L"").empty());

    const std::uint8_t invalid_utf8[] = {0xFFU, 0xFEU};
    const std::string bad_bytes(
        reinterpret_cast<const char*>(invalid_utf8), sizeof(invalid_utf8));
    EXPECT_THROW(apps::utf8_to_wide(bad_bytes), std::runtime_error);
}

TEST(Paths, ProcessUtf8ArgsAndParseProcessCommandLineSucceed) {
    const std::vector<std::string> live_args = apps::process_utf8_args();
    for (const std::string& arg : live_args) {
        EXPECT_FALSE(arg.empty());
    }

    {
        std::string project;
        apps::po::options_description desc("process_cli");
        desc.add(apps::project_only_options(project));
        register_active_gtest_flags(desc);

        apps::po::variables_map vm;
        const auto rc = apps::parse_process_command_line(desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, apps::default_project_dir());
    }

    {
        std::string project;
        apps::po::options_description desc("process_project_only_cli");
        desc.add(apps::project_only_options(project));
        register_active_gtest_flags(desc);

        apps::po::variables_map vm;
        const auto rc = apps::parse_process_project_only_command_line(desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, apps::default_project_dir());
    }
}

TEST(Paths, ParseCommandLineHandlesVectorAndArgcOverloads) {
    std::string project;
    std::string game;
    apps::po::options_description desc("test_cli");
    desc.add(apps::common_options(project, game));

    {
        apps::po::variables_map vm;
        const auto rc = run_argc_parse({"test_cli", "--help"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitOk);
    }

    {
        apps::po::variables_map vm;
        const auto rc =
            apps::parse_command_line(std::vector<std::string>{"--help"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitOk);
    }

    {
        apps::po::variables_map vm;
        const auto rc = run_argc_parse({"test_cli", "--dir", "D:\\Project"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitFailure);
    }

    {
        apps::po::variables_map vm;
        const auto rc = apps::parse_command_line(
            std::vector<std::string>{
                "test_cli", "--dir", "D:\\Project", "--game-dir", "E:\\Game"},
            desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitFailure);
    }

    {
        apps::po::variables_map vm;
        const auto rc = run_argc_parse(
            {"test_cli", "stray_token", "--dir", "D:\\Project", "--game-dir", "E:\\Game"},
            desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitFailure);
    }

    {
        apps::po::variables_map vm;
        const std::string jp_game_dir = "C:\\枕\\サクラノ刻 -櫻の森の下を歩む-";
        const std::string jp_proj_dir = "D:\\Projects\\サクラノ刻";
        const auto rc = apps::parse_command_line(
            std::vector<std::string>{
                "--dir", jp_proj_dir, "--game-dir", jp_game_dir},
            desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, jp_proj_dir);
        EXPECT_EQ(game, jp_game_dir);
    }
}

TEST(Paths, ProjectOnlyOptionsAcceptsDirAndRejectsGameDir) {
    std::string project;
    apps::po::options_description desc("test_project_only");
    desc.add(apps::project_only_options(project));

    {
        apps::po::variables_map vm;
        const auto rc = apps::parse_project_only_command_line(
            std::vector<std::string>{
                "--dir", "D:\\Project", "--game-dir", "E:\\Game"},
            desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitFailure);
    }

    {
        apps::po::variables_map vm;
        const auto rc = apps::parse_project_only_command_line(
            std::vector<std::string>{"--dir", "D:\\サクラノ刻"}, desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, "D:\\サクラノ刻");
    }

    {
        std::string bad_project;
        std::string bad_game;
        apps::po::options_description non_project_desc("has_game_dir");
        non_project_desc.add(apps::common_options(bad_project, bad_game));

        apps::po::variables_map vm;
        const auto rc = apps::parse_project_only_command_line(
            std::vector<std::string>{
                "--dir", "D:\\Project", "--game-dir", "E:\\Game"},
            non_project_desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitFailure);
    }
}

TEST(Paths, CommandLineToArgvWRoundTripsTrailingBackslashesAndQuotes) {
    {
        const std::string exe_trailing = "C:\\dir\\";
        const std::vector<std::string> args = {
            "--dir",
            "C:\\dir\\",
            "--game-dir",
            "C:\\dir with space\\",
            "--note",
            "say \"hello\" \\\"world\\\"\\",
            ""
        };
        const std::string formatted =
            apps::format_windows_command_line(exe_trailing, args);
        EXPECT_EQ(formatted.rfind("\"C:\\dir\\\"", 0), 0U);

        const std::vector<std::string> split =
            apps::split_windows_command_line(formatted);
        std::vector<std::string> expected = {exe_trailing};
        expected.insert(expected.end(), args.begin(), args.end());
        EXPECT_EQ(split, expected);
    }

    {
        const std::string jp_exe_trailing = "C:\\枕\\サクラノ刻\\";
        const std::vector<std::string> args = {
            "--game-dir",
            "C:\\枕\\サクラノ刻 -櫻の森の下を歩む-\\",
            "quote\"inside",
            "\\\\server\\share\\サクラノ刻\\"
        };
        const std::string formatted =
            apps::format_windows_command_line(jp_exe_trailing, args);
        const std::vector<std::string> split =
            apps::split_windows_command_line(formatted);
        std::vector<std::string> expected = {jp_exe_trailing};
        expected.insert(expected.end(), args.begin(), args.end());
        EXPECT_EQ(split, expected);
    }

    EXPECT_THROW(
        apps::format_windows_command_line("C:\\bad\"exe.exe", {}),
        std::runtime_error);
    EXPECT_THROW(
        apps::format_windows_command_line("", {}),
        std::runtime_error);
    EXPECT_THROW(
        apps::split_windows_command_line("   "),
        std::runtime_error);
}

TEST(Paths, RunProcessReportsExitCodesAndThrowsOnMissingBinary) {
    ScopedTempDir temp("vn_ut_run_process_サクラノ刻");

    EXPECT_EQ(
        apps::run_process("cmd.exe", {"/c", "exit 0"}, temp.path()),
        apps::kExitOk);
    EXPECT_EQ(
        apps::run_process("cmd.exe", {"/c", "exit 7"}, temp.path()),
        kNonZeroChildExitCode);

    const fs::path missing_exe =
        temp.path() / "definitely_missing_sakuranotoki_binary.exe";
    EXPECT_THROW(
        apps::run_process(missing_exe, {}),
        std::runtime_error);
}
