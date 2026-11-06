// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for src/apps/paths.{h,cpp}: project root discovery and CLI
// argument parsing (--help, --game-dir requirement, --dir override).
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "apps/paths.h"
#include "common/util.h"

namespace fs = std::filesystem;
namespace apps = exm::apps;

namespace {

constexpr int kArgcHelp = 2;
constexpr int kArgcMissingGameDir = 3;
constexpr int kArgcComplete = 5;

}  // namespace

TEST(Paths, find_project_dir_from_locates_parent_with_pipeline_and_build_py) {
    const fs::path root = fs::temp_directory_path() / "ageha_ut_paths_root";
    const fs::path nested = root / "build" / "install" / "bin";
    fs::remove_all(root);
    fs::create_directories(root / "pipeline_cpp");
    fs::create_directories(nested);
    exm::write_file_text((root / "build.py").u8string(), "# dummy build.py\n");

    const std::string found = apps::find_project_dir_from(nested.u8string());
    EXPECT_EQ(fs::u8path(found), root);

    fs::remove_all(root);
}

TEST(Paths, parse_command_line_handles_help_and_requires_game_dir) {
    std::string project;
    std::string game;
    apps::po::options_description desc("test_cli");
    desc.add(apps::common_options(project, game));

    {
        apps::po::variables_map vm;
        const char* argv[] = {"test_cli", "--help"};
        const auto rc = apps::parse_command_line(kArgcHelp, const_cast<char**>(argv), desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, 0);
    }

    {
        apps::po::variables_map vm;
        const char* argv[] = {"test_cli", "--dir", "D:\\Project"};
        const auto rc =
            apps::parse_command_line(kArgcMissingGameDir, const_cast<char**>(argv), desc, vm);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(*rc, 1);
    }

    {
        apps::po::variables_map vm;
        const char* argv[] = {"test_cli", "--dir", "D:\\Project", "--game-dir", "E:\\Game"};
        const auto rc =
            apps::parse_command_line(kArgcComplete, const_cast<char**>(argv), desc, vm);
        EXPECT_FALSE(rc.has_value());
        EXPECT_EQ(project, "D:\\Project");
        EXPECT_EQ(game, "E:\\Game");
    }
}
