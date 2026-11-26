// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for GameAssetSource::ReadStandaloneFile and boot.hg3 backup
// precedence so offline menu patching stays idempotent on already-patched
// installs.
#include "hg3/hg3_codec.h"
#include "imaging/bgra_image.h"
#include "menu/core/menu_patch_container.h"
#include "menu/domain_renderer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kSyntheticVCode2 = "NXT-76BPQ5YD";
constexpr std::size_t kMinSliceSize = ama::menu::kMinOrigSliceBytes;
constexpr std::size_t kBelowMinSliceSize = kMinSliceSize - 1U;
constexpr std::size_t kStandaloneSampleBytes = 128U;
constexpr int kBootWidth = 8;
constexpr int kBootHeight = 8;

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned> seq{0U};
        path_ = fs::temp_directory_path() /
                fs::u8path("ama_ut_domain_renderer_" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void WriteBytes(const fs::path& file_path, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    if (file_path.has_parent_path()) {
        fs::create_directories(file_path.parent_path(), ec);
    }
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << file_path.u8string();
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_TRUE(out.good()) << file_path.u8string();
}

std::vector<std::uint8_t> MakePattern(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> buf(size);
    for (std::size_t i = 0; i < size; ++i) {
        buf[i] = static_cast<std::uint8_t>((i * 37U + seed) & 0xFFU);
    }
    return buf;
}

std::vector<std::uint8_t> MakeSyntheticBootHg3(ama::imaging::BgraColor fill) {
    ama::hg3::Hg3Container container;
    ama::hg3::Hg3Frame frame;
    frame.frame_id = 0U;
    frame.stdinfo.width = kBootWidth;
    frame.stdinfo.height = kBootHeight;
    frame.stdinfo.bpp = 32U;
    frame.stdinfo.total_width = kBootWidth;
    frame.stdinfo.total_height = kBootHeight;
    frame.stdinfo.transparent_flag = 1U;
    frame.top_down_img0000 = true;
    frame.pixels = ama::imaging::MakeFilledBgraImage(kBootWidth, kBootHeight, fill);
    container.frames.push_back(std::move(frame));
    return ama::hg3::Hg3Codec::Encode(container);
}

}  // namespace

TEST(DomainRendererUt, ReadStandaloneFilePrefersBakOverMutatedLiveFileForRootAndNestedPaths) {
    const ScopedTempDir dir;
    const std::vector<std::uint8_t> retail_xml = MakePattern(kStandaloneSampleBytes, 0x11U);
    const std::vector<std::uint8_t> patched_xml = MakePattern(kStandaloneSampleBytes + 16U, 0x99U);
    WriteBytes(dir.path() / "config" / "startup.xml", patched_xml);
    WriteBytes(dir.path() / "config" / "startup.xml.bak", retail_xml);

    const ama::menu::GameAssetSource source(dir.path(), std::string(kSyntheticVCode2));
    const ama::menu::ExtractedAssetEntry entry =
        source.ReadStandaloneFile("config\\startup.xml");

    EXPECT_EQ(entry.archive_rel_path, "config\\startup.xml");
    EXPECT_EQ(entry.entry_name, "startup.xml");
    EXPECT_EQ(entry.orig_offset, 0U);
    EXPECT_EQ(entry.plain_bytes, retail_xml);
    EXPECT_EQ(entry.orig_raw_slice, retail_xml);

    // boot.hg3 loaded through LoadHg3 also reads boot.hg3.bak when present.
    const ama::imaging::BgraColor retail_color{10, 20, 30, 255};
    const ama::imaging::BgraColor patched_color{200, 210, 220, 255};
    const std::vector<std::uint8_t> retail_boot = MakeSyntheticBootHg3(retail_color);
    const std::vector<std::uint8_t> patched_boot = MakeSyntheticBootHg3(patched_color);
    WriteBytes(dir.path() / "boot.hg3", patched_boot);
    WriteBytes(dir.path() / "boot.hg3.bak", retail_boot);

    ama::menu::ExtractedAssetEntry boot_entry;
    const ama::hg3::Hg3Container loaded = source.LoadHg3("boot.hg3", &boot_entry);
    EXPECT_EQ(boot_entry.archive_rel_path, "boot.hg3");
    EXPECT_EQ(boot_entry.plain_bytes, retail_boot);
    ASSERT_EQ(loaded.frames.size(), 1U);
    EXPECT_EQ(ama::imaging::GetPixel(loaded.frames[0].pixels, 0, 0), retail_color);
}

TEST(DomainRendererUt, ReadStandaloneFileFallsBackWhenBakMissingOrUndersizedAndEnforcesMinSlice) {
    const ScopedTempDir dir;
    const std::vector<std::uint8_t> live_exact = MakePattern(kMinSliceSize, 0x42U);
    WriteBytes(dir.path() / "config" / "soundconf.xml", live_exact);

    const ama::menu::GameAssetSource source(dir.path(), std::string(kSyntheticVCode2));

    // Absent .bak reads live file at exact kMinOrigSliceBytes boundary (64 B).
    const ama::menu::ExtractedAssetEntry without_bak =
        source.ReadStandaloneFile("config\\soundconf.xml");
    EXPECT_EQ(without_bak.plain_bytes, live_exact);
    EXPECT_EQ(without_bak.orig_raw_slice, live_exact);

    // Truncated 63-byte .bak is ignored in favor of valid live file.
    WriteBytes(dir.path() / "config" / "soundconf.xml.bak",
               MakePattern(kBelowMinSliceSize, 0xEEU));
    const ama::menu::ExtractedAssetEntry with_short_bak =
        source.ReadStandaloneFile("config\\soundconf.xml");
    EXPECT_EQ(with_short_bak.plain_bytes, live_exact);

    // Undersized live file (63 bytes) with no valid .bak throws.
    WriteBytes(dir.path() / "config" / "tiny.xml", MakePattern(kBelowMinSliceSize, 0x55U));
    EXPECT_THROW((void)source.ReadStandaloneFile("config\\tiny.xml"), std::runtime_error);
}
