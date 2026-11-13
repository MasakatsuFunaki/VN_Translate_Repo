// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests for the checked-in `menu_patch.bin` ("MSGMENU1" v1) artifact.
// Runs offline without requiring installed game directories.

#include "menu/menu_patcher.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace mgi::menu {
namespace {

constexpr int kMaxAncestorLevels = 8;

constexpr std::size_t kExpectedOrigCharanameBytes = 104u;
constexpr std::uint32_t kExpectedOrigCharanameHash = 0x62F2B2CEu;
constexpr std::size_t kExpectedNewCharanameBytes = 154u;
constexpr std::uint32_t kExpectedNewCharanameHash = 0x94C1B06Cu;

constexpr std::size_t kExpectedOrigBacklogBytes = 1258u;
constexpr std::uint32_t kExpectedOrigBacklogHash = 0xB66908A4u;
constexpr std::size_t kExpectedNewBacklogBytes = 1244u;
constexpr std::uint32_t kExpectedNewBacklogHash = 0xDAB33A61u;

constexpr std::size_t kExpectedHd10OrigMainControlBytes = 480u;
constexpr std::uint32_t kExpectedHd10OrigMainControlHash = 0x6998A1B1u;
constexpr std::size_t kExpectedCeOrigMainControlBytes = 468u;
constexpr std::uint32_t kExpectedCeOrigMainControlHash = 0xD5D73580u;

constexpr std::size_t kSharedVariantCount = 1u;
constexpr std::size_t kMultiEditionVariantCount = 2u;

constexpr std::size_t kBitmapFileHeaderSize = 14u;
constexpr std::size_t kBmpMagicOffset0 = 0u;
constexpr std::size_t kBmpMagicOffset1 = 1u;
constexpr std::size_t kBmpReservedOffset0 = 6u;
constexpr std::size_t kBmpReservedOffset1 = 7u;
constexpr std::size_t kBmpReservedOffset2 = 8u;
constexpr std::size_t kBmpReservedOffset3 = 9u;

bool has_subsequence(const std::vector<std::uint8_t>& hay,
                     const std::uint8_t* needle,
                     std::size_t needle_len) {
    return std::search(hay.begin(), hay.end(), needle, needle + needle_len) != hay.end();
}

fs::path find_committed_patch_bin() {
    fs::path cur = fs::current_path();
    for (int depth = 0; depth < kMaxAncestorLevels && !cur.empty(); ++depth) {
        const fs::path direct = cur / fs::u8path("menu_patch.bin");
        if (fs::is_regular_file(direct) && fs::exists(cur / fs::u8path("pipeline_cpp"))) {
            return direct;
        }
        const fs::path sub =
            cur / fs::u8path("EXTRAVAGANZA_02_MUSHIGURUI_HD10") / fs::u8path("menu_patch.bin");
        if (fs::is_regular_file(sub)) {
            return sub;
        }
        if (cur == cur.parent_path()) {
            break;
        }
        cur = cur.parent_path();
    }
    return {};
}

}  // namespace

TEST(MenuPatcherComponentTest, CommittedMenuPatchBinMeetsWireInvariantsAndContainsZeroRawHeaders) {
    const fs::path patch_path = find_committed_patch_bin();
    ASSERT_FALSE(patch_path.empty()) << "Expected menu_patch.bin in EXTRAVAGANZA_02_MUSHIGURUI_HD10/";

    const auto raw = mgi::read_file(patch_path.u8string());
    const auto parsed = parse_menu_patch_bin(raw);

    ASSERT_EQ(parsed.version, kMenuPatchFormatVersion);
    ASSERT_EQ(parsed.standalone_groups.size(), kExpectedStandaloneGroupCount);
    ASSERT_EQ(parsed.archives.size(), kExpectedArchiveCount);

    EXPECT_EQ(parsed.standalone_groups[0].rel_path, "spt\\charaname.xtx");
    ASSERT_EQ(parsed.standalone_groups[0].variants.size(), kSharedVariantCount);
    EXPECT_EQ(parsed.standalone_groups[0].variants[0].orig_file_size, kExpectedOrigCharanameBytes);
    EXPECT_EQ(parsed.standalone_groups[0].variants[0].orig_data_hash, kExpectedOrigCharanameHash);
    EXPECT_EQ(parsed.standalone_groups[0].variants[0].new_file_size, kExpectedNewCharanameBytes);
    EXPECT_EQ(parsed.standalone_groups[0].variants[0].new_data_hash, kExpectedNewCharanameHash);

    EXPECT_EQ(parsed.standalone_groups[1].rel_path, "init2\\backlog.fxf");
    ASSERT_EQ(parsed.standalone_groups[1].variants.size(), kSharedVariantCount);
    EXPECT_EQ(parsed.standalone_groups[1].variants[0].orig_file_size, kExpectedOrigBacklogBytes);
    EXPECT_EQ(parsed.standalone_groups[1].variants[0].orig_data_hash, kExpectedOrigBacklogHash);
    EXPECT_EQ(parsed.standalone_groups[1].variants[0].new_file_size, kExpectedNewBacklogBytes);
    EXPECT_EQ(parsed.standalone_groups[1].variants[0].new_data_hash, kExpectedNewBacklogHash);

    EXPECT_EQ(parsed.standalone_groups[2].rel_path, "init2\\mainControl.fxf");
    ASSERT_EQ(parsed.standalone_groups[2].variants.size(), kMultiEditionVariantCount);
    EXPECT_EQ(parsed.standalone_groups[2].variants[0].orig_file_size,
              kExpectedHd10OrigMainControlBytes);
    EXPECT_EQ(parsed.standalone_groups[2].variants[0].orig_data_hash,
              kExpectedHd10OrigMainControlHash);
    EXPECT_EQ(parsed.standalone_groups[2].variants[1].orig_file_size,
              kExpectedCeOrigMainControlBytes);
    EXPECT_EQ(parsed.standalone_groups[2].variants[1].orig_data_hash,
              kExpectedCeOrigMainControlHash);

    EXPECT_EQ(parsed.archives[0].archive_stem, "dwq\\sys0");
    EXPECT_EQ(parsed.archives[0].entries.size(), kExpectedSys0PatchedCount);
    EXPECT_EQ(parsed.archives[1].archive_stem, "dwq\\ta0");
    EXPECT_EQ(parsed.archives[1].entries.size(), kExpectedTa0PatchedCount);
    EXPECT_EQ(parsed.archives[2].archive_stem, "dwq\\ev0");
    EXPECT_EQ(parsed.archives[2].entries.size(), kExpectedEv0PatchedCount);

    std::size_t total_entries = 0;
    for (const auto& sec : parsed.archives) {
        total_entries += sec.entries.size();
    }
    EXPECT_EQ(total_entries, kExpectedTotalPatchedEntryCount);

    static constexpr std::uint8_t kPackBmp[] = {'P', 'A', 'C', 'K', 'B', 'M', 'P'};
    static constexpr std::uint8_t kPackType[] = {'P', 'A', 'C', 'K', 'T', 'Y', 'P', 'E', '='};
    static constexpr std::uint8_t kPngMagic[] = {0x89u, 'P', 'N', 'G'};
    static constexpr std::uint8_t kDwqBmpMagic[] = {'B', 'M', 'P', ' '};
    static constexpr std::uint8_t kOver2G[] = {'o', 'v', 'e', 'r', '2', 'G', '!'};

    EXPECT_FALSE(has_subsequence(raw, kPackBmp, sizeof(kPackBmp)));
    EXPECT_FALSE(has_subsequence(raw, kPackType, sizeof(kPackType)));
    EXPECT_FALSE(has_subsequence(raw, kPngMagic, sizeof(kPngMagic)));
    EXPECT_FALSE(has_subsequence(raw, kDwqBmpMagic, sizeof(kDwqBmpMagic)));
    EXPECT_FALSE(has_subsequence(raw, kOver2G, sizeof(kOver2G)));

    bool found_bmp_header = false;
    for (std::size_t i = 0; i + kBitmapFileHeaderSize <= raw.size(); ++i) {
        if (raw[i + kBmpMagicOffset0] == 'B' &&
            raw[i + kBmpMagicOffset1] == 'M' &&
            raw[i + kBmpReservedOffset0] == 0u &&
            raw[i + kBmpReservedOffset1] == 0u &&
            raw[i + kBmpReservedOffset2] == 0u &&
            raw[i + kBmpReservedOffset3] == 0u) {
            found_bmp_header = true;
            break;
        }
    }
    EXPECT_FALSE(found_bmp_header);
}

}  // namespace mgi::menu
