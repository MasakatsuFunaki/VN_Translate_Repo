// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Component tests verifying committed Ren_mushi_CE artifacts: speaker-gate
// validation over script_output/translated_text.json and RENMENU1 v1 header/mask
// verification over menu_patch.bin.
#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/util.h"
#include "menu/menu_patcher.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;
namespace tr = exm::translate;

using exm::menu::kExpectedModifiedGpkEntryCount;
using exm::menu::kExpectedStandaloneFileCount;
using exm::menu::kPatchFormatVersion;
using exm::menu::kPatchMagic;
using exm::menu::kPatchMagicSize;
using exm::menu::parse_patch_bin;

constexpr int kMaxWalkLevels = 6;
constexpr int kNoFailedCheck = 0;
constexpr int kMemcmpEqual = 0;

fs::path FindRepoArtifact(const fs::path& relative_path) {
    fs::path cur = fs::current_path();
    const fs::path game_dir = fs::u8path("EXTRAVAGANZA_07_REN_MUSHI_CE");
    for (int i = 0; i < kMaxWalkLevels; ++i) {
        if (fs::exists(cur / relative_path)) {
            return cur / relative_path;
        }
        if (fs::exists(cur / game_dir / relative_path)) {
            return cur / game_dir / relative_path;
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) {
            break;
        }
        cur = cur.parent_path();
    }
    return {};
}

bool ContainsAsciiSubsequence(const std::vector<std::uint8_t>& haystack,
                              const std::uint8_t* needle,
                              std::size_t needle_len) {
    const auto it =
        std::search(haystack.begin(), haystack.end(), needle, needle + needle_len);
    return it != haystack.end();
}

}  // namespace

TEST(SpeakerGate, live_extracted_text_json_passes_all_five_speaker_checks) {
    const fs::path translated_path = FindRepoArtifact(
        fs::u8path("script_output") / fs::u8path("translated_text.json"));
    ASSERT_FALSE(translated_path.empty())
        << "script_output/translated_text.json not found in repository.";
    ASSERT_TRUE(fs::exists(translated_path));

    const auto bytes = exm::read_file(translated_path.u8string());
    const std::string json_text(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const bj::object translated = bj::parse(json_text).as_object();
    const tr::SpeakerCheckReport report = tr::run_speaker_checks(translated);
    EXPECT_TRUE(report.passed) << report.reason;
    EXPECT_EQ(report.failed_check, kNoFailedCheck);
    EXPECT_EQ(report.speakers_in_data, tr::name_translations_ordered().size());
}

TEST(MenuPatcherTest, CommittedMenuPatchBinHeaderAndAbsenceOfRawHeaders) {
    const fs::path patch_bin_path = FindRepoArtifact(fs::u8path("menu_patch.bin"));
    ASSERT_FALSE(patch_bin_path.empty())
        << "EXTRAVAGANZA_07_REN_MUSHI_CE/menu_patch.bin not found.";
    ASSERT_TRUE(fs::exists(patch_bin_path));
    const auto bytes = exm::read_file(patch_bin_path.u8string());
    ASSERT_GT(bytes.size(), kPatchMagicSize);
    EXPECT_EQ(std::memcmp(bytes.data(), kPatchMagic, kPatchMagicSize), kMemcmpEqual);

    const auto parsed = parse_patch_bin(bytes);
    EXPECT_EQ(parsed.version, kPatchFormatVersion);
    EXPECT_EQ(parsed.standalone_records.size(), kExpectedStandaloneFileCount);
    EXPECT_EQ(parsed.gpk_records.size(), kExpectedModifiedGpkEntryCount);

    constexpr std::uint8_t kPackBmpNeedle[] = {'P', 'A', 'C', 'K', 'B', 'M', 'P'};
    constexpr std::uint8_t kPngNeedle[] = {0x89, 'P', 'N', 'G'};
    EXPECT_FALSE(
        ContainsAsciiSubsequence(bytes, kPackBmpNeedle, sizeof(kPackBmpNeedle)));
    EXPECT_FALSE(ContainsAsciiSubsequence(bytes, kPngNeedle, sizeof(kPngNeedle)));
}
