// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for FVP NVSG image decoding/encoding, GraphArchive TOC parsing
// and repacking, ImageCanvas pixel primitives, and UI font rendering.
#include "fvp/fvp_graph.h"
#include "ui/ui_patcher.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace fav;

namespace {

constexpr uint32_t kByteMask = 0xFFu;
constexpr unsigned int kOneByteShift = 8u;
constexpr unsigned int kTwoBytesShift = 16u;
constexpr unsigned int kThreeBytesShift = 24u;
constexpr size_t kMagicFourByteLen = 4u;
constexpr size_t kHeaderSizeOffset = 4u;
constexpr size_t kTocOffsetFieldOffset = 4u;
constexpr size_t kTocSizeFieldOffset = 8u;
constexpr uint32_t kThreeArchiveEntries = 3u;
constexpr uint32_t kSingleArchiveEntry = 1u;

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void write_u32_le(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & kByteMask);
    p[1] = static_cast<uint8_t>((v >> kOneByteShift) & kByteMask);
    p[2] = static_cast<uint8_t>((v >> kTwoBytesShift) & kByteMask);
    p[3] = static_cast<uint8_t>((v >> kThreeBytesShift) & kByteMask);
}

}  // namespace

TEST(FvpGraph, nvsg_round_trip_bgr_and_bgra) {
    for (uint16_t fmt : {fvp::kNvsgFormatBgr24, fvp::kNvsgFormatBgra32}) {
        fvp::NvsgImage src;
        src.format = fmt;
        src.width = 32;
        src.height = 16;
        src.offset_x = 5;
        src.offset_y = 7;
        src.frame_count = 1;
        const size_t bpp = src.bytes_per_pixel();
        ASSERT_EQ(bpp, (fmt == fvp::kNvsgFormatBgr24)
                           ? fvp::kBgr24BytesPerPixel
                           : fvp::kBgra32BytesPerPixel);
        src.pixels.resize(src.expected_byte_size());
        for (size_t i = 0; i < src.pixels.size(); ++i) {
            src.pixels[i] = static_cast<uint8_t>((i * 37u + 13u) & kByteMask);
        }

        EXPECT_TRUE(src.in_bounds(0, 0));
        EXPECT_TRUE(src.in_bounds(31, 15));
        EXPECT_FALSE(src.in_bounds(-1, 0));
        EXPECT_FALSE(src.in_bounds(32, 0));
        EXPECT_FALSE(src.in_bounds(0, 16));

        const auto encoded = fvp::encode_nvsg(src);
        ASSERT_GE(encoded.size(), fvp::kCompressedStreamHeaderByteCount);
        EXPECT_EQ(std::memcmp(encoded.data(), "hzc1", kMagicFourByteLen), 0);
        EXPECT_EQ(std::memcmp(encoded.data() + fvp::kHzc1EnvelopeHeaderByteCount,
                              "NVSG", kMagicFourByteLen),
                  0);

        const auto decoded = fvp::decode_nvsg(encoded);
        EXPECT_EQ(decoded.format, src.format);
        EXPECT_EQ(decoded.width, src.width);
        EXPECT_EQ(decoded.height, src.height);
        EXPECT_EQ(decoded.offset_x, src.offset_x);
        EXPECT_EQ(decoded.offset_y, src.offset_y);
        EXPECT_EQ(decoded.frame_count, src.frame_count);
        EXPECT_EQ(decoded.pixels, src.pixels);
    }
}

TEST(FvpGraph, archive_toc_and_repack_round_trip) {
    const ScopedTempDir tmp_dir("fav_ut_graph_repack");
    const std::string src_bin = (tmp_dir.path() / "src.bin").u8string();
    const std::string dst_bin = (tmp_dir.path() / "dst.bin").u8string();

    const std::string name_pool_str =
        std::string("first\0", 6) + std::string("middle\0", 7) + std::string("last\0", 5);
    const std::vector<uint8_t> d0 = {1, 2, 3, 4};
    const std::vector<uint8_t> d1 = {10, 20, 30, 40, 50};
    const std::vector<uint8_t> d2 = {99, 98, 97};

    {
        std::ofstream out(fs::u8path(src_bin), std::ios::binary | std::ios::trunc);
        uint8_t hdr[fvp::kArchiveHeaderByteCount]{};
        write_u32_le(hdr, kThreeArchiveEntries);
        write_u32_le(hdr + kHeaderSizeOffset,
                     static_cast<uint32_t>(name_pool_str.size()));
        out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));

        const uint32_t data_base =
            static_cast<uint32_t>(fvp::kArchiveHeaderByteCount +
                                  kThreeArchiveEntries * fvp::kTocEntryByteCount +
                                  name_pool_str.size());
        uint8_t toc[kThreeArchiveEntries * fvp::kTocEntryByteCount]{};
        write_u32_le(toc, 0u);
        write_u32_le(toc + kTocOffsetFieldOffset, data_base);
        write_u32_le(toc + kTocSizeFieldOffset, static_cast<uint32_t>(d0.size()));

        uint8_t* const toc_1 = toc + fvp::kTocEntryByteCount;
        write_u32_le(toc_1, 6u);
        write_u32_le(toc_1 + kTocOffsetFieldOffset,
                     data_base + static_cast<uint32_t>(d0.size()));
        write_u32_le(toc_1 + kTocSizeFieldOffset, static_cast<uint32_t>(d1.size()));

        uint8_t* const toc_2 = toc + 2u * fvp::kTocEntryByteCount;
        write_u32_le(toc_2, 13u);
        write_u32_le(toc_2 + kTocOffsetFieldOffset,
                     data_base + static_cast<uint32_t>(d0.size() + d1.size()));
        write_u32_le(toc_2 + kTocSizeFieldOffset, static_cast<uint32_t>(d2.size()));
        out.write(reinterpret_cast<const char*>(toc), sizeof(toc));

        out.write(name_pool_str.data(),
                  static_cast<std::streamsize>(name_pool_str.size()));
        out.write(reinterpret_cast<const char*>(d0.data()),
                  static_cast<std::streamsize>(d0.size()));
        out.write(reinterpret_cast<const char*>(d1.data()),
                  static_cast<std::streamsize>(d1.size()));
        out.write(reinterpret_cast<const char*>(d2.data()),
                  static_cast<std::streamsize>(d2.size()));
    }

    const std::vector<uint8_t> d1_replacement = {7, 8, 9, 10, 11, 12, 13, 14, 15};
    std::unordered_map<std::string, std::vector<uint8_t>> repl;
    repl["middle"] = d1_replacement;

    fvp::repack_archive(src_bin, dst_bin, repl);

    const auto toc = fvp::read_archive_toc(dst_bin);
    ASSERT_EQ(toc.file_count, kThreeArchiveEntries);
    EXPECT_EQ(toc.entries[0].name, "first");
    EXPECT_EQ(toc.entries[1].name, "middle");
    EXPECT_EQ(toc.entries[2].name, "last");

    EXPECT_EQ(toc.entries[0].data_size, d0.size());
    EXPECT_EQ(toc.entries[1].data_size, d1_replacement.size());
    EXPECT_EQ(toc.entries[2].data_size, d2.size());
    EXPECT_EQ(toc.entries[1].data_offset,
              toc.entries[0].data_offset + toc.entries[0].data_size);
    EXPECT_EQ(toc.entries[2].data_offset,
              toc.entries[1].data_offset + toc.entries[1].data_size);

    EXPECT_EQ(fvp::extract_entry(dst_bin, toc.entries[0]), d0);
    EXPECT_EQ(fvp::extract_entry(dst_bin, toc.entries[1]), d1_replacement);
    EXPECT_EQ(fvp::extract_entry(dst_bin, toc.entries[2]), d2);
}

TEST(FvpGraph, graph_archive_pimpl_lookup_and_repack) {
    const ScopedTempDir tmp_dir("fav_ut_graph_archive_pimpl");
    const std::string src_bin = (tmp_dir.path() / "src.bin").u8string();
    const std::string dst_bin = (tmp_dir.path() / "dst.bin").u8string();

    fvp::NvsgImage sample_img;
    sample_img.format = fvp::kNvsgFormatBgra32;
    sample_img.width = 8;
    sample_img.height = 4;
    sample_img.pixels.assign(sample_img.expected_byte_size(), 128u);
    const std::vector<uint8_t> encoded_img = fvp::encode_nvsg(sample_img);

    const std::string name_pool_str = std::string("tex_a\0", 6);
    {
        std::ofstream out(fs::u8path(src_bin), std::ios::binary | std::ios::trunc);
        uint8_t hdr[fvp::kArchiveHeaderByteCount]{};
        write_u32_le(hdr, kSingleArchiveEntry);
        write_u32_le(hdr + kHeaderSizeOffset,
                     static_cast<uint32_t>(name_pool_str.size()));
        out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));

        const uint32_t data_base =
            static_cast<uint32_t>(fvp::kArchiveHeaderByteCount +
                                  fvp::kTocEntryByteCount +
                                  name_pool_str.size());
        uint8_t toc[fvp::kTocEntryByteCount]{};
        write_u32_le(toc, 0u);
        write_u32_le(toc + kTocOffsetFieldOffset, data_base);
        write_u32_le(toc + kTocSizeFieldOffset,
                     static_cast<uint32_t>(encoded_img.size()));
        out.write(reinterpret_cast<const char*>(toc), sizeof(toc));
        out.write(name_pool_str.data(),
                  static_cast<std::streamsize>(name_pool_str.size()));
        out.write(reinterpret_cast<const char*>(encoded_img.data()),
                  static_cast<std::streamsize>(encoded_img.size()));
    }

    fvp::GraphArchive archive(src_bin);
    fvp::GraphArchive moved_archive = std::move(archive);

    EXPECT_EQ(moved_archive.path(), src_bin);
    EXPECT_EQ(moved_archive.header().file_count, kSingleArchiveEntry);
    EXPECT_TRUE(moved_archive.contains("tex_a"));
    EXPECT_FALSE(moved_archive.contains("nonexistent"));
    ASSERT_NE(moved_archive.find_entry("tex_a"), nullptr);
    EXPECT_EQ(moved_archive.find_entry("nonexistent"), nullptr);

    const fvp::NvsgImage loaded = moved_archive.load_image("tex_a");
    EXPECT_EQ(loaded.width, 8u);
    EXPECT_EQ(loaded.height, 4u);
    EXPECT_EQ(loaded.pixels, sample_img.pixels);

    fvp::NvsgImage modified = loaded;
    std::fill(modified.pixels.begin(), modified.pixels.end(), 250u);
    std::unordered_map<std::string, std::vector<uint8_t>> replacements;
    replacements["tex_a"] = fvp::encode_nvsg(modified);
    moved_archive.repack_to(dst_bin, replacements);

    const fvp::GraphArchive repacked(dst_bin);
    const fvp::NvsgImage reloaded = repacked.load_image("tex_a");
    EXPECT_EQ(reloaded.pixels, modified.pixels);
}

TEST(FvpGraph, image_canvas_primitives) {
    fvp::NvsgImage bgra;
    bgra.format = fvp::kNvsgFormatBgra32;
    bgra.width = 10;
    bgra.height = 10;
    bgra.pixels.assign(bgra.expected_byte_size(), 0u);

    ui::ImageCanvas canvas(bgra);
    EXPECT_EQ(canvas.width(), 10);
    EXPECT_EQ(canvas.height(), 10);

    canvas.fill_rect(ui::Rect{2, 3, 4, 2}, ui::Color{200, 100, 50, 255});
    const uint8_t* p_in = bgra.pixel_ptr(3, 4);
    EXPECT_EQ(p_in[0], 50u);
    EXPECT_EQ(p_in[1], 100u);
    EXPECT_EQ(p_in[2], 200u);
    EXPECT_EQ(p_in[3], 255u);

    canvas.copy_vertical_offset(ui::Rect{2, 6, 4, 2}, -3);
    const uint8_t* p_copied = bgra.pixel_ptr(3, 7);
    EXPECT_EQ(p_copied[0], 50u);
    EXPECT_EQ(p_copied[1], 100u);
    EXPECT_EQ(p_copied[2], 200u);
    EXPECT_EQ(p_copied[3], 255u);

    canvas.blend_pixel(0, 0, ui::Color{255, 128, 64, 255}, 255u);
    const uint8_t* p_blended = bgra.pixel_ptr(0, 0);
    EXPECT_EQ(p_blended[0], 64u);
    EXPECT_EQ(p_blended[1], 128u);
    EXPECT_EQ(p_blended[2], 255u);
    EXPECT_EQ(p_blended[3], 255u);

    canvas.clear_top_rows(5);
    EXPECT_EQ(bgra.pixel_ptr(3, 4)[3], 0u);
    EXPECT_EQ(bgra.pixel_ptr(3, 7)[3], 255u);

    fvp::NvsgImage bgr;
    bgr.format = fvp::kNvsgFormatBgr24;
    bgr.width = 10;
    bgr.height = 6;
    bgr.pixels.assign(bgr.expected_byte_size(), 0u);
    ui::ImageCanvas bgr_canvas(bgr);
    bgr_canvas.fill_rect(ui::Rect{0, 0, 2, 6}, ui::Color{10, 20, 30, 255});
    bgr_canvas.fill_rect(ui::Rect{8, 0, 2, 6}, ui::Color{110, 120, 130, 255});
    bgr_canvas.interpolate_horizontal(ui::Rect{2, 0, 6, 6});
    const uint8_t* mid = bgr.pixel_ptr(5, 2);
    EXPECT_GT(mid[0], 30u);
    EXPECT_LT(mid[0], 130u);
}

TEST(FvpGraph, font_renders_onto_bgr_and_bgra_canvases) {
    ui::Font font;
    ui::Font moved_font = std::move(font);
    EXPECT_GT(moved_font.measure_width("Settings", 15, true), 20);

    fvp::NvsgImage bgr;
    bgr.format = fvp::kNvsgFormatBgr24;
    bgr.width = 120;
    bgr.height = 30;
    bgr.pixels.assign(bgr.expected_byte_size(), 0);
    ui::ImageCanvas bgr_canvas(bgr);
    moved_font.draw_centered(
        bgr_canvas, ui::Rect{0, 0, 120, 30}, "Settings",
        ui::TextStyle{15, true, ui::Color::white(), {25, 15, 10, 200}, 1});
    bool bgr_modified = false;
    for (uint8_t b : bgr.pixels) {
        if (b != 0) {
            bgr_modified = true;
            break;
        }
    }
    EXPECT_TRUE(bgr_modified);

    fvp::NvsgImage bgra;
    bgra.format = fvp::kNvsgFormatBgra32;
    bgra.width = 120;
    bgra.height = 30;
    bgra.pixels.assign(bgra.expected_byte_size(), 0);
    ui::ImageCanvas bgra_canvas(bgra);
    moved_font.draw_left(
        bgra_canvas, ui::Rect{4, 4, 112, 22}, "Option",
        ui::TextStyle{14, true, ui::Color::white(), {25, 15, 10, 200}, 1});
    bool bgra_modified = false;
    for (size_t i = 3; i < bgra.pixels.size(); i += fvp::kBgra32BytesPerPixel) {
        if (bgra.pixels[i] > 0) {
            bgra_modified = true;
            break;
        }
    }
    EXPECT_TRUE(bgra_modified);
}
