// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Build-and-deploy steps for the AlphaROM helper: run a child process, probe a
// directory for write access, build helper_dll/ via CMake, copy the artifacts.
//
// Split out of the app so the path/permission logic can be unit-tested without
// spawning cmake.
#pragma once

#include <string>
#include <vector>

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

// The command as 01_patch echoes it: arguments containing a space quoted.
std::string command_display(const std::vector<std::string>& cmd);

// Runs `cmd` inheriting stdio, optionally from `cwd`.  Does not echo -- the two
// callers print different forms.  Returns the child's exit code, or -1 if it
// could not be started.
int run(const std::vector<std::string>& cmd, const std::string& cwd = {});

// Rebuilds or copies 0script.pak and loose script\*.nps into `game_dir` when
// translation script inputs or staged distribution script files are present.
void deploy_script_artifacts(const std::string& project_dir,
                             const std::string& game_dir,
                             const std::string& dist_game_dir = {});

}  // namespace ksh::patch
