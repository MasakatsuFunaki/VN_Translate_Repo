// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Deployment helpers for the AlphaROM helper and rebuilt script archives:
// path manipulation, write-access probing, staged artifact discovery across
// release/install/checkout layouts, and 0script.pak + loose .nps deployment.
#pragma once

#include <string>

namespace ksh::patch {

// Append a single component to `dir`: no separator is inserted when `dir`
// already ends in one, and a bare drive letter stays bare ("C:" + "x" = "C:x").
std::string path_join(const std::string& dir, const std::string& name);

// Everything after the last separator, either slash.
std::string base_name(const std::string& path);

// True when a file can actually be created in `dir`.  This writes a probe file
// rather than reading the ACLs: the default game folder is under Program Files,
// where the ACL grants Administrators write access that an unelevated process
// still does not have.
bool can_write(const std::string& dir);

// Whether the current process is elevated; false if the query fails.
bool is_admin();

// Probes in priority order for the directory holding both pre-built Win32
// helper artifacts (`alpharom_helper.dll` and `launch.exe`):
//   1. <project_dir>/game
//   2. <exe_dir>/../game
//   3. <project_dir>/build/install/game
//   4. <project_dir>/build/helper/Release
//   5. <exe_dir>/helper_dll
// Returns empty when none of the candidate directories contains both files.
std::string find_helper_artifact_dir(const std::string& project_dir,
                                     const std::string& exe_dir);

// Probes in priority order for the staged `game` directory containing
// pre-built `0script.pak` (or loose `script/` files):
//   1. <project_dir>/game
//   2. <exe_dir>/../game
//   3. <project_dir>/build/install/game
// Returns empty when no staged script directory is present.
std::string find_staged_game_dir(const std::string& project_dir,
                                 const std::string& exe_dir);

// Rebuilds or copies 0script.pak and loose script\*.nps into `game_dir` when
// translation script inputs or staged distribution script files are present.
void deploy_script_artifacts(const std::string& project_dir,
                             const std::string& game_dir,
                             const std::string& dist_game_dir = {});

}  // namespace ksh::patch
