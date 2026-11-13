// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System verification tests for `menu_patch.bin` against pristine HD10 and
// Complete Edition installations when `VN_GAME_DIR` and `VN_CE_GAME_DIR` are
// provided in the environment.

#include "menu/menu_patcher.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace mgi::menu {
namespace {

constexpr std::size_t kByteIndex0 = 0u;
constexpr std::size_t kByteIndex1 = 1u;
constexpr std::size_t kByteIndex2 = 2u;
constexpr std::size_t kByteIndex3 = 3u;
constexpr std::uint32_t kByteShift1 = 8u;
constexpr std::uint32_t kByteShift2 = 16u;
constexpr std::uint32_t kByteShift3 = 24u;

constexpr int kMaxAncestorLevels = 8;
constexpr std::size_t kPngMinIhdrBytes = 33u;
constexpr std::uint8_t kPngColorTypeTruecolorRgb = 2u;
constexpr std::uint8_t kPngColorTypeTruecolorRgba = 6u;

std::uint32_t read_u32_be(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[kByteIndex0]) << kByteShift3) |
           (static_cast<std::uint32_t>(p[kByteIndex1]) << kByteShift2) |
           (static_cast<std::uint32_t>(p[kByteIndex2]) << kByteShift1) |
           static_cast<std::uint32_t>(p[kByteIndex3]);
}

std::uint32_t read_u32_le_local(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[kByteIndex0]) |
           (static_cast<std::uint32_t>(p[kByteIndex1]) << kByteShift1) |
           (static_cast<std::uint32_t>(p[kByteIndex2]) << kByteShift2) |
           (static_cast<std::uint32_t>(p[kByteIndex3]) << kByteShift3);
}

std::uint16_t read_u16_le_local(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[kByteIndex0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[kByteIndex1]) << kByteShift1);
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

TEST(MenuPatcherSystemTest, ReconstructsAllStandaloneVariantsAnd74ArchiveEntriesAcrossHd10AndCe) {
    const wchar_t* hd10_env = _wgetenv(L"VN_GAME_DIR");
    const wchar_t* ce_env = _wgetenv(L"VN_CE_GAME_DIR");
    if (!hd10_env || hd10_env[0] == L'\0' || !ce_env || ce_env[0] == L'\0') {
        GTEST_SKIP() << "Skipping live archive verification (VN_GAME_DIR / VN_CE_GAME_DIR unset)";
    }

    const fs::path hd10_dir(hd10_env);
    const fs::path ce_dir(ce_env);
    ASSERT_TRUE(fs::is_directory(hd10_dir));
    ASSERT_TRUE(fs::is_directory(ce_dir));

    const fs::path patch_path = find_committed_patch_bin();
    ASSERT_FALSE(patch_path.empty());
    const auto parsed = parse_menu_patch_bin(mgi::read_file(patch_path.u8string()));

    for (const fs::path& game_dir : {hd10_dir, ce_dir}) {
        for (const auto& grp : parsed.standalone_groups) {
            const auto orig_bytes = mgi::read_file((game_dir / fs::u8path(grp.rel_path)).u8string());
            const auto rebuilt = reconstruct_standalone_file(grp, orig_bytes);
            EXPECT_FALSE(rebuilt.empty());
        }
    }

    {
        const auto sys0_gtb =
            mgi::read_file((hd10_dir / fs::u8path("dwq") / fs::u8path("sys0.gtb")).u8string());
        const auto sys0_gpk =
            mgi::read_file((hd10_dir / fs::u8path("dwq") / fs::u8path("sys0.gpk")).u8string());
        const auto [new_gtb, new_gpk] =
            reconstruct_archive_pair(parsed.archives[0], sys0_gtb, sys0_gpk);
        EXPECT_EQ(new_gtb.size(), parsed.archives[0].new_gtb_size);
        EXPECT_EQ(new_gpk.size(), parsed.archives[0].new_gpk_size);
    }

    constexpr std::size_t kPngSigLen = 8u;
    constexpr std::uint8_t kExpectedPngSig[kPngSigLen] = {
        0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    constexpr std::size_t kIhdrWidthOff = 16u;
    constexpr std::size_t kIhdrHeightOff = 20u;
    constexpr std::size_t kIhdrColorTypeOff = 25u;

    for (const fs::path& game_dir : {hd10_dir, ce_dir}) {
        std::size_t verified_entries = 0;
        for (const auto& sec : parsed.archives) {
            const fs::path stem_rel = fs::u8path(sec.archive_stem);
            const auto verified = verify_and_unmask_archive_from_disk(
                sec,
                game_dir / fs::u8path(stem_rel.u8string() + ".gtb"),
                game_dir / fs::u8path(stem_rel.u8string() + ".gpk"));

            EXPECT_EQ(verified.computed_new_gpk_size, sec.new_gpk_size);
            const auto gtb_table = parse_gtb_offsets(verified.new_gtb, sec.new_gpk_size);
            EXPECT_TRUE(gtb_table.has_over2g_u64_table);

            const std::uint8_t expected_color_type = (sec.archive_stem == "dwq\\ev0")
                                                         ? kPngColorTypeTruecolorRgb
                                                         : kPngColorTypeTruecolorRgba;

            for (const auto& entry : verified.patched_entries) {
                ASSERT_GT(entry.unmasked_dwq.size(), kDwqHeaderSize + kPngMinIhdrBytes);
                const std::uint8_t* dwq = entry.unmasked_dwq.data();
                const std::uint32_t png_sz = read_u32_le_local(dwq + kDwqDataSizeOffset);
                ASSERT_EQ(kDwqHeaderSize + png_sz, entry.unmasked_dwq.size());

                const std::uint32_t dwq_w = read_u32_le_local(dwq + kDwqWidthOffset);
                const std::uint16_t dwq_h = read_u16_le_local(dwq + kDwqHeightOffset);
                const std::uint16_t dwq_bpp = read_u16_le_local(dwq + kDwqBppOffset);
                EXPECT_EQ(dwq_bpp, 0u);

                const std::uint8_t* png = dwq + kDwqHeaderSize;
                ASSERT_EQ(std::memcmp(png, kExpectedPngSig, kPngSigLen), 0);
                EXPECT_EQ(read_u32_be(png + kIhdrWidthOff), dwq_w);
                EXPECT_EQ(read_u32_be(png + kIhdrHeightOff), dwq_h);
                EXPECT_EQ(png[kIhdrColorTypeOff], expected_color_type);

                const RgbaImage decoded = decode_png_rgba(png, png_sz);
                EXPECT_EQ(decoded.width, static_cast<int>(dwq_w));
                EXPECT_EQ(decoded.height, static_cast<int>(dwq_h));
                ++verified_entries;
            }
        }
        EXPECT_EQ(verified_entries, kExpectedTotalPatchedEntryCount);
    }
}

}  // namespace mgi::menu
