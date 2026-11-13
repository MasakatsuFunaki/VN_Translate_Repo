// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Component tests verifying committed menu_patch.bin invariants and
// script_output/translated_text.json speaker-gate compliance in EXTRAVAGANZA_06_YUMEMI_CE.
#include "cache/speaker_gate.h"
#include "common/util.h"
#include "menu/menu_patcher.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr int kMaxWalkLevels = 6;
constexpr int kMemcmpEqual = 0;

fs::path find_repo_artifact(const fs::path& relative_target) {
    fs::path cur = fs::current_path();
    const fs::path game_dir = fs::u8path("EXTRAVAGANZA_06_YUMEMI_CE");
    for (int i = 0; i < kMaxWalkLevels; ++i) {
        if (fs::exists(cur / relative_target)) {
            return cur / relative_target;
        }
        if (fs::exists(cur / game_dir / relative_target)) {
            return cur / game_dir / relative_target;
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) {
            break;
        }
        cur = cur.parent_path();
    }
    return {};
}

bool contains_byte_subsequence(const std::vector<std::uint8_t>& haystack,
                               const std::uint8_t* needle,
                               std::size_t needle_len) {
    const auto it =
        std::search(haystack.begin(), haystack.end(), needle, needle + needle_len);
    return it != haystack.end();
}

TEST(CorpusSpeakerGateCt, ExtractedCorpusPassesSpeakerGate) {
    const fs::path translated_path = find_repo_artifact(
        fs::u8path("script_output") / fs::u8path("translated_text.json"));
    ASSERT_FALSE(translated_path.empty())
        << "script_output/translated_text.json not found in repository.";
    ASSERT_TRUE(fs::exists(translated_path));

    const auto root = exm::json_parse_file(translated_path.u8string());
    ASSERT_TRUE(root.is_object());
    const auto report = exm::translate::run_speaker_checks(root.get_object());
    EXPECT_TRUE(report.passed) << report.reason;
}

TEST(MenuPatchBinCt, CommittedMenuPatchBinHeaderAndAbsenceOfRawHeaders) {
    const fs::path patch_bin_path = find_repo_artifact(fs::u8path("menu_patch.bin"));
    ASSERT_FALSE(patch_bin_path.empty())
        << "EXTRAVAGANZA_06_YUMEMI_CE/menu_patch.bin not found in repository.";
    ASSERT_TRUE(fs::exists(patch_bin_path));

    const auto bytes = exm::read_file(patch_bin_path.u8string());
    ASSERT_GT(bytes.size(), exm::menu::kPatchMagicSize);
    EXPECT_EQ(std::memcmp(bytes.data(), exm::menu::kPatchMagic, exm::menu::kPatchMagicSize),
              kMemcmpEqual);

    const auto parsed = exm::menu::parse_patch_bin(bytes);
    EXPECT_EQ(parsed.version, exm::menu::kPatchFormatVersion);
    EXPECT_EQ(parsed.standalone_records.size(), exm::menu::kExpectedStandaloneFileCount);
    EXPECT_GT(parsed.gpk_records.size(), 0u);

    constexpr std::uint8_t kPackBmpNeedle[] = {'P', 'A', 'C', 'K', 'B', 'M', 'P'};
    constexpr std::uint8_t kPngNeedle[] = {0x89u, 'P', 'N', 'G'};
    EXPECT_FALSE(
        contains_byte_subsequence(bytes, kPackBmpNeedle, sizeof(kPackBmpNeedle)));
    EXPECT_FALSE(contains_byte_subsequence(bytes, kPngNeedle, sizeof(kPngNeedle)));
}

}  // namespace
