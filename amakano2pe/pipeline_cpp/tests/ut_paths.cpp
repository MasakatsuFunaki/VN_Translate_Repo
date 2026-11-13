// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: two-pass UTF-8 project-root discovery
// across checkout and staged release layouts (including Japanese/fullwidth
// vendor paths), UTF-8 CLI option parsing, and wide Win32 process quoting and
// execution.
#include "apps/paths.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace apps = ama::apps;

namespace {

constexpr int kNestedEmptyLevels = 8;
constexpr int kZeroExitCode = 0;
constexpr int kCustomChildExitCode = 7;

constexpr const char kJapaneseDirSegment[] =
    "あざらしそふと_アマカノ2～Perfect Edition～";

constexpr const char kJapaneseGameDir[] =
    "C:\\あざらしそふと\\アマカノ2～Perfect Edition～";

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

std::string ResolveCmdExeUtf8() {
    if (const wchar_t* sys_root = _wgetenv(L"SystemRoot");
        sys_root != nullptr && *sys_root != L'\0') {
        const fs::path candidate = fs::path(sys_root) / "System32" / "cmd.exe";
        if (fs::exists(candidate)) {
            return candidate.u8string();
        }
    }
    return "C:\\Windows\\System32\\cmd.exe";
}

}  // namespace

TEST(Paths, ReleaseZipLayoutResolvesRootWithoutScriptOutput) {
    const ScopedTempDir temp("vn_ut_paths_amakano2pe");
    const fs::path root = CreateNestedEmptyPad(temp.path()) / "release_root";
    fs::create_directories(root / "bin");
    fs::create_directories(root / "game");
    ASSERT_FALSE(fs::exists(root / "script_output"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, CheckoutPriorityBeatsNestedBuildInstallStaging) {
    const ScopedTempDir temp("vn_ut_paths_amakano2pe");
    const fs::path checkout = CreateNestedEmptyPad(temp.path()) / "checkout_root";
    fs::create_directories(checkout / "pipeline_cpp");
    WriteDummyBuildPy(checkout);

    const fs::path staged_bin = checkout / "build" / "install" / "bin";
    const fs::path staged_game = checkout / "build" / "install" / "game";
    fs::create_directories(staged_bin);
    fs::create_directories(staged_game);

    const std::string found = apps::find_project_dir_from(staged_bin);
    EXPECT_EQ(fs::u8path(found), checkout);
}

TEST(Paths, LegacyStagedLayoutWithScriptOutputResolvesRoot) {
    const ScopedTempDir temp("vn_ut_paths_amakano2pe");
    const fs::path root = CreateNestedEmptyPad(temp.path()) / "legacy_root";
    fs::create_directories(root / "bin");
    fs::create_directories(root / "script_output");
    ASSERT_FALSE(fs::exists(root / "game"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, JapaneseFullwidthDirectoryResolvesAsValidUtf8) {
    const ScopedTempDir temp("vn_ut_paths_amakano2pe_jp");
    const fs::path pad = CreateNestedEmptyPad(temp.path());

    const fs::path checkout = pad / fs::u8path(kJapaneseDirSegment) / "checkout";
    fs::create_directories(checkout / "pipeline_cpp");
    WriteDummyBuildPy(checkout);

    const std::string found_checkout =
        apps::find_project_dir_from(checkout / "pipeline_cpp");
    ASSERT_FALSE(found_checkout.empty());
    EXPECT_NE(found_checkout.find(kJapaneseDirSegment), std::string::npos);
    EXPECT_EQ(fs::u8path(found_checkout), checkout);
    EXPECT_TRUE(fs::exists(fs::u8path(found_checkout)));

    const fs::path staged = pad / fs::u8path(kJapaneseDirSegment) / "release";
    fs::create_directories(staged / "bin");
    fs::create_directories(staged / "game");

    const std::string found_staged = apps::find_project_dir_from(staged / "bin");
    ASSERT_FALSE(found_staged.empty());
    EXPECT_NE(found_staged.find(kJapaneseDirSegment), std::string::npos);
    EXPECT_EQ(fs::u8path(found_staged), staged);
    EXPECT_TRUE(fs::exists(fs::u8path(found_staged)));
}

TEST(Paths, UnrelatedDeepDirectoryReturnsEmpty) {
    const ScopedTempDir temp("vn_ut_paths_amakano2pe");
    const fs::path deep = CreateNestedEmptyPad(temp.path());

    EXPECT_TRUE(apps::find_project_dir_from(deep).empty());
    EXPECT_TRUE(apps::find_project_dir_from(fs::path{}).empty());
}

TEST(Paths, ParseCommandLineHandlesHelpAndOptions) {
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
        const char* argv[] = {"test_cli", "--dir", "D:\\Project", "--game-dir", "E:\\Game"};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, "D:\\Project");
        EXPECT_EQ(game, "E:\\Game");
    }
}

TEST(Paths, ParseCommandLineVectorPreservesJapaneseUtf8AndRejectsLeadingProgramName) {
    std::string project;
    std::string game;
    apps::po::options_description desc("test_cli");
    desc.add(apps::common_options(project, game));

    const std::string jp_proj = std::string("D:\\") + kJapaneseDirSegment;
    const std::string jp_game = kJapaneseGameDir;

    {
        apps::po::variables_map vm;
        const std::vector<std::string> args = {
            "--dir", jp_proj, "--game-dir", jp_game};
        const auto rc = apps::parse_command_line(args, desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, jp_proj);
        EXPECT_EQ(game, jp_game);
        EXPECT_EQ(fs::u8path(game).u8string(), jp_game);
    }

    // Passing argv[0] inside the vector overload must fail as an unexpected
    // positional argument because Boost's vector parser treats element 0 as the
    // first option token rather than the executable name.
    {
        apps::po::variables_map vm;
        const std::vector<std::string> unstripped_args = {
            "01_extract.exe", "--dir", jp_proj, "--game-dir", jp_game};
        const auto rc = apps::parse_command_line(unstripped_args, desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }
}

TEST(Paths, ProcessUtf8ArgsExcludesProgramNameAndParsesCleanly) {
    const std::vector<std::string> proc_args = apps::process_utf8_args();
    for (const auto& token : proc_args) {
        EXPECT_EQ(token.find("ut_paths"), std::string::npos);
    }

    apps::po::options_description desc("ut_paths_proc");
    std::string dummy_dir = "D:\\DefaultDir";
    std::string dummy_game = kJapaneseGameDir;
    desc.add_options()
        ("help,h", "help")
        ("dir", apps::po::value(&dummy_dir)->default_value(dummy_dir), "dir")
        ("game-dir", apps::po::value(&dummy_game)->default_value(dummy_game), "game-dir");

    // Register any `--gtest_*` flags forwarded by ctest so
    // parse_process_command_line exercises GetCommandLineW() directly.
    constexpr std::string_view kGtestPrefix = "--gtest_";
    constexpr std::size_t kLongFlagPrefixLen = 2;
    for (const auto& token : proc_args) {
        if (token.rfind(kGtestPrefix.data(), 0) != 0) {
            continue;
        }
        const std::size_t eq = token.find('=');
        const std::string opt_name =
            (eq == std::string::npos)
                ? token.substr(kLongFlagPrefixLen)
                : token.substr(kLongFlagPrefixLen, eq - kLongFlagPrefixLen);
        if (desc.find_nothrow(opt_name, false) == nullptr) {
            desc.add_options()
                (opt_name.c_str(), apps::po::value<std::string>()->implicit_value(""), "gtest");
        }
    }

    apps::po::variables_map vm;
    const auto rc = apps::parse_process_command_line(desc, vm);
    EXPECT_FALSE(rc.has_value());
    EXPECT_EQ(dummy_game, kJapaneseGameDir);
}

TEST(Paths, WindowsCommandLineQuotingRoundTripsJapaneseSpacesBackslashesAndQuotes) {
    const std::string exe =
        std::string("D:\\") + kJapaneseDirSegment + "\\bin\\01_extract.exe";
    const std::vector<std::string> args = {
        "--dir",
        std::string("D:\\Projects\\") + kJapaneseDirSegment + "\\",
        "--game-dir",
        std::string(kJapaneseGameDir) + "\\",
        "--note",
        "quoted \"inner\" value\\end\\",
        "",
    };

    const std::string formatted = apps::format_windows_command_line(exe, args);
    const std::vector<std::string> round_trip =
        apps::split_windows_command_line(formatted);

    ASSERT_EQ(round_trip.size(), args.size() + 1);
    EXPECT_EQ(round_trip[0], exe);
    for (std::size_t i = 0; i < args.size(); ++i) {
        EXPECT_EQ(round_trip[i + 1], args[i]);
    }
}

TEST(Paths, WindowsCommandLineQuotingPreservesTrailingBackslashOnExeAndArgs) {
    const std::vector<std::string> exe_cases = {
        "C:\\dir\\",
        "C:\\あざらしそふと\\bin\\",
    };
    const std::vector<std::string> args = {
        "--dir",
        "C:\\dir\\",
        "--game-dir",
        std::string(kJapaneseGameDir) + "\\",
        "--spaced",
        "C:\\spaced dir\\",
    };

    for (const auto& exe : exe_cases) {
        const std::string formatted = apps::format_windows_command_line(exe, args);
        const std::vector<std::string> round_trip =
            apps::split_windows_command_line(formatted);

        ASSERT_EQ(round_trip.size(), args.size() + 1);
        EXPECT_EQ(round_trip[0], exe);
        for (std::size_t i = 0; i < args.size(); ++i) {
            EXPECT_EQ(round_trip[i + 1], args[i]);
        }
    }
}

TEST(Paths, FormatWindowsCommandLineRejectsDoubleQuotesInExePath) {
    EXPECT_THROW(
        apps::format_windows_command_line("C:\\bad\"dir\\01_extract.exe", {"--help"}),
        std::runtime_error);
}

TEST(Paths, RunProcessPropagatesExitCodeAndThrowsOnMissingExecutable) {
    const std::string cmd_exe = ResolveCmdExeUtf8();
    ASSERT_TRUE(fs::exists(fs::u8path(cmd_exe)));

    EXPECT_EQ(
        apps::run_process(cmd_exe, {"/c", "exit", std::to_string(kZeroExitCode)}),
        kZeroExitCode);
    EXPECT_EQ(apps::run_process(
                  cmd_exe, {"/c", "exit", std::to_string(kCustomChildExitCode)}),
              kCustomChildExitCode);

    const std::string missing_exe =
        (fs::temp_directory_path() / fs::u8path(kJapaneseDirSegment) /
         "nonexistent_binary.exe")
            .u8string();
    EXPECT_THROW(apps::run_process(missing_exe, {}), std::runtime_error);
}
