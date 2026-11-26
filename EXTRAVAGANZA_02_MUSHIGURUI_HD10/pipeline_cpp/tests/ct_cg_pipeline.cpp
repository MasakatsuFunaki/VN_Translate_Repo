// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests driving mgi::cg::run_find_narrative_cg through its public
// CgOptions facade across multi-archive repacks (PACKTYPE=8A RGBA PNG +
// sparse-palette PACKTYPE=2A masked BMP), pristine .backup extraction,
// shrinking-set & empty-set backup restoration, and strict all-or-nothing
// failure on incomplete backup pairs.
#include "cg/cg_pipeline.h"

#include "cg/image.h"
#include "common/util.h"
#include "gpk/gpk_archive.h"
#include "scoped_temp_dir.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bj = boost::json;
namespace fs = std::filesystem;
using namespace mgi;

namespace {

constexpr std::size_t kBmpHdrBytes = 54u;
constexpr std::uint32_t kByteShift1 = 8u;
constexpr std::uint32_t kByteShift2 = 16u;
constexpr std::uint32_t kByteShift3 = 24u;

void put_u16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> kByteShift1));
}

void put_u32(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> kByteShift1));
    b.push_back(static_cast<std::uint8_t>(v >> kByteShift2));
    b.push_back(static_cast<std::uint8_t>(v >> kByteShift3));
}

void put_i32(Bytes& b, std::int32_t v) {
    put_u32(b, static_cast<std::uint32_t>(v));
}

std::uint32_t rd_u32(const Bytes& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1u]) << kByteShift1) |
           (static_cast<std::uint32_t>(b[off + 2u]) << kByteShift2) |
           (static_cast<std::uint32_t>(b[off + 3u]) << kByteShift3);
}

std::int32_t rd_i32(const Bytes& b, std::size_t off) {
    return static_cast<std::int32_t>(rd_u32(b, off));
}

cg::Image make_rgba_box(int w, int h,
                        int x0, int y0, int x1, int y1,
                        std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    cg::Image img;
    img.width = w;
    img.height = h;
    img.channels = 4;
    img.px.assign(static_cast<std::size_t>(w) * h * 4u, 0u);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            std::uint8_t* p = img.at(x, y);
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = a;
        }
    }
    return img;
}

Bytes make_8a_entry(const cg::Image& rgba) {
    const Bytes png = cg::encode_png(rgba);
    gpk::EntryHeader hdr;
    std::memcpy(hdr.type_raw.data(), "PNG             ", 16u);
    hdr.width = static_cast<std::uint32_t>(rgba.width);
    hdr.height = static_cast<std::uint32_t>(rgba.height);
    hdr.bpp = 0u;
    hdr.packtype = gpk::PACKTYPE_DEFAULT;
    return gpk::build_entry(hdr, png);
}

Bytes make_sparse_2a_entry(const cg::Image& rgba,
                           const std::vector<std::uint8_t>& sparse_pal) {
    const std::size_t stride =
        ((static_cast<std::size_t>(rgba.width) * 3u) + 3u) & ~std::size_t(3u);
    const std::size_t bmp_px = stride * static_cast<std::size_t>(rgba.height);
    const std::size_t bmp_total = kBmpHdrBytes + bmp_px;

    Bytes dwq(gpk::GPK_HEADER_SIZE, ' ');
    std::memcpy(dwq.data(), "BMP+MASK        ", 16u);
    for (std::size_t i = 0x10u; i < 0x20u; ++i) {
        dwq[i] = 0u;
    }
    const auto wr32 = [&](std::size_t o, std::uint32_t v) {
        dwq[o] = static_cast<std::uint8_t>(v);
        dwq[o + 1u] = static_cast<std::uint8_t>(v >> kByteShift1);
        dwq[o + 2u] = static_cast<std::uint8_t>(v >> kByteShift2);
        dwq[o + 3u] = static_cast<std::uint8_t>(v >> kByteShift3);
    };
    const auto wr16 = [&](std::size_t o, std::uint16_t v) {
        dwq[o] = static_cast<std::uint8_t>(v);
        dwq[o + 1u] = static_cast<std::uint8_t>(v >> kByteShift1);
    };
    wr32(0x20u, static_cast<std::uint32_t>(bmp_total));
    wr32(0x24u, static_cast<std::uint32_t>(rgba.width));
    wr16(0x28u, static_cast<std::uint16_t>(rgba.height));
    wr16(0x2Au, 24u);
    std::memcpy(dwq.data() + 0x30u, "PACKTYPE=2A     ", 16u);

    dwq.push_back('B');
    dwq.push_back('M');
    put_u32(dwq, static_cast<std::uint32_t>(bmp_total));
    put_u16(dwq, 0u);
    put_u16(dwq, 0u);
    put_u32(dwq, static_cast<std::uint32_t>(kBmpHdrBytes));
    put_u32(dwq, 40u);
    put_i32(dwq, rgba.width);
    put_i32(dwq, rgba.height);
    put_u16(dwq, 1u);
    put_u16(dwq, 24u);
    put_u32(dwq, 0u);
    put_u32(dwq, static_cast<std::uint32_t>(bmp_px));
    put_i32(dwq, 0);
    put_i32(dwq, 0);
    put_u32(dwq, 0u);
    put_u32(dwq, 0u);

    Bytes bgr(bmp_px, 0u);
    for (int y = 0; y < rgba.height; ++y) {
        for (int x = 0; x < rgba.width; ++x) {
            const std::uint8_t* p = rgba.at(x, y);
            const std::size_t di = static_cast<std::size_t>(y) * stride + x * 3u;
            bgr[di + 0u] = p[2];
            bgr[di + 1u] = p[1];
            bgr[di + 2u] = p[0];
        }
    }
    dwq.insert(dwq.end(), bgr.begin(), bgr.end());

    const std::size_t total_px =
        static_cast<std::size_t>(rgba.width) * static_cast<std::size_t>(rgba.height);
    Bytes idx(total_px, 0u);
    for (std::size_t i = 0; i < total_px; ++i) {
        const std::uint8_t a = rgba.px[i * 4u + 3u];
        for (std::size_t k = 0; k < sparse_pal.size(); ++k) {
            if (sparse_pal[k] == a) {
                idx[i] = static_cast<std::uint8_t>(k);
                break;
            }
        }
    }
    const Bytes rle = gpk::encode_rle_xor_mask(idx.data(), rgba.width, rgba.height);
    const std::uint32_t bf_off =
        static_cast<std::uint32_t>(kBmpHdrBytes + sparse_pal.size() * 4u);
    dwq.push_back('B');
    dwq.push_back('M');
    put_u32(dwq, static_cast<std::uint32_t>(bf_off + total_px));
    put_u16(dwq, 0u);
    put_u16(dwq, 0u);
    put_u32(dwq, bf_off);
    put_u32(dwq, 40u);
    put_i32(dwq, rgba.width);
    put_i32(dwq, rgba.height);
    put_u16(dwq, 1u);
    put_u16(dwq, 8u);
    put_u32(dwq, 0u);
    put_u32(dwq, static_cast<std::uint32_t>(total_px));
    put_i32(dwq, 0);
    put_i32(dwq, 0);
    put_u32(dwq, static_cast<std::uint32_t>(sparse_pal.size()));
    put_u32(dwq, 0u);
    for (std::uint8_t v : sparse_pal) {
        dwq.push_back(v);
        dwq.push_back(v);
        dwq.push_back(v);
        dwq.push_back(0u);
    }
    dwq.insert(dwq.end(), rle.begin(), rle.end());
    return dwq;
}

void write_archive(const std::string& gpk_path,
                   const std::string& gtb_path,
                   const std::vector<std::pair<std::string, Bytes>>& chunks) {
    Bytes gpk_buf;
    std::vector<gpk::GtbEntry> entries;
    entries.reserve(chunks.size());
    for (const auto& [name, chunk] : chunks) {
        entries.push_back({name, static_cast<std::uint64_t>(gpk_buf.size())});
        gpk_buf.insert(gpk_buf.end(), chunk.begin(), chunk.end());
    }
    write_file(gpk_path, gpk_buf);
    gpk::write_gtb(gtb_path, entries, true);
}

class ScopedCinRedirect {
public:
    explicit ScopedCinRedirect(const std::string& input)
        : stream_(input), old_(std::cin.rdbuf(stream_.rdbuf())) {}
    ~ScopedCinRedirect() { std::cin.rdbuf(old_); }
    ScopedCinRedirect(const ScopedCinRedirect&) = delete;
    ScopedCinRedirect& operator=(const ScopedCinRedirect&) = delete;

private:
    std::istringstream stream_;
    std::streambuf* old_;
};

}  // namespace

TEST(CgPipelineCt, MultiArchiveRepackShrinkingSetRestoreAndPristineBackupExtract) {
    const test_util::ScopedTempDir sandbox("mgi_ct_cg_pipeline");
    const std::string game_dir = sandbox.join_u8("game");
    const std::string dwq_dir = game_dir + "\\dwq";
    const std::string proj_dir = sandbox.join_u8("proj");
    const std::string out_dir = proj_dir + "\\script_output";
    const std::string patched_dir = out_dir + "\\narrative_patched";
    fs::create_directories(fs::u8path(dwq_dir));
    fs::create_directories(fs::u8path(patched_dir));

    // Build pristine ev0 (8A RGBA PNG + sparse-palette 2A masked BMP + untouched)
    // and ta0 (8A RGBA PNG + untouched).
    const cg::Image orig_ev_png = make_rgba_box(16, 12, 3, 2, 12, 9, 30u, 80u, 150u, 220u);
    const cg::Image orig_ev_2a = make_rgba_box(12, 10, 2, 2, 9, 7, 40u, 160u, 60u, 0x80u);
    const cg::Image orig_ev_keep = make_rgba_box(8, 8, 0, 0, 7, 7, 10u, 20u, 30u, 255u);
    const cg::Image orig_ta_png = make_rgba_box(14, 10, 2, 1, 11, 8, 90u, 40u, 180u, 240u);
    const cg::Image orig_ta_keep = make_rgba_box(8, 8, 0, 0, 7, 7, 50u, 60u, 70u, 255u);

    const Bytes orig_ev_2a_chunk = make_sparse_2a_entry(orig_ev_2a, {0x00u, 0x80u, 0xFFu});
    const Bytes orig_ev_keep_chunk = make_8a_entry(orig_ev_keep);
    write_archive(dwq_dir + "\\ev0.gpk", dwq_dir + "\\ev0.gtb", {
        {"ev_png", make_8a_entry(orig_ev_png)},
        {"ev_mask2a", orig_ev_2a_chunk},
        {"ev_keep", orig_ev_keep_chunk},
    });
    write_archive(dwq_dir + "\\ta0.gpk", dwq_dir + "\\ta0.gtb", {
        {"ta_png", make_8a_entry(orig_ta_png)},
        {"ta_keep", make_8a_entry(orig_ta_keep)},
    });

    const Bytes pristine_ev_gpk = read_file(dwq_dir + "\\ev0.gpk");
    const Bytes pristine_ev_gtb = read_file(dwq_dir + "\\ev0.gtb");
    const Bytes pristine_ta_gpk = read_file(dwq_dir + "\\ta0.gpk");
    const Bytes pristine_ta_gtb = read_file(dwq_dir + "\\ta0.gtb");

    // Prepare patched 32-bpp RGBA PNGs with shifted bounds and new alpha bytes.
    const cg::Image new_ev_png = make_rgba_box(16, 12, 1, 1, 14, 10, 210u, 50u, 30u, 255u);
    const cg::Image new_ev_2a = make_rgba_box(12, 10, 1, 1, 10, 8, 220u, 110u, 20u, 0xC8u);
    const cg::Image new_ta_png = make_rgba_box(14, 10, 1, 2, 12, 7, 25u, 200u, 120u, 255u);

    const std::string p_ev_png = patched_dir + "\\ev_png.png";
    const std::string p_ev_2a = patched_dir + "\\ev_mask2a.png";
    const std::string p_ta_png = patched_dir + "\\ta_png.png";
    write_file(p_ev_png, cg::encode_png(new_ev_png));
    write_file(p_ev_2a, cg::encode_png(new_ev_2a));
    write_file(p_ta_png, cg::encode_png(new_ta_png));

    const std::string candidates_path = out_dir + "\\narrative_candidates.json";
    auto write_candidates = [&](const bj::array& arr) {
        write_file_text(candidates_path, json_pretty(bj::value(arr), 2));
    };

    cg::CgOptions opt;
    opt.game_dir = game_dir;
    opt.project_dir = proj_dir;
    opt.repack_only = true;

    // Pass 1: Repack {ev0: [ev_png, ev_mask2a], ta0: [ta_png]}.
    write_candidates(bj::array{
        bj::object{{"arc", "ev0.gpk"}, {"entry", "ev_png"}, {"patched_img", p_ev_png}},
        bj::object{{"arc", "ev0.gpk"}, {"entry", "ev_mask2a"}, {"patched_img", p_ev_2a}},
        bj::object{{"arc", "ta0.gpk"}, {"entry", "ta_png"}, {"patched_img", p_ta_png}},
    });
    ASSERT_EQ(cg::run_find_narrative_cg(opt), 0);

    // Pristine .backup pairs created for both archives, zero .tmp left behind.
    EXPECT_EQ(read_file(dwq_dir + "\\ev0.gpk.backup"), pristine_ev_gpk);
    EXPECT_EQ(read_file(dwq_dir + "\\ev0.gtb.backup"), pristine_ev_gtb);
    EXPECT_EQ(read_file(dwq_dir + "\\ta0.gpk.backup"), pristine_ta_gpk);
    EXPECT_EQ(read_file(dwq_dir + "\\ta0.gtb.backup"), pristine_ta_gtb);
    EXPECT_FALSE(fs::exists(fs::u8path(dwq_dir + "\\ev0.gpk.tmp")));
    EXPECT_FALSE(fs::exists(fs::u8path(dwq_dir + "\\ta0.gpk.tmp")));

    // Verify native PACKTYPE=2A + 8A + untouched sibling parity in live ev0.gpk.
    {
        const gpk::Archive live_ev = gpk::read_archive(dwq_dir + "\\ev0.gpk", dwq_dir + "\\ev0.gtb");
        ASSERT_EQ(live_ev.entries.size(), 3u);
        EXPECT_EQ(gpk::extract_entry_raw(live_ev, 2u), orig_ev_keep_chunk);

        // ev_png updated crop margins: x=1..14, y=1..10 on 16x12 -> {top=1, left=1, right=1, bottom=1}.
        const Bytes ev_png_raw = gpk::extract_entry_raw(live_ev, 0u);
        EXPECT_EQ(rd_i32(ev_png_raw, 0x10u), 1);
        EXPECT_EQ(rd_i32(ev_png_raw, 0x14u), 1);
        EXPECT_EQ(rd_i32(ev_png_raw, 0x18u), 1);
        EXPECT_EQ(rd_i32(ev_png_raw, 0x1Cu), 1);

        // ev_mask2a stayed native PACKTYPE=2A with mask BMP bfOffBits==1078 and exact alpha 0xC8.
        const Bytes ev_2a_raw = gpk::extract_entry_raw(live_ev, 1u);
        EXPECT_EQ(live_ev.entries[1].type_tag, "BMP+MASK");
        EXPECT_EQ(gpk::packtype_char(live_ev.entries[1]), '2');
        EXPECT_EQ(live_ev.entries[1].bpp, 24u);
        const std::size_t mask_off = gpk::GPK_HEADER_SIZE + live_ev.entries[1].packed_size;
        ASSERT_GT(ev_2a_raw.size(), mask_off + 1078u);
        EXPECT_EQ(rd_u32(ev_2a_raw, mask_off + 10u), 1078u);
        EXPECT_EQ(rd_u32(ev_2a_raw, mask_off + 46u), 256u);
        const gpk::DwqRgbaImage dec_2a =
            gpk::decode_dwq_masked_rgba(ev_2a_raw.data(), ev_2a_raw.size());
        EXPECT_EQ(dec_2a.pixels, new_ev_2a.px);
    }

    // Extraction after live archives were patched MUST read pristine .backup
    // (NOT mutated live .gpk) and decode PACKTYPE=2A to 32-bpp RGBA PNG.
    {
        cg::CgOptions extract_opt;
        extract_opt.game_dir = game_dir;
        extract_opt.project_dir = proj_dir;
        extract_opt.repack_only = false;
        ScopedCinRedirect abort_before_api("n\n");
        ASSERT_EQ(cg::run_find_narrative_cg(extract_opt), 0);

        const auto ext_ev_png = cg::open_image(out_dir + "\\narrative_extracted\\ev0\\ev_png.png");
        ASSERT_TRUE(ext_ev_png.has_value());
        EXPECT_EQ(ext_ev_png->px, orig_ev_png.px);  // pristine, NOT patched!

        const auto ext_ev_2a = cg::open_image(out_dir + "\\narrative_extracted\\ev0\\ev_mask2a.png");
        ASSERT_TRUE(ext_ev_2a.has_value());
        EXPECT_EQ(ext_ev_2a->channels, 4);
        EXPECT_EQ(ext_ev_2a->px, orig_ev_2a.px);  // full 2A alpha preserved from pristine .backup!
    }

    // Pass 2 (shrinking-set): remove ev_mask2a from ev0 AND remove ta0 completely.
    write_candidates(bj::array{
        bj::object{{"arc", "ev0.gpk"}, {"entry", "ev_png"}, {"patched_img", p_ev_png}},
    });
    ASSERT_EQ(cg::run_find_narrative_cg(opt), 0);

    // ta0 restored byte-for-byte from .backup; ev_mask2a restored byte-for-byte in ev0.
    EXPECT_EQ(read_file(dwq_dir + "\\ta0.gpk"), pristine_ta_gpk);
    EXPECT_EQ(read_file(dwq_dir + "\\ta0.gtb"), pristine_ta_gtb);
    {
        const gpk::Archive live_ev = gpk::read_archive(dwq_dir + "\\ev0.gpk", dwq_dir + "\\ev0.gtb");
        EXPECT_EQ(gpk::extract_entry_raw(live_ev, 1u), orig_ev_2a_chunk);
        const auto dec_ev_png = cg::decode_image(
            gpk::extract_entry(live_ev.gpk_buf, static_cast<std::size_t>(live_ev.entries[0].offset)));
        ASSERT_TRUE(dec_ev_png.has_value());
        EXPECT_EQ(dec_ev_png->px, new_ev_png.px);
    }

    // Pass 3 (empty candidates): ev0 restored byte-for-byte from .backup too.
    write_candidates(bj::array{});
    ASSERT_EQ(cg::run_find_narrative_cg(opt), 0);
    EXPECT_EQ(read_file(dwq_dir + "\\ev0.gpk"), pristine_ev_gpk);
    EXPECT_EQ(read_file(dwq_dir + "\\ev0.gtb"), pristine_ev_gtb);

    // Pass 4 (incomplete backup pair — missing ta0.gtb.backup): strict
    // all-or-nothing fails loudly without mutating ev0 or leaving .tmp files.
    fs::remove(fs::u8path(dwq_dir + "\\ta0.gtb.backup"));
    write_candidates(bj::array{
        bj::object{{"arc", "ev0.gpk"}, {"entry", "ev_png"}, {"patched_img", p_ev_png}},
        bj::object{{"arc", "ta0.gpk"}, {"entry", "ta_png"}, {"patched_img", p_ta_png}},
    });
    EXPECT_THROW(cg::run_find_narrative_cg(opt), std::runtime_error);
    EXPECT_EQ(read_file(dwq_dir + "\\ev0.gpk"), pristine_ev_gpk);
    EXPECT_FALSE(fs::exists(fs::u8path(dwq_dir + "\\ev0.gpk.tmp")));
    EXPECT_FALSE(fs::exists(fs::u8path(dwq_dir + "\\ta0.gpk.tmp")));
}
