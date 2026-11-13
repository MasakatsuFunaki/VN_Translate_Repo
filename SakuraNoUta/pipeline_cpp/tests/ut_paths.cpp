// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: UTF-16 <-> UTF-8 conversion, two-pass
// project root discovery across Japanese paths, CLI parsing, Windows command-line
// escaping/splitting, and wide CreateProcessW child execution.
#include "apps/paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace apps = snu::apps;

constexpr int kNestedEmptyLevels = 8;
constexpr int kNonZeroChildExitCode = 7;
constexpr int kMissingCwdMarkerExitCode = 3;
constexpr std::size_t kLongFlagPrefixLen = 2;
constexpr char kMalformedUtf8Lead = static_cast<char>(0xFF);
constexpr char kMalformedUtf8Trail = static_cast<char>(0xFE);
constexpr wchar_t kUnpairedHighSurrogate = static_cast<wchar_t>(0xD800);

class ScopedTempDir {
public:
    explicit ScopedTempDir(const std::string& utf8_name)
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

fs::path MakeIsolatedRoot(const fs::path& base, const std::string& utf8_leaf) {
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

std::optional<int> RunArgvParse(std::initializer_list<const char*> args,
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

std::optional<int> RunProjectOnlyArgvParse(
    std::initializer_list<const char*> args,
    const apps::po::options_description& desc,
    apps::po::variables_map& vm) {
    std::vector<std::string> storage(args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (std::string& s : storage) {
        argv.push_back(s.data());
    }
    return apps::parse_project_only_command_line(
        static_cast<int>(argv.size()), argv.data(), desc, vm);
}

void RegisterLiveProcessGtestFlags(apps::po::options_description& desc,
                                   const std::vector<std::string>& live_args) {
    for (const auto& token : live_args) {
        if (token.rfind("--", 0) != 0) {
            continue;
        }
        const std::size_t eq = token.find('=');
        const std::string opt_name =
            eq == std::string::npos
                ? token.substr(kLongFlagPrefixLen)
                : token.substr(kLongFlagPrefixLen, eq - kLongFlagPrefixLen);
        if (opt_name.empty() || desc.find_nothrow(opt_name, false) != nullptr) {
            continue;
        }
        desc.add_options()(
            opt_name.c_str(),
            apps::po::value<std::string>()->implicit_value(""));
    }
}

std::string SystemCmdExeUtf8() {
    std::wstring sys_dir(MAX_PATH, L'\0');
    const UINT len = GetSystemDirectoryW(sys_dir.data(),
                                         static_cast<UINT>(sys_dir.size()));
    if (len == 0) {
        throw std::runtime_error("GetSystemDirectoryW failed");
    }
    sys_dir.resize(len);
    return (fs::path(sys_dir) / L"cmd.exe").u8string();
}

}  // namespace

TEST(Paths, ReleaseZipLayoutResolvesRootWithoutScriptOutput) {
    ScopedTempDir temp("vn_ut_paths_sakuranouta");
    const fs::path root = MakeIsolatedRoot(temp.path(), "release_root");
    fs::create_directories(root / "bin");
    fs::create_directories(root / "game");
    ASSERT_FALSE(fs::exists(root / "script_output"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, CheckoutPriorityBeatsNestedBuildInstallStaging) {
    ScopedTempDir temp("vn_ut_paths_sakuranouta");
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
    ScopedTempDir temp("vn_ut_paths_sakuranouta");
    const fs::path root = MakeIsolatedRoot(temp.path(), "legacy_staged");
    fs::create_directories(root / "bin");
    fs::create_directories(root / "script_output");
    ASSERT_FALSE(fs::exists(root / "game"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, UnrelatedDeepDirectoryReturnsEmpty) {
    ScopedTempDir temp("vn_ut_paths_sakuranouta");
    fs::path deep = temp.path();
    for (int level = 0; level < kNestedEmptyLevels; ++level) {
        deep /= "empty";
    }
    fs::create_directories(deep);

    EXPECT_TRUE(apps::find_project_dir_from(deep).empty());
    EXPECT_TRUE(apps::find_project_dir_from(fs::path{}).empty());
}

TEST(Paths, JapaneseUtf8DirectoryResolvesLosslessly) {
    ScopedTempDir temp("vn_ut_paths_櫻の詩");
    const fs::path jp_parent = MakeIsolatedRoot(temp.path(), "枕");
    const fs::path jp_root = jp_parent / fs::u8path("櫻の詩 -Sakura no Uta-");
    fs::create_directories(jp_root / "bin");
    fs::create_directories(jp_root / "game");

    const std::string found = apps::find_project_dir_from(jp_root / "bin");
    EXPECT_EQ(fs::u8path(found), jp_root);
    EXPECT_NE(found.find("櫻の詩"), std::string::npos);
    EXPECT_NE(found.find("枕\\櫻の詩 -Sakura no Uta-"), std::string::npos);

    const fs::path jp_checkout = jp_parent / fs::u8path("櫻の詩");
    fs::create_directories(jp_checkout / "pipeline_cpp");
    WriteDummyFile(jp_checkout / "build.py");
    const std::string found_checkout =
        apps::find_project_dir_from(jp_checkout / "pipeline_cpp");
    EXPECT_EQ(fs::u8path(found_checkout), jp_checkout);
    EXPECT_NE(found_checkout.find("櫻の詩"), std::string::npos);
}

TEST(Paths, Utf8WideConversionRoundTripsJapanesePathsAndRejectsInvalidInput) {
    EXPECT_TRUE(apps::utf8_to_wide("").empty());
    EXPECT_TRUE(apps::wide_to_utf8(L"").empty());

    const std::string jp_path = "C:\\枕\\櫻の詩 -Sakura no Uta-\\櫻の詩.exe";
    const std::wstring wide = apps::utf8_to_wide(jp_path);
    EXPECT_EQ(wide, L"C:\\枕\\櫻の詩 -Sakura no Uta-\\櫻の詩.exe");
    EXPECT_EQ(apps::wide_to_utf8(wide), jp_path);

    const std::array<char, 2> bad_utf8 = {kMalformedUtf8Lead, kMalformedUtf8Trail};
    EXPECT_THROW(
        apps::utf8_to_wide(std::string_view(bad_utf8.data(), bad_utf8.size())),
        std::runtime_error);

    const std::array<wchar_t, 1> bad_wide = {kUnpairedHighSurrogate};
    EXPECT_THROW(
        apps::wide_to_utf8(std::wstring_view(bad_wide.data(), bad_wide.size())),
        std::runtime_error);
}

TEST(Paths, ProcessUtf8ArgsAndProcessParsersDecodeLiveCommandLine) {
    EXPECT_FALSE(apps::exe_dir().empty());

    const std::vector<std::string> live_args = apps::process_utf8_args();
    for (const auto& token : live_args) {
        EXPECT_FALSE(token.empty());
    }

    std::string dir_val;
    std::string game_val;
    apps::po::options_description desc("live_common");
    desc.add_options()
        ("help,h", "help")
        ("dir", apps::po::value(&dir_val)->default_value("C:\\枕\\櫻の詩 -Sakura no Uta-"))
        ("game-dir", apps::po::value(&game_val)->default_value("C:\\櫻の詩"));
    RegisterLiveProcessGtestFlags(desc, live_args);

    apps::po::variables_map vm;
    const auto rc = apps::parse_process_command_line(desc, vm);
    EXPECT_FALSE(rc.has_value());
    EXPECT_EQ(dir_val, "C:\\枕\\櫻の詩 -Sakura no Uta-");
    EXPECT_EQ(game_val, "C:\\櫻の詩");

    std::string proj_only_dir;
    apps::po::options_description proj_desc("live_project_only");
    proj_desc.add_options()
        ("help,h", "help")
        ("dir", apps::po::value(&proj_only_dir)->default_value("C:\\櫻の詩"));
    RegisterLiveProcessGtestFlags(proj_desc, live_args);

    apps::po::variables_map proj_vm;
    const auto proj_rc = apps::parse_process_project_only_command_line(proj_desc, proj_vm);
    EXPECT_FALSE(proj_rc.has_value());
    EXPECT_EQ(proj_only_dir, "C:\\櫻の詩");
}

TEST(Paths, ParseCommandLineVectorAndArgvOverloadsValidateOptionsAndRejectLeadingExe) {
    std::string project;
    std::string game;
    apps::po::options_description desc("test_cli");
    desc.add(apps::common_options(project, game));

    {
        apps::po::variables_map vm;
        const auto rc = apps::parse_command_line(
            std::vector<std::string>{"--help"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitOk);
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunArgvParse({"test_cli", "--help"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitHelp);
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunArgvParse({"test_cli", "--dir", "D:\\Project"}, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitFailure);
    }

    // Passing argv[0] into the vector overload must fail as an unrecognized positional token.
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
        const auto rc = apps::parse_command_line(
            std::vector<std::string>{
                "--dir", "C:\\枕\\櫻の詩 -Sakura no Uta-",
                "--game-dir", "E:\\櫻の詩"},
            desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, "C:\\枕\\櫻の詩 -Sakura no Uta-");
        EXPECT_EQ(game, "E:\\櫻の詩");
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunArgvParse(
            {"test_cli", "--dir", "D:\\Project", "--game-dir", "E:\\Game"}, desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, "D:\\Project");
        EXPECT_EQ(game, "E:\\Game");
    }
}

TEST(Paths, ProjectOnlyOptionsAcceptsDirAndRejectsGameDir) {
    std::string project;
    apps::po::options_description desc("test_project_only");
    desc.add(apps::project_only_options(project));

    {
        apps::po::variables_map vm;
        const auto rc = apps::parse_project_only_command_line(
            std::vector<std::string>{"--dir", "D:\\Project", "--game-dir", "E:\\Game"},
            desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitFailure);
    }

    {
        apps::po::variables_map vm;
        const auto rc = RunProjectOnlyArgvParse(
            {"test_project_only", "--dir", "D:\\Project", "--game-dir", "E:\\Game"},
            desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }

    {
        apps::po::variables_map vm;
        const auto rc = apps::parse_project_only_command_line(
            std::vector<std::string>{"--dir", "C:\\枕\\櫻の詩 -Sakura no Uta-"},
            desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, "C:\\枕\\櫻の詩 -Sakura no Uta-");
    }

    std::string bad_proj;
    std::string bad_game;
    apps::po::options_description wrong_desc = apps::common_options(bad_proj, bad_game);
    apps::po::variables_map wrong_vm;
    EXPECT_THROW(
        apps::parse_project_only_command_line(
            std::vector<std::string>{"--dir", "D:\\Project"}, wrong_desc, wrong_vm),
        std::logic_error);
}

TEST(Paths, CommandLineToArgvWRoundTripsTrailingBackslashesQuotesAndJapanesePaths) {
    EXPECT_THROW(apps::format_windows_command_line("C:\\bad\"exe.exe", {}),
                 std::runtime_error);
    EXPECT_TRUE(apps::split_windows_command_line("").empty());

    // Quoted argv[0] in CommandLineToArgvW does NOT unescape \\, so trailing
    // backslashes on exe_path must NOT be doubled.
    {
        const std::string exe = "C:\\dir\\";
        const std::vector<std::string> args = {
            "--dir", "C:\\dir\\", "--game-dir", "C:\\枕\\櫻の詩 -Sakura no Uta-\\"};
        const std::string cmd = apps::format_windows_command_line(exe, args);
        const std::vector<std::string> tokens = apps::split_windows_command_line(cmd);
        ASSERT_EQ(tokens.size(), 5u);
        EXPECT_EQ(tokens[0], exe);
        EXPECT_EQ(tokens[1], "--dir");
        EXPECT_EQ(tokens[2], "C:\\dir\\");
        EXPECT_EQ(tokens[3], "--game-dir");
        EXPECT_EQ(tokens[4], "C:\\枕\\櫻の詩 -Sakura no Uta-\\");
    }

    {
        const std::string exe = "C:\\枕\\櫻の詩\\";
        const std::vector<std::string> args = {
            "",
            "plain_token",
            "with spaces and \t tabs",
            "say \"櫻の詩\"",
            "trail\\\\",
            "slash_before_quote\\\"end",
            "double_slash_before_quote\\\\\"end"};
        const std::string cmd = apps::format_windows_command_line(exe, args);
        const std::vector<std::string> tokens = apps::split_windows_command_line(cmd);
        ASSERT_EQ(tokens.size(), args.size() + 1);
        EXPECT_EQ(tokens[0], exe);
        for (std::size_t i = 0; i < args.size(); ++i) {
            EXPECT_EQ(tokens[i + 1], args[i]) << "arg index " << i;
        }
    }
}

TEST(Paths, RunProcessHandlesZeroNonZeroJapaneseWorkingDirAndMissingExe) {
    const std::string cmd_exe = SystemCmdExeUtf8();

    EXPECT_EQ(apps::run_process(cmd_exe, {"/c", "exit", "0"}), apps::kExitOk);
    EXPECT_EQ(apps::run_process(cmd_exe, {"/c", "exit", "7"}),
              kNonZeroChildExitCode);

    ScopedTempDir temp("vn_ut_run_process_櫻の詩");
    const fs::path jp_cwd = temp.path() / fs::u8path("枕\\櫻の詩 -Sakura no Uta-");
    WriteDummyFile(jp_cwd / "marker.txt");
    const int cwd_rc = apps::run_process(
        cmd_exe,
        {"/c", "if exist marker.txt (exit 0) else (exit 3)"},
        jp_cwd.u8string());
    EXPECT_NE(cwd_rc, kMissingCwdMarkerExitCode);
    EXPECT_EQ(cwd_rc, apps::kExitOk);

    const std::string missing_exe =
        (temp.path() / fs::u8path("櫻の詩_nonexistent_child.exe")).u8string();
    EXPECT_THROW(apps::run_process(missing_exe, {"--help"}), std::runtime_error);
}
