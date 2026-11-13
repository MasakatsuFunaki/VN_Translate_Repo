// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests verifying the committed extravaganza_patch.bin container
// header, record counts, and absence of raw unmasked asset signatures.
#include "common/util.h"
#include "menus/menu_patcher.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace mn = exc::menus;
namespace fs = std::filesystem;
using namespace exc;

namespace {

constexpr std::size_t kDwqSignatureLength = 16;
constexpr std::size_t kPackBmpHeaderLen = 7;
constexpr std::size_t kPackTypePrefixLen = 9;

fs::path ResolvePatchBinPath() {
    std::error_code ec;
    const fs::path roots[] = {fs::current_path(ec), fs::u8path(__FILE__).parent_path()};
    for (const fs::path& base : roots) {
        for (fs::path cur = base; !cur.empty(); cur = cur.parent_path()) {
            const fs::path direct = cur / "extravaganza_patch.bin";
            if (fs::is_regular_file(direct, ec)) {
                return direct;
            }
            const fs::path nested = cur / "EXTRAVAGANZA_01_CE" / "extravaganza_patch.bin";
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

bool ContainsBytes(const Bytes& haystack, const void* needle, std::size_t needle_len) {
    if (needle_len == 0 || haystack.size() < needle_len) {
        return false;
    }
    const auto* pat = static_cast<const std::uint8_t*>(needle);
    const std::size_t limit = haystack.size() - needle_len;
    for (std::size_t i = 0; i <= limit; ++i) {
        if (std::memcmp(haystack.data() + i, pat, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(MenuPatcherCt, CommittedPatchBinHeaderAndAbsenceOfRawHeaders) {
    const fs::path patch_bin_path = ResolvePatchBinPath();
    std::error_code ec;
    ASSERT_FALSE(patch_bin_path.empty());
    ASSERT_TRUE(fs::is_regular_file(patch_bin_path, ec))
        << "Missing extravaganza_patch.bin at " << patch_bin_path.u8string();

    const Bytes patch_bin = read_file(patch_bin_path.u8string());
    ASSERT_GE(patch_bin.size(), mn::kPatchHeaderSize);
    EXPECT_EQ(std::memcmp(patch_bin.data(), mn::kPatchMagic, mn::kPatchMagicSize), 0);

    constexpr std::uint8_t kPngHeader[] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    EXPECT_FALSE(ContainsBytes(patch_bin, "PACKBMP", kPackBmpHeaderLen));
    EXPECT_FALSE(ContainsBytes(patch_bin, "BMP+MASK        ", kDwqSignatureLength));
    EXPECT_FALSE(ContainsBytes(patch_bin, "PACKTYPE=", kPackTypePrefixLen));
    EXPECT_FALSE(ContainsBytes(patch_bin, kPngHeader, sizeof(kPngHeader)));

    const mn::ParsedPatchBin parsed = mn::parse_patch_bin(patch_bin);
    EXPECT_EQ(parsed.version, mn::kPatchFormatVersion);
    EXPECT_EQ(parsed.standalone_file_count, mn::kExpectedStandaloneFileCount);
    EXPECT_EQ(parsed.gpk_entry_count, mn::kExpectedModifiedGpkEntryCount);
    EXPECT_EQ(parsed.standalone_records.size(), mn::kExpectedStandaloneFileCount);
    EXPECT_EQ(parsed.gpk_records.size(), mn::kExpectedModifiedGpkEntryCount);

    for (const mn::StandalonePatchRecord& rec : parsed.standalone_records) {
        EXPECT_FALSE(rec.rel_path.empty());
        EXPECT_GT(rec.new_file_size, 0u);
        EXPECT_EQ(rec.masked_payload.size(), rec.new_file_size);
    }
    for (const mn::GpkEntryPatchRecord& rec : parsed.gpk_records) {
        EXPECT_GT(rec.new_size, 0u);
        EXPECT_EQ(rec.masked_entry.size(), rec.new_size);
    }
}
