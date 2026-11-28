// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "common/win32_cli_env.h"
#include "common/utf8_text.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace spot_checker {
namespace {

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<std::uint64_t> counter{0};
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::uint64_t id = counter.fetch_add(1U, std::memory_order_relaxed);
        path_ = std::filesystem::temp_directory_path() /
                ("spot_checker_ut_env_" + std::to_string(tick) + "_" +
                 std::to_string(id));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

    void touch_file(const std::filesystem::path& relative_path) const {
        const std::filesystem::path full = path_ / relative_path;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out << "# test\n";
    }

private:
    std::filesystem::path path_;
};

TEST(Win32CliEnv, ResolvesRepoRootAndGameDirInsideHermeticTempTree) {
    ScopedTempDir sandbox;
    const std::filesystem::path isolated = sandbox.path() / "isolated";
    std::filesystem::create_directories(isolated / "tools" / "spot_checker");
    EXPECT_FALSE(is_repo_root_dir(isolated));

    const std::filesystem::path repo_a = sandbox.path() / "repo_cmake";
    sandbox.touch_file("repo_cmake/tools/spot_checker/CMakeLists.txt");
    EXPECT_TRUE(is_repo_root_dir(repo_a));
    EXPECT_FALSE(is_repo_root_dir(repo_a / "tools"));

    const std::filesystem::path repo_b = sandbox.path() / "repo_bat";
    std::filesystem::create_directories(repo_b / "tools" / "spot_checker");
    sandbox.touch_file("repo_bat/encrypt_translations.bat");
    EXPECT_TRUE(is_repo_root_dir(repo_b));

    const std::filesystem::path repo_c = sandbox.path() / "repo_py";
    std::filesystem::create_directories(repo_c / "tools" / "spot_checker");
    sandbox.touch_file("repo_py/build.py");
    EXPECT_TRUE(is_repo_root_dir(repo_c));

    sandbox.touch_file(
        "repo_cmake/tools/spot_checker/build/install/bin/01_spot_check.exe");
    const std::filesystem::path nested_exe =
        repo_a / "tools" / "spot_checker" / "build" / "install" / "bin" /
        "01_spot_check.exe";
    EXPECT_TRUE(std::filesystem::equivalent(
        find_repo_root_from(nested_exe, isolated), repo_a));
    EXPECT_EQ(find_repo_root_from(isolated, repo_b), repo_b);

    EXPECT_EQ(resolve_game_dir("", "", repo_a), "");

    const std::filesystem::path game_dir = repo_a / "SampleGame";
    std::filesystem::create_directories(game_dir);

    const std::filesystem::path resolved_by_name(
        utf8_to_wide(resolve_game_dir("", "SampleGame", repo_a)));
    EXPECT_TRUE(std::filesystem::equivalent(resolved_by_name, game_dir));

    const std::filesystem::path explicit_dir = sandbox.path() / "ExplicitGame";
    std::filesystem::create_directories(explicit_dir);
    const std::filesystem::path resolved_explicit(utf8_to_wide(
        resolve_game_dir(wide_to_utf8(explicit_dir.wstring()), "SampleGame", repo_a)));
    EXPECT_TRUE(std::filesystem::equivalent(resolved_explicit, explicit_dir));

    const std::filesystem::path missing_fallback(
        utf8_to_wide(resolve_game_dir("", "MissingGameName", repo_a)));
    EXPECT_EQ(missing_fallback, std::filesystem::absolute("MissingGameName"));
}

}  // namespace
}  // namespace spot_checker
