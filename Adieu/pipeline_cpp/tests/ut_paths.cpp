// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: two-pass UTF-8 project-root discovery
// across checkout and staged release layouts, UTF-8 CLI option parsing, and
// UTF-16 CreateProcessW / CommandLineToArgvW command-line formatting.
#include "apps/paths.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
namespace apps = adi::apps;

namespace {

constexpr int kNestedEmptyLevels = 8;
constexpr int kFirstNestedLevel = 1;
constexpr int kMinParsedTokens = 1;
constexpr int kFirstArgIndex = 1;
constexpr std::size_t kFirstParsedElement = 0;
constexpr std::size_t kInvalidUtf8Len = 8;
constexpr DWORD kZeroModuleLen = 0U;
constexpr DWORD kModulePathBufChars = 32768U;

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
            ::LocalFree(argv_);
        }
    }

    LPWSTR* get() const noexcept { return argv_; }

private:
    LPWSTR* argv_;
};

std::string WideToUtf8ForTest(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("WideCharToMultiByte size failed in test");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        throw std::runtime_error("WideCharToMultiByte conversion failed in test");
    }
    return out;
}

// Parses a formatted UTF-16 command line via CommandLineToArgvW and returns
// {argv[0], argv[1], ...} transcoded to UTF-8.
std::vector<std::string> ParseViaCommandLineToArgvW(const std::wstring& cmd_line) {
    int argc = 0;
    const ScopedLocalArgv argv_w(::CommandLineToArgvW(cmd_line.c_str(), &argc));
    if (argv_w.get() == nullptr || argc < kMinParsedTokens) {
        throw std::runtime_error("CommandLineToArgvW failed in test");
    }
    std::vector<std::string> tokens;
    tokens.reserve(static_cast<std::size_t>(argc));
    tokens.push_back(WideToUtf8ForTest(argv_w.get()[kFirstParsedElement]));
    for (int i = kFirstArgIndex; i < argc; ++i) {
        tokens.push_back(WideToUtf8ForTest(argv_w.get()[i]));
    }
    return tokens;
}

// Pads `base` with `kNestedEmptyLevels` empty ancestor directories so the
// bounded ancestor walk cannot escape into any outer directory above `%TEMP%`.
fs::path CreateNestedEmptyPad(const fs::path& base) {
    fs::path cur = base;
    for (int level = kFirstNestedLevel; level <= kNestedEmptyLevels; ++level) {
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
    const ScopedTempDir temp("vn_ut_paths_adieu");
    const fs::path root = CreateNestedEmptyPad(temp.path()) / "release_root";
    fs::create_directories(root / "bin");
    fs::create_directories(root / "game");
    ASSERT_FALSE(fs::exists(root / "script_output"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, CheckoutPriorityBeatsNestedBuildInstallStaging) {
    const ScopedTempDir temp("vn_ut_paths_adieu");
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
    const ScopedTempDir temp("vn_ut_paths_adieu");
    const fs::path root = CreateNestedEmptyPad(temp.path()) / "legacy_root";
    fs::create_directories(root / "bin");
    fs::create_directories(root / "script_output");
    ASSERT_FALSE(fs::exists(root / "game"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, JapaneseDirectoryDiscoveryReturnsUtf8ForCheckoutAndRelease) {
    const ScopedTempDir temp("vn_ut_paths_adieu_jp");
    const fs::path pad = CreateNestedEmptyPad(temp.path());

    const fs::path jp_checkout = pad / fs::u8path("日本語_チェックアウト");
    fs::create_directories(jp_checkout / "pipeline_cpp");
    WriteDummyBuildPy(jp_checkout);
    const fs::path staged_bin = jp_checkout / "build" / "install" / "bin";
    fs::create_directories(staged_bin);
    fs::create_directories(jp_checkout / "build" / "install" / "game");

    const std::string found_checkout = apps::find_project_dir_from(staged_bin);
    EXPECT_EQ(found_checkout, jp_checkout.u8string());
    EXPECT_NE(found_checkout.find("日本語_チェックアウト"), std::string::npos);
    EXPECT_EQ(fs::u8path(found_checkout), jp_checkout);

    const fs::path jp_release = pad / fs::u8path("日本語_リリース");
    fs::create_directories(jp_release / "bin");
    fs::create_directories(jp_release / "game");

    const std::string found_release = apps::find_project_dir_from(jp_release / "bin");
    EXPECT_EQ(found_release, jp_release.u8string());
    EXPECT_NE(found_release.find("日本語_リリース"), std::string::npos);
    EXPECT_EQ(fs::u8path(found_release), jp_release);
}

TEST(Paths, UnrelatedDeepDirectoryReturnsEmpty) {
    const ScopedTempDir temp("vn_ut_paths_adieu");
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

    std::string standalone_project;
    apps::po::options_description standalone_desc("standalone_cli");
    standalone_desc.add(apps::project_only_options(standalone_project));

    {
        apps::po::variables_map vm;
        const char* argv[] = {"standalone_cli", "--dir", "D:\\StandaloneProject"};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), standalone_desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(standalone_project, "D:\\StandaloneProject");
    }

    {
        apps::po::variables_map vm;
        const char* argv[] = {
            "standalone_cli", "--dir", "D:\\StandaloneProject", "--game-dir", "E:\\Game"};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), standalone_desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }
}

TEST(Paths, ParseCommandLinePreservesUtf8JapaneseDirectories) {
    std::string project;
    std::string game;
    apps::po::options_description desc("test_utf8_cli");
    desc.add(apps::common_options(project, game));

    apps::po::variables_map vm;
    const char* argv[] = {
        "test_utf8_cli",
        "--dir",
        "D:\\日本語プロジェクト",
        "--game-dir",
        "E:\\ゲーム本体\\Adieu",
    };
    const auto rc = apps::parse_command_line(
        static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
    EXPECT_FALSE(rc.has_value());
    EXPECT_EQ(project, "D:\\日本語プロジェクト");
    EXPECT_EQ(game, "E:\\ゲーム本体\\Adieu");
}

TEST(Paths, ProcessUtf8ArgsAndExeDirExecuteCleanly) {
    const std::string dir = apps::exe_dir();
    EXPECT_FALSE(dir.empty());
    EXPECT_TRUE(fs::exists(fs::u8path(dir)));

    std::vector<std::string> live_args;
    EXPECT_NO_THROW(live_args = apps::process_utf8_args());
    for (const std::string& arg : live_args) {
        EXPECT_FALSE(arg.empty());
    }
}

TEST(Paths, BuildCommandLineUtf16RoundTripsThroughCommandLineToArgvW) {
    const std::string exe_with_spaces_and_jp =
        "D:\\日本語 ツール\\bin\\01_extract.exe";
    const std::vector<std::string> args = {
        "--dir",
        "D:\\日本語 プロジェクト\\末尾スラッシュ\\",
        "--game-dir",
        "E:\\ゲーム本体\\Adieu\\",
        "quoted \"こんにちは\" value",
        "C:\\dir\\\"inner quote\"\\trail\\\\",
        "",
        "plain_token",
    };

    const std::wstring cmd_line =
        apps::build_command_line_utf16(exe_with_spaces_and_jp, args);
    const std::vector<std::string> parsed = ParseViaCommandLineToArgvW(cmd_line);

    std::vector<std::string> expected;
    expected.push_back(exe_with_spaces_and_jp);
    expected.insert(expected.end(), args.begin(), args.end());
    EXPECT_EQ(parsed, expected);
}

TEST(Paths, BuildCommandLineUtf16DoesNotDoubleTrailingBackslashInArgv0) {
    const std::string exe_trailing_backslash = "D:\\日本語 Tools\\bin\\";
    const std::vector<std::string> args = {"--help"};

    const std::wstring cmd_line =
        apps::build_command_line_utf16(exe_trailing_backslash, args);
    EXPECT_EQ(cmd_line.find(L"\\\\\""), std::wstring::npos);

    const std::vector<std::string> parsed = ParseViaCommandLineToArgvW(cmd_line);
    ASSERT_EQ(parsed.size(), args.size() + static_cast<std::size_t>(kMinParsedTokens));
    EXPECT_EQ(parsed[kFirstParsedElement], exe_trailing_backslash);
    EXPECT_EQ(parsed[static_cast<std::size_t>(kFirstArgIndex)], "--help");
}

TEST(Paths, BuildCommandLineUtf16RejectsMalformedExecutableOrInvalidUtf8) {
    EXPECT_THROW(apps::build_command_line_utf16("", {"--help"}), std::invalid_argument);
    EXPECT_THROW(
        apps::build_command_line_utf16("D:\\bad\"quote\\tool.exe", {"--help"}),
        std::invalid_argument);

    const std::string bad_utf8("bad\xFFutf8", kInvalidUtf8Len);
    EXPECT_THROW(apps::build_command_line_utf16(bad_utf8, {}), std::runtime_error);
    EXPECT_THROW(
        apps::build_command_line_utf16("D:\\tool.exe", {bad_utf8}),
        std::runtime_error);
}

TEST(Paths, RunProcessUtf8ExecutesChildAndReportsMissingExecutable) {
    std::wstring module_buf(kModulePathBufChars, L'\0');
    const DWORD module_len = ::GetModuleFileNameW(
        nullptr, module_buf.data(), static_cast<DWORD>(module_buf.size()));
    ASSERT_GT(module_len, kZeroModuleLen);
    ASSERT_LT(module_len, static_cast<DWORD>(module_buf.size()));
    module_buf.resize(module_len);
    const std::string self_exe = WideToUtf8ForTest(module_buf);

    const int rc = apps::run_process_utf8(
        self_exe,
        {"--gtest_list_tests", "--gtest_filter=Paths.UnrelatedDeepDirectoryReturnsEmpty"});
    EXPECT_EQ(rc, apps::kExitOk);

    const ScopedTempDir temp("vn_ut_paths_missing_日本語");
    const std::string missing_exe =
        (temp.path() / fs::u8path("存在しない.exe")).u8string();
    EXPECT_THROW(apps::run_process_utf8(missing_exe, {"--help"}), std::runtime_error);
}
