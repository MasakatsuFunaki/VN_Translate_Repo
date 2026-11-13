// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Component tests verifying the committed MATMENU1 v1 menu_patch.bin container
// header, record counts, and absence of raw unmasked asset signatures.
#include "common/util.h"
#include "menu/menu_patcher.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using exm::menu::kExpectedModifiedGpkEntryCount;
using exm::menu::kExpectedStandaloneFileCount;
using exm::menu::kPatchFormatVersion;
using exm::menu::kPatchHeaderSize;
using exm::menu::kPatchMagic;
using exm::menu::kPatchMagicSize;
using exm::menu::parse_patch_bin;

bool ContainsSubsequence(const std::vector<std::uint8_t>& haystack,
                         const std::uint8_t* needle,
                         std::size_t needle_len) {
    auto it = std::search(haystack.begin(), haystack.end(), needle, needle + needle_len);
    return it != haystack.end();
}

fs::path FindCommittedPatchBin() {
    std::error_code ec;
    const fs::path roots[] = {fs::current_path(ec), fs::u8path(__FILE__).parent_path()};
    for (const fs::path& base : roots) {
        for (fs::path cur = base; !cur.empty(); cur = cur.parent_path()) {
            const fs::path direct = cur / "menu_patch.bin";
            if (fs::is_regular_file(direct, ec)) {
                return direct;
            }
            const fs::path nested = cur / "EXTRAVAGANZA_03_MATSURO_CE" / "menu_patch.bin";
            if (fs::is_regular_file(nested, ec)) {
                return nested;
            }
            if (cur == cur.root_path()) {
                break;
            }
        }
    }
    return {};
}

TEST(MenuPatcherCt, CommittedMenuPatchBinHeaderAndAbsenceOfRawHeaders) {
    const fs::path patch_bin_path = FindCommittedPatchBin();
    ASSERT_FALSE(patch_bin_path.empty())
        << "Committed EXTRAVAGANZA_03_MATSURO_CE/menu_patch.bin not found.";

    const auto bytes = exm::read_file(patch_bin_path.u8string());
    ASSERT_GE(bytes.size(), kPatchHeaderSize);
    EXPECT_EQ(std::memcmp(bytes.data(), kPatchMagic, kPatchMagicSize), 0);

    const auto parsed = parse_patch_bin(bytes);
    EXPECT_EQ(parsed.version, kPatchFormatVersion);
    EXPECT_EQ(parsed.standalone_records.size(), kExpectedStandaloneFileCount);
    EXPECT_EQ(parsed.gpk_records.size(), kExpectedModifiedGpkEntryCount);

    constexpr std::uint8_t kPackBmpNeedle[] = {'P', 'A', 'C', 'K', 'B', 'M', 'P'};
    constexpr std::uint8_t kPackTypeNeedle[] = {'P', 'A', 'C', 'K', 'T', 'Y', 'P', 'E', '='};
    constexpr std::uint8_t kPngNeedle[] = {0x89u, 'P', 'N', 'G'};
    EXPECT_FALSE(ContainsSubsequence(bytes, kPackBmpNeedle, sizeof(kPackBmpNeedle)));
    EXPECT_FALSE(ContainsSubsequence(bytes, kPackTypeNeedle, sizeof(kPackTypeNeedle)));
    EXPECT_FALSE(ContainsSubsequence(bytes, kPngNeedle, sizeof(kPngNeedle)));
}

}  // namespace
