// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "fvp/fvp_graph.h"
#include "ui/ui_patcher.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace fav;

namespace {

constexpr uint32_t kByteMask = 0xFFu;
constexpr unsigned int kOneByteShift = 8u;
constexpr unsigned int kTwoBytesShift = 16u;
constexpr unsigned int kThreeBytesShift = 24u;

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
        EXPECT_EQ(std::memcmp(encoded.data(), "hzc1", 4), 0);
        EXPECT_EQ(std::memcmp(encoded.data() + fvp::kHzc1EnvelopeHeaderByteCount,
                              "NVSG", 4),
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
    const fs::path tmp_dir = fs::temp_directory_path() / "fav_ut_graph_repack";
    fs::create_directories(tmp_dir);
    const std::string src_bin = (tmp_dir / "src.bin").u8string();
    const std::string dst_bin = (tmp_dir / "dst.bin").u8string();

    const std::string name_pool_str =
        std::string("first\0", 6) + std::string("middle\0", 7) + std::string("last\0", 5);
    const std::vector<uint8_t> d0 = {1, 2, 3, 4};
    const std::vector<uint8_t> d1 = {10, 20, 30, 40, 50};
    const std::vector<uint8_t> d2 = {99, 98, 97};

    {
        std::ofstream out(fs::u8path(src_bin), std::ios::binary | std::ios::trunc);
        uint8_t hdr[fvp::kArchiveHeaderByteCount]{};
        write_u32_le(hdr, 3u);
        write_u32_le(hdr + 4, static_cast<uint32_t>(name_pool_str.size()));
        out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));

        const uint32_t data_base =
            static_cast<uint32_t>(fvp::kArchiveHeaderByteCount +
                                  3u * fvp::kTocEntryByteCount +
                                  name_pool_str.size());
        uint8_t toc[3 * fvp::kTocEntryByteCount]{};
        write_u32_le(toc + 0, 0u);
        write_u32_le(toc + 4, data_base);
        write_u32_le(toc + 8, static_cast<uint32_t>(d0.size()));

        write_u32_le(toc + 12, 6u);
        write_u32_le(toc + 16, data_base + static_cast<uint32_t>(d0.size()));
        write_u32_le(toc + 20, static_cast<uint32_t>(d1.size()));

        write_u32_le(toc + 24, 13u);
        write_u32_le(toc + 28,
                     data_base + static_cast<uint32_t>(d0.size() + d1.size()));
        write_u32_le(toc + 32, static_cast<uint32_t>(d2.size()));
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
    ASSERT_EQ(toc.file_count, 3u);
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

    fs::remove_all(tmp_dir);
}

TEST(FvpGraph, graph_archive_pimpl_lookup_and_repack) {
    const fs::path tmp_dir = fs::temp_directory_path() / "fav_ut_graph_archive_pimpl";
    fs::create_directories(tmp_dir);
    const std::string src_bin = (tmp_dir / "src.bin").u8string();
    const std::string dst_bin = (tmp_dir / "dst.bin").u8string();

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
        write_u32_le(hdr, 1u);
        write_u32_le(hdr + 4, static_cast<uint32_t>(name_pool_str.size()));
        out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));

        const uint32_t data_base =
            static_cast<uint32_t>(fvp::kArchiveHeaderByteCount +
                                  fvp::kTocEntryByteCount +
                                  name_pool_str.size());
        uint8_t toc[fvp::kTocEntryByteCount]{};
        write_u32_le(toc + 0, 0u);
        write_u32_le(toc + 4, data_base);
        write_u32_le(toc + 8, static_cast<uint32_t>(encoded_img.size()));
        out.write(reinterpret_cast<const char*>(toc), sizeof(toc));
        out.write(name_pool_str.data(),
                  static_cast<std::streamsize>(name_pool_str.size()));
        out.write(reinterpret_cast<const char*>(encoded_img.data()),
                  static_cast<std::streamsize>(encoded_img.size()));
    }

    fvp::GraphArchive archive(src_bin);
    fvp::GraphArchive moved_archive = std::move(archive);

    EXPECT_EQ(moved_archive.path(), src_bin);
    EXPECT_EQ(moved_archive.header().file_count, 1u);
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

    fs::remove_all(tmp_dir);
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

TEST(FvpGraph, ui_patcher_patches_real_graph_bin_entries) {
    const std::string bin_path = "C:\\Favorite\\graph.bin";
    const std::string backup_path = "C:\\Favorite\\graph.bin.backup";
    if (!fs::exists(fs::u8path(bin_path)) || !fs::exists(fs::u8path(backup_path))) {
        GTEST_SKIP() << "C:\\Favorite\\graph.bin(.backup) not installed";
    }
    const fvp::GraphArchive deployed(bin_path);
    const fvp::GraphArchive backup(backup_path);
    EXPECT_EQ(deployed.header().file_count, 1146u);

    std::vector<const fvp::ArchiveEntry*> ui_entries;
    for (const auto& e : backup.header().entries) {
        if (e.name.rfind("menu_", 0) == 0 ||
            e.name.rfind("opt_", 0) == 0 ||
            e.name.rfind("fc_", 0) == 0 ||
            e.name.rfind("sys_", 0) == 0) {
            ui_entries.push_back(&e);
        }
    }

    const fs::path tmp_dir = fs::temp_directory_path() / "fav_ut_ui_patcher_facade";
    fs::create_directories(tmp_dir);
    const std::string mini_bin = (tmp_dir / "graph.bin").u8string();

    {
        std::string name_pool;
        std::vector<uint32_t> name_offsets;
        std::vector<std::vector<uint8_t>> payloads;
        for (const fvp::ArchiveEntry* e : ui_entries) {
            name_offsets.push_back(static_cast<uint32_t>(name_pool.size()));
            name_pool.append(e->name);
            name_pool.push_back('\0');
            payloads.push_back(backup.extract_raw(e->name));
        }

        std::ofstream out(fs::u8path(mini_bin), std::ios::binary | std::ios::trunc);
        uint8_t hdr[fvp::kArchiveHeaderByteCount]{};
        const uint32_t count = static_cast<uint32_t>(ui_entries.size());
        write_u32_le(hdr, count);
        write_u32_le(hdr + 4, static_cast<uint32_t>(name_pool.size()));
        out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));

        uint32_t offset = static_cast<uint32_t>(
            fvp::kArchiveHeaderByteCount +
            static_cast<size_t>(count) * fvp::kTocEntryByteCount +
            name_pool.size());
        std::vector<uint8_t> toc(static_cast<size_t>(count) * fvp::kTocEntryByteCount, 0);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t* p = toc.data() + static_cast<size_t>(i) * fvp::kTocEntryByteCount;
            write_u32_le(p + 0, name_offsets[i]);
            write_u32_le(p + 4, offset);
            write_u32_le(p + 8, static_cast<uint32_t>(payloads[i].size()));
            offset += static_cast<uint32_t>(payloads[i].size());
        }
        out.write(reinterpret_cast<const char*>(toc.data()),
                  static_cast<std::streamsize>(toc.size()));
        out.write(name_pool.data(), static_cast<std::streamsize>(name_pool.size()));
        for (const auto& payload : payloads) {
            out.write(reinterpret_cast<const char*>(payload.data()),
                      static_cast<std::streamsize>(payload.size()));
        }
    }

    ui::UiPatcher patcher;
    ui::UiPatcher moved_patcher = std::move(patcher);
    ui::PatchUiOptions opts;
    opts.game_dir = tmp_dir.u8string();
    ASSERT_EQ(moved_patcher.patch_archive(opts), 0);
    EXPECT_TRUE(fs::exists(tmp_dir / "graph.bin.backup"));

    const fvp::GraphArchive patched_mini(mini_bin);
    for (const char* req :
         {"opt_sheet1", "opt_sheet2", "opt_sheet3", "opt_parts", "menu_bg",
          "menu_exit_mes", "menu_yes_1", "menu_no_1", "opt_s_pts1", "opt_m_pts2",
          "opt_001", "opt_117", "opt_201", "fc_back_help", "sys_config_M"}) {
        ASSERT_TRUE(patched_mini.contains(req)) << "Missing UI entry: " << req;
        const auto actual_img = patched_mini.load_image(req);
        const auto expected_img = deployed.load_image(req);
        EXPECT_EQ(actual_img.width, expected_img.width) << req;
        EXPECT_EQ(actual_img.height, expected_img.height) << req;
        EXPECT_EQ(actual_img.pixels, expected_img.pixels)
            << "Pixel mismatch on patched UI entry: " << req;
    }

    fs::remove_all(tmp_dir);
}
