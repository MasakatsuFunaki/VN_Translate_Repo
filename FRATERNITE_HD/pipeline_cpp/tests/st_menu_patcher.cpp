// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System tests against a live game installation specified via VN_GAME_DIR.
#include "common/util.h"
#include "menu_patcher/menu_patcher.h"
#include "test_support.h"
#include "yuris/ypf_archive.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>

using namespace frat;
using namespace frat::menu;
using namespace frat::yuris;
using frat_test::sha256_hex;

namespace {

constexpr std::size_t kExpectedLiveEntryCount = 1483u;
constexpr std::size_t kExpectedLiveTranslatedCount = 507u;
constexpr std::size_t kExpectedLiveUnchangedCount =
    kExpectedLiveEntryCount - kExpectedLiveTranslatedCount;
constexpr std::size_t kExpectedLivePatchedSize = 20202996u;
constexpr const char* kExpectedLivePatchedSha256 =
    "c36e53c008ef34cce4fc530f8f30383dce96b857b6fdd8c3830a6f548907b897";

std::optional<std::string> resolve_live_unpatched_cgsys() {
    const wchar_t* env_game = _wgetenv(L"VN_GAME_DIR");
    if (env_game == nullptr || env_game[0] == L'\0') {
        return std::nullopt;
    }
    const std::filesystem::path pac_dir = std::filesystem::path(env_game) / "pac";
    std::error_code ec;
    const std::filesystem::path bak_path = pac_dir / "cgsys.ypf.bak";
    if (std::filesystem::is_regular_file(bak_path, ec)) {
        return bak_path.u8string();
    }
    const std::filesystem::path ypf_path = pac_dir / "cgsys.ypf";
    if (!std::filesystem::is_regular_file(ypf_path, ec)) {
        return std::nullopt;
    }
    const auto sz = std::filesystem::file_size(ypf_path, ec);
    if (ec || sz == kExpectedLivePatchedSize) {
        return std::nullopt;
    }
    return ypf_path.u8string();
}

}  // namespace

TEST(MenuPatcherSystem, LiveArchive_ParsesAll1483EntriesAndIdentityRepacksLosslessly) {
    const auto live_archive = resolve_live_unpatched_cgsys();
    if (!live_archive.has_value()) {
        GTEST_SKIP() << "VN_GAME_DIR not set or unpatched pac/cgsys.ypf(.bak) not present";
    }
    const Bytes orig = read_file(*live_archive);
    const auto entries = parse_ypf_index_bytes(orig, *live_archive);
    ASSERT_EQ(entries.size(), kExpectedLiveEntryCount);

    MenuArchivePatcher patcher;
    PatchSummary summary;
    const std::unordered_map<std::string, Bytes> no_replacements;
    const Bytes repacked = patcher.repack_archive_bytes(orig, no_replacements, &summary);
    EXPECT_EQ(summary.total_entries, kExpectedLiveEntryCount);
    EXPECT_EQ(summary.replaced_entries, 0u);
    EXPECT_EQ(repacked, orig);
}

TEST(MenuPatcherSystem, LiveArchive_XorPatchReconstructsBitExactArchive) {
    const auto live_archive = resolve_live_unpatched_cgsys();
    if (!live_archive.has_value()) {
        GTEST_SKIP() << "VN_GAME_DIR not set or unpatched pac/cgsys.ypf(.bak) not present";
    }

    const std::filesystem::path tracked_patch =
        std::filesystem::u8path(FRAT_PROJECT_DIR) / "cgsys_patch.bin";
    ASSERT_TRUE(std::filesystem::exists(tracked_patch));
    const Bytes patch_blob = read_file(tracked_patch.u8string());

    const Bytes orig = read_file(*live_archive);
    MenuArchivePatcher patcher;
    PatchSummary summary;
    const Bytes repacked = patcher.apply_xor_patch_bytes(orig, patch_blob, &summary);
    EXPECT_EQ(summary.total_entries, kExpectedLiveEntryCount);
    EXPECT_EQ(summary.replaced_entries, kExpectedLiveTranslatedCount);
    EXPECT_EQ(summary.discovered_assets, kExpectedLiveTranslatedCount);
    EXPECT_EQ(repacked.size(), kExpectedLivePatchedSize);
    EXPECT_EQ(sha256_hex(repacked.data(), repacked.size()), kExpectedLivePatchedSha256);

    const auto orig_entries = parse_ypf_index_bytes(orig, *live_archive);
    const auto repacked_entries = parse_ypf_index_bytes(repacked, "reconstructed_cgsys.ypf");
    ASSERT_EQ(orig_entries.size(), kExpectedLiveEntryCount);
    ASSERT_EQ(repacked_entries.size(), kExpectedLiveEntryCount);

    std::size_t translated_count = 0;
    std::size_t unchanged_count = 0;
    for (std::size_t i = 0; i < kExpectedLiveEntryCount; ++i) {
        const YpfEntry& oe = orig_entries[i];
        const YpfEntry& re = repacked_entries[i];
        EXPECT_EQ(re.name, oe.name);
        EXPECT_EQ(re.name_hash, oe.name_hash);
        EXPECT_EQ(re.name_size_raw, oe.name_size_raw);
        EXPECT_EQ(re.name_bytes_xor, oe.name_bytes_xor);
        EXPECT_EQ(re.type, oe.type);
        EXPECT_EQ(re.compressed, oe.compressed);

        const Bytes orig_payload = read_entry(orig, oe);
        const Bytes repacked_payload = read_entry(repacked, re);
        const PngHeaderInfo orig_ihdr = parse_png_ihdr(orig_payload, oe.name);
        const PngHeaderInfo repacked_ihdr = parse_png_ihdr(repacked_payload, re.name);
        EXPECT_EQ(repacked_ihdr.width, orig_ihdr.width);
        EXPECT_EQ(repacked_ihdr.height, orig_ihdr.height);
        EXPECT_EQ(repacked_ihdr.bit_depth, orig_ihdr.bit_depth);
        EXPECT_EQ(repacked_ihdr.color_type, orig_ihdr.color_type);

        if (repacked_payload != orig_payload) {
            ++translated_count;
            continue;
        }
        ++unchanged_count;
    }
    EXPECT_EQ(translated_count, kExpectedLiveTranslatedCount);
    EXPECT_EQ(unchanged_count, kExpectedLiveUnchangedCount);
}
