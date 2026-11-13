// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System test validating KifArchive against the installed game's scene.int.
#include "cs2/cs2_archive.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMinExpectedSceneEntries = 100;
constexpr const char kDefaultGameDir[] =
    "C:\\あざらしそふと\\アマカノ2～Perfect Edition～";

fs::path resolve_game_dir() {
    const wchar_t* const wenv = _wgetenv(L"VN_GAME_DIR");
    if (wenv != nullptr && *wenv != L'\0') {
        return fs::path(wenv);
    }
    return fs::u8path(kDefaultGameDir);
}

}  // namespace

TEST(Cs2Archive, real_scene_int_opens) {
    const fs::path scene = resolve_game_dir() / "scene.int";
    if (!fs::exists(scene)) {
        GTEST_SKIP() << "scene.int not installed";
    }
    const ama::cs2::KifArchive arc(scene.u8string());
    EXPECT_GT(arc.entries().size(), kMinExpectedSceneEntries);
}
