// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: UTF-8 project-root discovery across
// checkout and staged release layouts (including Japanese directory names),
// CLI option parsing, wide argument extraction, and CreateProcessW UTF-16
// command-line building.
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
namespace apps = crc::apps;

namespace {

constexpr int kNestedEmptyLevels = 8;
constexpr std::size_t kBufferGrowthFactor = 2;
constexpr unsigned char kInvalidUtf8LeadByte = 0xFF;

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

class LocalFreeGuard {
public:
    explicit LocalFreeGuard(void* ptr) noexcept : ptr_(ptr) {}
    LocalFreeGuard(const LocalFreeGuard&) = delete;
    LocalFreeGuard& operator=(const LocalFreeGuard&) = delete;

    ~LocalFreeGuard() {
        if (ptr_ != nullptr) {
            ::LocalFree(ptr_);
        }
    }

private:
    void* ptr_;
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

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        throw std::runtime_error("WideCharToMultiByte mismatch");
    }
    return out;
}

std::vector<std::string> SplitCommandLineUtf16ToUtf8(const std::wstring& cmd) {
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(cmd.c_str(), &argc);
    const LocalFreeGuard guard(argv);
    if (argv == nullptr) {
        throw std::runtime_error("CommandLineToArgvW returned null");
    }
    std::vector<std::string> tokens;
    tokens.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        tokens.push_back(WideToUtf8(argv[i]));
    }
    return tokens;
}

std::string CurrentExecutableUtf8() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(),
                                             static_cast<DWORD>(buf.size()));
        if (n == 0) {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * kBufferGrowthFactor);
    }
    return fs::path(buf).u8string();
}

}  // namespace

TEST(Paths, ReleaseZipLayoutResolvesRootWithoutScriptOutput) {
    const ScopedTempDir temp("vn_ut_paths_cross_channel");
    const fs::path root = CreateNestedEmptyPad(temp.path()) / "release_root";
    fs::create_directories(root / "bin");
    fs::create_directories(root / "game");
    ASSERT_FALSE(fs::exists(root / "script_output"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, CheckoutPriorityBeatsNestedBuildInstallStaging) {
    const ScopedTempDir temp("vn_ut_paths_cross_channel");
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
    const ScopedTempDir temp("vn_ut_paths_cross_channel");
    const fs::path root = CreateNestedEmptyPad(temp.path()) / "legacy_root";
    fs::create_directories(root / "bin");
    fs::create_directories(root / "script_output");
    ASSERT_FALSE(fs::exists(root / "game"));

    const std::string found = apps::find_project_dir_from(root / "bin");
    EXPECT_EQ(fs::u8path(found), root);
}

TEST(Paths, JapaneseCheckoutAndReleaseLayoutsResolveUtf8Root) {
    const ScopedTempDir temp("vn_ut_paths_cross_channel_jp");

    const fs::path checkout =
        CreateNestedEmptyPad(temp.path()) / fs::u8path("日本語_チェックアウト");
    fs::create_directories(checkout / "pipeline_cpp");
    WriteDummyBuildPy(checkout);
    const fs::path staged_bin = checkout / "build" / "install" / "bin";
    fs::create_directories(staged_bin);
    fs::create_directories(checkout / "build" / "install" / "game");

    const std::string found_checkout = apps::find_project_dir_from(staged_bin);
    EXPECT_EQ(found_checkout, checkout.u8string());
    EXPECT_EQ(fs::u8path(found_checkout), checkout);
    EXPECT_NE(found_checkout.find("日本語_チェックアウト"), std::string::npos);

    const fs::path release =
        CreateNestedEmptyPad(temp.path()) / fs::u8path("日本語_リリース");
    fs::create_directories(release / "bin");
    fs::create_directories(release / "game");

    const std::string found_release = apps::find_project_dir_from(release / "bin");
    EXPECT_EQ(found_release, release.u8string());
    EXPECT_EQ(fs::u8path(found_release), release);
    EXPECT_NE(found_release.find("日本語_リリース"), std::string::npos);
}

TEST(Paths, UnrelatedDeepDirectoryReturnsEmpty) {
    const ScopedTempDir temp("vn_ut_paths_cross_channel");
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
        const char* argv[] = {"test_cli", "--dir", "X:\\vn_project"};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }

    {
        apps::po::variables_map vm;
        const char* argv[] = {
            "test_cli", "--dir", "X:\\vn_project", "--game-dir", "Y:\\vn_game", "stray_token"};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, apps::kExitError);
    }

    {
        apps::po::variables_map vm;
        const char* argv[] = {"test_cli", "--dir", "X:\\vn_project", "--game-dir", "Y:\\vn_game"};
        const auto rc = apps::parse_command_line(
            static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, "X:\\vn_project");
        EXPECT_EQ(game, "Y:\\vn_game");
    }
}

TEST(Paths, ParseCommandLinePreservesUtf8JapanesePaths) {
    std::string project;
    std::string game;
    apps::po::options_description desc("test_cli_utf8");
    desc.add(apps::common_options(project, game));

    apps::po::variables_map vm;
    const char* argv[] = {
        "test_cli_utf8",
        "--dir",
        "X:\\日本語_プロジェクト\\CROSS_CHANNEL",
        "--game-dir",
        "Y:\\ゲーム\\CROSS†CHANNEL",
    };
    const auto rc = apps::parse_command_line(
        static_cast<int>(std::size(argv)), const_cast<char**>(argv), desc, vm);
    EXPECT_FALSE(rc.has_value());
    EXPECT_EQ(project, "X:\\日本語_プロジェクト\\CROSS_CHANNEL");
    EXPECT_EQ(game, "Y:\\ゲーム\\CROSS†CHANNEL");
}

TEST(Paths, GeminiArtifactPathsPointUnconditionallyToGeminiCacheWithZeroLegacyFallback) {
    const ScopedTempDir temp("vn_ut_paths_gemini_layout");
    const fs::path proj_path = temp.path() / fs::u8path("日本語_CROSS_CHANNEL");
    const fs::path game_path = temp.path() / fs::u8path("ゲーム_install");
    fs::create_directories(proj_path / "script_output");
    fs::create_directories(game_path / "data");

    const std::string proj = proj_path.u8string();
    const std::string game = game_path.u8string();
    const std::string expected_gemini_cache =
        proj + "\\script_output\\translation_cache_gemini.json";

    // Clean slate: resolves unconditionally to translation_cache_gemini.json.
    EXPECT_EQ(apps::cache_json(proj), expected_gemini_cache);
    EXPECT_EQ(apps::extracted_json(proj), proj + "\\script_output\\extracted_text.json");
    EXPECT_EQ(apps::blocked_lines_json(proj), proj + "\\script_output\\blocked_lines.json");
    EXPECT_EQ(apps::raw_answers_log(proj), proj + "\\script_output\\gemini_raw_answers.log");
    EXPECT_EQ(apps::last_batch_json(proj),
              proj + "\\script_output\\last_gemini_translate\\last_translation.json");
    EXPECT_EQ(apps::translated_json(proj), proj + "\\script_output\\translated_text.json");
    EXPECT_EQ(apps::out_tsv(proj), proj + "\\build\\install\\game\\translations.tsv");
    EXPECT_EQ(apps::sn_bin(game), game + "\\data\\sn.bin");
    EXPECT_EQ(apps::game_tsv(game), game + "\\translations.tsv");

    // Even if ONLY a legacy cache file exists on disk (and Gemini cache is absent),
    // cache_json MUST still point unconditionally to translation_cache_gemini.json.
    {
        std::ofstream legacy(proj_path / "script_output" / "translation_cache_anthropic.json",
                             std::ios::binary);
        legacy << "{\"太一\": \"Taichi\"}";
    }
    ASSERT_FALSE(fs::exists(proj_path / "script_output" / "translation_cache_gemini.json"));
    EXPECT_EQ(apps::cache_json(proj), expected_gemini_cache);
}

TEST(Paths, ProcessUtf8ArgsExecutesAndSkipsArgvZero) {
    const std::string self_exe = CurrentExecutableUtf8();
    std::vector<std::string> args;
    ASSERT_NO_THROW(args = apps::process_utf8_args());
    for (const std::string& arg : args) {
        EXPECT_NE(arg, self_exe);
    }
}

TEST(Paths, BuildCommandLineUtf16RoundTripsThroughCommandLineToArgvW) {
    const std::string exe_path = "X:\\日本語 フォルダ\\bin\\01_extract.exe";
    const std::vector<std::string> args = {
        "--dir",
        "X:\\日本語 プロジェクト\\CROSS CHANNEL\\",
        "--game-dir",
        "Y:\\Games\\CROSS†CHANNEL\\\\",
        "plain_no_spaces\\",
        "",
        "say \"hello\" world",
        "trailing_backslash_before_quote\\\"end",
        "double_backslash_before_quote\\\\\"end",
    };

    const std::wstring cmd = apps::build_command_line_utf16(exe_path, args);
    const std::vector<std::string> parsed = SplitCommandLineUtf16ToUtf8(cmd);

    ASSERT_EQ(parsed.size(), args.size() + 1u);
    EXPECT_EQ(parsed[0], exe_path);
    for (std::size_t i = 0; i < args.size(); ++i) {
        EXPECT_EQ(parsed[i + 1u], args[i]) << "arg index " << i;
    }
}

TEST(Paths, BuildCommandLineUtf16DoesNotDoubleTrailingBackslashOnArgvZero) {
    const std::string exe_with_slash = "X:\\Tools\\";
    const std::vector<std::string> args = {"--game-dir", "Y:\\My Game\\"};

    const std::wstring cmd = apps::build_command_line_utf16(exe_with_slash, args);
    EXPECT_EQ(cmd.rfind(L"\"X:\\Tools\\\"", 0), 0u);

    const std::vector<std::string> parsed = SplitCommandLineUtf16ToUtf8(cmd);
    ASSERT_EQ(parsed.size(), 3u);
    EXPECT_EQ(parsed[0], exe_with_slash);
    EXPECT_EQ(parsed[1], "--game-dir");
    EXPECT_EQ(parsed[2], "Y:\\My Game\\");
}

TEST(Paths, BuildCommandLineUtf16RejectsInvalidExePathAndBadUtf8) {
    EXPECT_THROW(apps::build_command_line_utf16("", {}), std::invalid_argument);
    EXPECT_THROW(apps::build_command_line_utf16("X:\\bad\"exe.exe", {}),
                 std::invalid_argument);

    const char invalid_utf8[] = {static_cast<char>(kInvalidUtf8LeadByte), '\0'};
    EXPECT_THROW(apps::build_command_line_utf16("X:\\valid.exe", {std::string(invalid_utf8)}),
                 std::runtime_error);
}

TEST(Paths, RunProcessUtf8SpawnsExecutableAndReturnsExitCode) {
    const std::string self_exe = CurrentExecutableUtf8();
    EXPECT_EQ(apps::run_process_utf8(self_exe, {"--help"}), apps::kExitHelp);
}
