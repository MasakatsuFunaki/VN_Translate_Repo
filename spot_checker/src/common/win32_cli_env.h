// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace spot_checker {

// Configures Windows console input/output codepages to UTF-8 (`CP_UTF8`) and
// disables stdio sync buffering caveats. Safe no-op when no console is attached.
void setup_console_utf8() noexcept;

// Lowers the current process scheduling priority to `BELOW_NORMAL_PRIORITY_CLASS`
// so background spot-check runs never starve foreground desktop responsiveness.
void lower_process_priority() noexcept;

// Returns the absolute path to the running executable image.
std::filesystem::path executable_path();

// Parses the current process wide command line (`GetCommandLineW` /
// `CommandLineToArgvW`) and returns UTF-8 arguments excluding `argv[0]`.
std::vector<std::string> process_utf8_args();

// Game-agnostic repository root predicate: returns true when `candidate`
// contains `spot_checker/CMakeLists.txt` or contains a `spot_checker` directory
// alongside `encrypt_translations.bat` or `build.py`. Never hardcodes any
// specific game directory name.
bool is_repo_root_dir(const std::filesystem::path& candidate) noexcept;

// Walks upward from `start_path` (if `start_path` is a regular file, starts at
// `start_path.parent_path()`) and returns the first ancestor satisfying
// `is_repo_root_dir`. Falls back to `fallback_dir` when no ancestor matches.
std::filesystem::path find_repo_root_from(
    const std::filesystem::path& start_path,
    const std::filesystem::path& fallback_dir = {});

// Convenience wrapper walking upward from `executable_path()` with
// `std::filesystem::current_path()` as fallback.
std::filesystem::path find_repo_root();

// Resolves the target game directory from CLI options:
// - If `game_dir_opt` is non-empty, returns its absolute UTF-8 path.
// - Else if `game_name_opt` is non-empty, checks `repo_root / game_name_opt`
//   first (when `repo_root` is non-empty and that directory exists), falling
//   back to `std::filesystem::absolute(game_name_opt)`.
// - If both options are empty, returns an empty string.
std::string resolve_game_dir(std::string_view game_dir_opt,
                             std::string_view game_name_opt,
                             const std::filesystem::path& repo_root);

}  // namespace spot_checker
