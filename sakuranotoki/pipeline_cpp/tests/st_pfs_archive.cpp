// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System test verifying the Artemis PF8 reader against the installed game
// archive (`VN_GAME_DIR/sakuranotoki.pfs`).  Skips cleanly when `VN_GAME_DIR`
// is unset or does not contain the archive.
#include "artemis/pfs_archive.h"

#include <cstddef>
#include <cstdlib>
#include <cwchar>
#include <filesystem>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMinExpectedArchiveEntries = 100;

}  // namespace

TEST(PfsArchive, real_archive_opens) {
    const wchar_t* const game_dir = _wgetenv(L"VN_GAME_DIR");
    if (game_dir == nullptr || *game_dir == L'\0') {
        GTEST_SKIP() << "set VN_GAME_DIR to the game folder to run this check";
    }
    const fs::path base = fs::path(game_dir) / L"sakuranotoki.pfs";
    if (!fs::exists(base)) {
        GTEST_SKIP() << "no sakuranotoki.pfs under VN_GAME_DIR ("
                     << base.u8string() << ")";
    }
    EXPECT_GT(
        snt::pfs::list_pfs(base.u8string()).size(),
        kMinExpectedArchiveEntries);
}
