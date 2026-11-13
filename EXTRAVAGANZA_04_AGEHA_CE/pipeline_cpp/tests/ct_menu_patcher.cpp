// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Component tests for the committed AGEHA_CE AGHMENU1 v1 menu_patch.bin
// artifact: verifies container header invariants, expected record counts, and
// obfuscation (absence of plaintext DWQ/PNG headers).
#include "menu/menu_patcher.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {

namespace fs = std::filesystem;
using exm::menu::kExpectedModifiedGpkEntryCount;
using exm::menu::kExpectedStandaloneFileCount;
using exm::menu::kPatchFormatVersion;
using exm::menu::kPatchMagic;
using exm::menu::kPatchMagicSize;
using exm::menu::parse_patch_bin;

constexpr int kMaxWalkLevels = 6;

bool ContainsSubsequence(const std::vector<std::uint8_t>& haystack,
                         const std::uint8_t* needle,
                         std::size_t needle_len) {
    const auto it =
        std::search(haystack.begin(), haystack.end(), needle, needle + needle_len);
    return it != haystack.end();
}

fs::path FindCommittedPatchBin() {
    fs::path cur = fs::current_path();
    for (int i = 0; i < kMaxWalkLevels; ++i) {
        if (fs::exists(cur / "menu_patch.bin")) {
            return cur / "menu_patch.bin";
        }
        if (fs::exists(cur / "EXTRAVAGANZA_04_AGEHA_CE" / "menu_patch.bin")) {
            return cur / "EXTRAVAGANZA_04_AGEHA_CE" / "menu_patch.bin";
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) {
            break;
        }
        cur = cur.parent_path();
    }
    return {};
}

}  // namespace

TEST(MenuPatcherComponent, CommittedMenuPatchBinHeaderAndAbsenceOfRawHeaders) {
    const fs::path patch_bin_path = FindCommittedPatchBin();
    ASSERT_FALSE(patch_bin_path.empty())
        << "Committed EXTRAVAGANZA_04_AGEHA_CE/menu_patch.bin not found";

    const auto bytes = exm::read_file(patch_bin_path.u8string());
    ASSERT_GT(bytes.size(), kPatchMagicSize);
    EXPECT_EQ(std::memcmp(bytes.data(), kPatchMagic, kPatchMagicSize), 0);

    const auto parsed = parse_patch_bin(bytes);
    EXPECT_EQ(parsed.version, kPatchFormatVersion);
    EXPECT_EQ(parsed.standalone_records.size(), kExpectedStandaloneFileCount);
    EXPECT_EQ(parsed.gpk_records.size(), kExpectedModifiedGpkEntryCount);

    for (const auto& rec : parsed.standalone_records) {
        EXPECT_FALSE(rec.rel_path.empty());
        EXPECT_GT(rec.new_file_size, 0u);
        EXPECT_EQ(rec.masked_payload.size(), rec.new_file_size);
    }
    for (const auto& rec : parsed.gpk_records) {
        EXPECT_GT(rec.new_size, 0u);
        EXPECT_EQ(rec.masked_entry.size(), rec.new_size);
    }

    constexpr std::uint8_t kPackBmpNeedle[] = {'P', 'A', 'C', 'K', 'B', 'M', 'P'};
    constexpr std::uint8_t kPackTypeNeedle[] = {'P', 'A', 'C', 'K', 'T', 'Y', 'P', 'E', '='};
    constexpr std::uint8_t kPngNeedle[] = {0x89u, 'P', 'N', 'G'};
    EXPECT_FALSE(ContainsSubsequence(bytes, kPackBmpNeedle, sizeof(kPackBmpNeedle)));
    EXPECT_FALSE(ContainsSubsequence(bytes, kPackTypeNeedle, sizeof(kPackTypeNeedle)));
    EXPECT_FALSE(ContainsSubsequence(bytes, kPngNeedle, sizeof(kPngNeedle)));
}
