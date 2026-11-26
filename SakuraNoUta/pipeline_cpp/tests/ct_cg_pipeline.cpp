// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Black-box component tests for the narrative-CG pipeline public facade
// (`snu::cg::run_find_narrative_cg`) over synthetic multi-entry BURIKO ARC20
// archives (`data02500.arc`, `data02501.arc`, `data02503.arc`), verifying
// universal `DSC FORMAT 1.00` + 16-byte BGI BG output, 32-bpp alpha donor
// preservation, skip-list enforcement, idempotent `.backup` restoration when
// all candidates for an archive are removed, and per-archive fault isolation.
#include "arc_fixture.h"
#include "bgi/bgi_archive.h"
#include "bgi/bgi_sysgrp.h"
#include "cg/cg_pipeline.h"
#include "cg/cg_repacker.h"
#include "cg/image.h"
#include "common/util.h"
#include "menu/bgra_image.h"
#include "menu_patch_format.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINGDI_)
#error "Windows header leaked through public cg/bgi/wire headers"
#endif

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace bgi = snu::bgi;
namespace bj = boost::json;
namespace cg = snu::cg;
namespace fs = std::filesystem;
namespace menu = snu::menu;
using snu::Bytes;
using snu::test_fixture::make_arc;

constexpr int kBgW = 4;
constexpr int kBgH = 2;
constexpr int kBpp24 = 24;
constexpr int kBpp32 = 32;
constexpr std::size_t kCbgV2StubBytes = 0x30u;
constexpr std::size_t kCbgWidthOffset = 0x10u;
constexpr std::size_t kCbgHeightOffset = 0x12u;
constexpr std::size_t kCbgBppOffset = 0x14u;
constexpr std::size_t kCbgVersionOffset = 0x2Eu;
constexpr std::uint8_t kBmpSigB = 'B';
constexpr std::uint8_t kBmpSigM = 'M';

class ScopedSandbox {
public:
    ScopedSandbox() {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1u, std::memory_order_relaxed);
        root_ = fs::temp_directory_path() /
                fs::u8path("snu_ct_cg_pipeline_" + std::to_string(id));
        std::error_code ec;
        fs::remove_all(root_, ec);
        project_dir_ = root_ / fs::u8path("project");
        game_dir_ = root_ / fs::u8path("game");
        fs::create_directories(project_dir_ / fs::u8path("script_output") /
                                   fs::u8path("narrative_patched"),
                               ec);
        fs::create_directories(game_dir_, ec);
    }

    ScopedSandbox(const ScopedSandbox&) = delete;
    ScopedSandbox& operator=(const ScopedSandbox&) = delete;

    ~ScopedSandbox() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    const fs::path& project_dir() const noexcept { return project_dir_; }
    const fs::path& game_dir() const noexcept { return game_dir_; }

    fs::path candidates_json() const {
        return project_dir_ / fs::u8path("script_output") /
               fs::u8path("narrative_candidates.json");
    }

    fs::path patched_bmp(std::string_view filename) const {
        return project_dir_ / fs::u8path("script_output") /
               fs::u8path("narrative_patched") / fs::u8path(filename);
    }

private:
    fs::path root_;
    fs::path project_dir_;
    fs::path game_dir_;
};

Bytes make_dsc_bg(const menu::BgraImage& img, int bpp,
                  const std::array<std::uint8_t, bgi::kSysGrpReservedBytes>& res = {}) {
    return bgi::dsc_compress(bgi::encode_sysgrp(img, bpp, res));
}

Bytes make_undecodable_cbg_v2_32bpp(std::uint16_t w, std::uint16_t h) {
    Bytes blob(kCbgV2StubBytes, 0u);
    static constexpr char kMagic[] = "CompressedBG___";
    for (std::size_t i = 0; i < sizeof(kMagic); ++i) {
        blob[i] = static_cast<std::uint8_t>(kMagic[i]);
    }
    blob[kCbgWidthOffset] = static_cast<std::uint8_t>(w & 0xFFu);
    blob[kCbgWidthOffset + 1u] = static_cast<std::uint8_t>((w >> 8) & 0xFFu);
    blob[kCbgHeightOffset] = static_cast<std::uint8_t>(h & 0xFFu);
    blob[kCbgHeightOffset + 1u] = static_cast<std::uint8_t>((h >> 8) & 0xFFu);
    blob[kCbgBppOffset] = static_cast<std::uint8_t>(kBpp32);
    blob[kCbgVersionOffset] = 2u;  // CBG v2 without valid stream
    return blob;
}

menu::BgraImage make_patterned_bgra(int w, int h, std::uint8_t seed,
                                    bool vary_alpha) {
    menu::BgraImage img(w, h);
    constexpr std::array<std::uint8_t, 4> kAlphaSeam = {0x00u, 0x7Fu, 0x80u, 0xFFu};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t idx =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                static_cast<std::size_t>(x);
            const auto b = static_cast<std::uint8_t>(seed + idx * 7u);
            const auto g = static_cast<std::uint8_t>(seed + idx * 13u + 0x40u);
            const auto r = static_cast<std::uint8_t>(seed + idx * 19u + 0x80u);
            const std::uint8_t a = vary_alpha ? kAlphaSeam[idx % kAlphaSeam.size()]
                                              : menu::kOpaqueAlpha;
            img.SetPixel(x, y, menu::BgraColor{b, g, r, a});
        }
    }
    return img;
}

bool starts_with_bm(const Bytes& blob) noexcept {
    return blob.size() >= 2u && blob[0] == kBmpSigB && blob[1] == kBmpSigM;
}

bj::object make_candidate_obj(std::string_view arc, std::string_view entry,
                              const fs::path& bmp_path) {
    bj::object c;
    c["arc"] = std::string(arc);
    c["entry"] = std::string(entry);
    c["confidence"] = 0.95;
    c["description"] = "synthetic test candidate";
    c["regions"] = bj::array{};
    c["patched_bmp"] = bmp_path.u8string();
    return c;
}

}  // namespace

TEST(CgPipelineComponentTest,
     RepacksMultiArchiveCandidatesToNativeDscPreservingAlphaAndRevertsRemovedArchives) {
    ScopedSandbox sb;

    const std::array<std::uint8_t, bgi::kSysGrpReservedBytes> kRes24 = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
    };
    const std::array<std::uint8_t, bgi::kSysGrpReservedBytes> kRes32 = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA,
    };

    const menu::BgraImage orig_ev24 = make_patterned_bgra(kBgW, kBgH, 0x10u, false);
    const menu::BgraImage orig_ev32 = make_patterned_bgra(kBgW, kBgH, 0x20u, true);
    const menu::BgraImage orig_keep = make_patterned_bgra(kBgW, kBgH, 0x30u, false);
    const menu::BgraImage orig_kt_ib = make_patterned_bgra(kBgW, kBgH, 0x40u, true);
    const menu::BgraImage orig_kt_mh = make_patterned_bgra(kBgW, kBgH, 0x50u, true);
    const menu::BgraImage orig_kt_ny = make_patterned_bgra(kBgW, kBgH, 0x60u, true);
    const menu::BgraImage orig_logo32 = make_patterned_bgra(kBgW, kBgH, 0x70u, true);
    const menu::BgraImage orig_dim = make_patterned_bgra(kBgW, kBgH, 0x80u, false);

    const Bytes orig_ev24_raw = make_dsc_bg(orig_ev24, kBpp24, kRes24);
    const Bytes orig_ev32_raw = make_dsc_bg(orig_ev32, kBpp32, kRes32);
    const Bytes orig_keep_raw = make_dsc_bg(orig_keep, kBpp24);
    const Bytes orig_kt_ib_raw = make_dsc_bg(orig_kt_ib, kBpp32);
    const Bytes orig_kt_mh_raw = make_dsc_bg(orig_kt_mh, kBpp32);
    const Bytes orig_kt_ny_raw = make_dsc_bg(orig_kt_ny, kBpp32);
    const Bytes orig_logo32_raw = make_dsc_bg(orig_logo32, kBpp32, kRes32);
    const Bytes orig_dim_raw = make_dsc_bg(orig_dim, kBpp24);

    const Bytes pristine_2500 = make_arc({
        {"ev_bg_24", orig_ev24_raw},
        {"ev_ov_32", orig_ev32_raw},
        {"untouched_bg", orig_keep_raw},
    });
    const Bytes pristine_2503 = make_arc({
        {"kt_ib", orig_kt_ib_raw},
        {"kt_mh", orig_kt_mh_raw},
        {"kt_ny", orig_kt_ny_raw},
        {"logo_ov", orig_logo32_raw},
        {"dim_mismatch", orig_dim_raw},
    });

    const fs::path arc_2500 = sb.game_dir() / fs::u8path("data02500.arc");
    const fs::path arc_2503 = sb.game_dir() / fs::u8path("data02503.arc");
    const fs::path bak_2500 = sb.game_dir() / fs::u8path("data02500.arc.backup");
    const fs::path bak_2503 = sb.game_dir() / fs::u8path("data02503.arc.backup");
    snu::write_file(arc_2500.u8string(), pristine_2500);
    snu::write_file(arc_2503.u8string(), pristine_2503);

    // Stage patched BMP files on disk:
    //  - ev_bg_24.bmp: 24-bpp replacement for 24-bpp entry
    //  - ev_ov_32.bmp: 32-bpp replacement carrying its own straight alpha
    //  - logo_ov.bmp:  legacy 24-bpp BMP replacing a 32-bpp entry (must transplant donor alpha!)
    //  - kt_ib/mh/ny.bmp: bogus 24-bpp BMPs that must be rejected by skip list
    //  - dim_mismatch.bmp: wrong dimensions (2x2 vs 4x2), must be skipped cleanly
    const menu::BgraImage patched_ev24 = make_patterned_bgra(kBgW, kBgH, 0xA0u, false);
    const menu::BgraImage patched_ev32 = make_patterned_bgra(kBgW, kBgH, 0xB0u, true);
    const menu::BgraImage patched_logo_rgb = make_patterned_bgra(kBgW, kBgH, 0xC0u, false);
    const menu::BgraImage wrong_dim_img = make_patterned_bgra(2, 2, 0xD0u, false);

    const fs::path bmp_ev24 = sb.patched_bmp("ev_bg_24.bmp");
    const fs::path bmp_ev32 = sb.patched_bmp("ev_ov_32.bmp");
    const fs::path bmp_logo = sb.patched_bmp("logo_ov.bmp");
    const fs::path bmp_kt_ib = sb.patched_bmp("kt_ib.bmp");
    const fs::path bmp_kt_mh = sb.patched_bmp("kt_mh.bmp");
    const fs::path bmp_kt_ny = sb.patched_bmp("kt_ny.bmp");
    const fs::path bmp_dim = sb.patched_bmp("dim_mismatch.bmp");

    snu::write_file(bmp_ev24.u8string(), patched_ev24.EncodeBmp(kBpp24));
    snu::write_file(bmp_ev32.u8string(), patched_ev32.EncodeBmp(kBpp32));
    snu::write_file(bmp_logo.u8string(), patched_logo_rgb.EncodeBmp(kBpp24));
    snu::write_file(bmp_kt_ib.u8string(), patched_ev24.EncodeBmp(kBpp24));
    snu::write_file(bmp_kt_mh.u8string(), patched_ev24.EncodeBmp(kBpp24));
    snu::write_file(bmp_kt_ny.u8string(), patched_ev24.EncodeBmp(kBpp24));
    snu::write_file(bmp_dim.u8string(), wrong_dim_img.EncodeBmp(kBpp24));

    const bj::array pass1_candidates = {
        make_candidate_obj("data02500.arc", "ev_bg_24", bmp_ev24),
        make_candidate_obj("data02500.arc", "ev_ov_32", bmp_ev32),
        make_candidate_obj("data02503.arc", "kt_ib", bmp_kt_ib),
        make_candidate_obj("data02503.arc", "kt_mh", bmp_kt_mh),
        make_candidate_obj("data02503.arc", "kt_ny", bmp_kt_ny),
        make_candidate_obj("data02503.arc", "logo_ov", bmp_logo),
        make_candidate_obj("data02503.arc", "dim_mismatch", bmp_dim),
    };
    snu::write_file(sb.candidates_json().u8string(),
                    bj::serialize(bj::value(pass1_candidates)));

    // --- Pass 1: public facade repack_only ---
    cg::CgOptions opt;
    opt.game_dir = sb.game_dir().u8string();
    opt.project_dir = sb.project_dir().u8string();
    opt.repack_only = true;
    ASSERT_EQ(cg::run_find_narrative_cg(opt), 0);

    // Backups hold pristine bytes; zero .tmp leftovers.
    ASSERT_TRUE(fs::exists(bak_2500));
    ASSERT_TRUE(fs::exists(bak_2503));
    EXPECT_EQ(snu::read_file(bak_2500.u8string()), pristine_2500);
    EXPECT_EQ(snu::read_file(bak_2503.u8string()), pristine_2503);
    EXPECT_FALSE(fs::exists(sb.game_dir() / fs::u8path("data02500.arc.tmp")));
    EXPECT_FALSE(fs::exists(sb.game_dir() / fs::u8path("data02503.arc.tmp")));

    // Verify outputs through independent real consumers:
    // proxy_dll's FindArc20EntrySlice + bgi::dsc_decompress + bgi::decode_sysgrp.
    const Bytes repacked_2500 = snu::read_file(arc_2500.u8string());
    const Bytes repacked_2503 = snu::read_file(arc_2503.u8string());

    auto read_via_proxy_parser = [](const Bytes& arc_blob, std::string_view name) {
        std::uint64_t off = 0u;
        std::uint64_t sz = 0u;
        EXPECT_EQ(menu_patch_format::FindArc20EntrySlice(
                      menu_patch_format::ByteSpan(arc_blob), name, off, sz),
                  menu_patch_format::ArcStatus::kOk)
            << "entry: " << name;
        return Bytes(arc_blob.begin() + static_cast<std::ptrdiff_t>(off),
                     arc_blob.begin() + static_cast<std::ptrdiff_t>(off + sz));
    };

    const Bytes out_ev24 = read_via_proxy_parser(repacked_2500, "ev_bg_24");
    ASSERT_TRUE(bgi::is_dsc(out_ev24));
    EXPECT_FALSE(starts_with_bm(out_ev24));
    const bgi::SysGrpImage dec_ev24 = bgi::decode_sysgrp(bgi::dsc_decompress(out_ev24));
    EXPECT_EQ(dec_ev24.orig_bpp, kBpp24);
    EXPECT_EQ(dec_ev24.reserved, kRes24);
    EXPECT_EQ(dec_ev24.image, patched_ev24);

    const Bytes out_ev32 = read_via_proxy_parser(repacked_2500, "ev_ov_32");
    ASSERT_TRUE(bgi::is_dsc(out_ev32));
    EXPECT_FALSE(starts_with_bm(out_ev32));
    const bgi::SysGrpImage dec_ev32 = bgi::decode_sysgrp(bgi::dsc_decompress(out_ev32));
    EXPECT_EQ(dec_ev32.orig_bpp, kBpp32);
    EXPECT_EQ(dec_ev32.reserved, kRes32);
    EXPECT_EQ(dec_ev32.image, patched_ev32);

    EXPECT_EQ(read_via_proxy_parser(repacked_2500, "untouched_bg"), orig_keep_raw);

    // Skipped kt_* and dimension-mismatched entry stay byte-exact to original.
    EXPECT_EQ(read_via_proxy_parser(repacked_2503, "kt_ib"), orig_kt_ib_raw);
    EXPECT_EQ(read_via_proxy_parser(repacked_2503, "kt_mh"), orig_kt_mh_raw);
    EXPECT_EQ(read_via_proxy_parser(repacked_2503, "kt_ny"), orig_kt_ny_raw);
    EXPECT_EQ(read_via_proxy_parser(repacked_2503, "dim_mismatch"), orig_dim_raw);

    // logo_ov: RGB from 24-bpp patched BMP + alpha transplanted from orig_logo32!
    const Bytes out_logo = read_via_proxy_parser(repacked_2503, "logo_ov");
    ASSERT_TRUE(bgi::is_dsc(out_logo));
    EXPECT_FALSE(starts_with_bm(out_logo));
    const bgi::SysGrpImage dec_logo = bgi::decode_sysgrp(bgi::dsc_decompress(out_logo));
    EXPECT_EQ(dec_logo.orig_bpp, kBpp32);
    EXPECT_EQ(dec_logo.reserved, kRes32);
    for (int y = 0; y < kBgH; ++y) {
        for (int x = 0; x < kBgW; ++x) {
            const menu::BgraColor rgb_src = patched_logo_rgb.PixelAt(x, y);
            const menu::BgraColor a_src = orig_logo32.PixelAt(x, y);
            EXPECT_EQ(dec_logo.image.PixelAt(x, y),
                      (menu::BgraColor{rgb_src.b, rgb_src.g, rgb_src.r, a_src.a}));
        }
    }

    // --- Pass 2: externally dirty live data02503.arc AND remove ALL data02503.arc
    //     candidates (plus ev_ov_32) from narrative_candidates.json ---
    const Bytes dirty_bmp = patched_ev24.EncodeBmp(kBpp24);
    const fs::path dirty_tmp = sb.game_dir() / fs::u8path("dirty.tmp");
    bgi::repack_arc(arc_2503.u8string(), dirty_tmp.u8string(), {{"kt_ib", dirty_bmp}});
    fs::rename(dirty_tmp, arc_2503);
    ASSERT_EQ(bgi::read_entry_by_name(arc_2503.u8string(), "kt_ib", false), dirty_bmp);

    const bj::array pass2_candidates = {
        make_candidate_obj("data02500.arc", "ev_bg_24", bmp_ev24),
    };
    snu::write_file(sb.candidates_json().u8string(),
                    bj::serialize(bj::value(pass2_candidates)));

    ASSERT_EQ(cg::run_find_narrative_cg(opt), 0);

    // data02503.arc had zero remaining candidates yet a `.backup` existed:
    // the pipeline must restore data02503.arc byte-for-byte to pristine `.backup`,
    // eliminating both the previously patched logo_ov and the externally dirtied kt_ib.
    EXPECT_EQ(snu::read_file(arc_2503.u8string()), pristine_2503);
    EXPECT_EQ(snu::read_file(bak_2503.u8string()), pristine_2503);

    // data02500.arc keeps ev_bg_24 patched while reverting removed ev_ov_32 to pristine.
    const Bytes repacked2_2500 = snu::read_file(arc_2500.u8string());
    EXPECT_EQ(read_via_proxy_parser(repacked2_2500, "ev_bg_24"), out_ev24);
    EXPECT_EQ(read_via_proxy_parser(repacked2_2500, "ev_ov_32"), orig_ev32_raw);
    EXPECT_EQ(snu::read_file(bak_2500.u8string()), pristine_2500);
}

TEST(CgPipelineComponentTest,
     RefusesToStripUnpreservable32BppAlphaAndIsolatesCorruptArchive) {
    ScopedSandbox sb;

    // 1. data02503.arc holds an undecodable 32-bpp CBG v2 stub paired with a
    //    legacy 24-bpp BMP on disk: without a decodable 32-bpp alpha donor,
    //    repack must skip the entry rather than silently overwriting with opaque 0xFF.
    const Bytes orig_cbg32_stub = make_undecodable_cbg_v2_32bpp(kBgW, kBgH);
    const Bytes pristine_2503 = make_arc({{"opaque_trap_32", orig_cbg32_stub}});
    const fs::path arc_2503 = sb.game_dir() / fs::u8path("data02503.arc");
    const fs::path bak_2503 = sb.game_dir() / fs::u8path("data02503.arc.backup");
    snu::write_file(arc_2503.u8string(), pristine_2503);

    // 2. data02501.arc is corrupt (non-ARC20), while data02500.arc is valid.
    const Bytes corrupt_2501 = {'B', 'A', 'D', 'A', 'R', 'C', 0, 0};
    const fs::path arc_2501 = sb.game_dir() / fs::u8path("data02501.arc");
    const fs::path bak_2501 = sb.game_dir() / fs::u8path("data02501.arc.backup");
    snu::write_file(arc_2501.u8string(), corrupt_2501);

    const menu::BgraImage orig_valid = make_patterned_bgra(kBgW, kBgH, 0x15u, false);
    const menu::BgraImage patched_valid = make_patterned_bgra(kBgW, kBgH, 0x95u, false);
    const fs::path arc_2500 = sb.game_dir() / fs::u8path("data02500.arc");
    const fs::path bak_2500 = sb.game_dir() / fs::u8path("data02500.arc.backup");
    const Bytes pristine_2500 =
        make_arc({{"valid_24", make_dsc_bg(orig_valid, kBpp24)}});
    snu::write_file(arc_2500.u8string(), pristine_2500);

    const fs::path bmp_24 = sb.patched_bmp("valid_24.bmp");
    const fs::path bmp_trap = sb.patched_bmp("opaque_trap_32.bmp");
    snu::write_file(bmp_24.u8string(), patched_valid.EncodeBmp(kBpp24));
    snu::write_file(bmp_trap.u8string(), patched_valid.EncodeBmp(kBpp24));

    const bj::array candidates = {
        make_candidate_obj("data02503.arc", "opaque_trap_32", bmp_trap),
        make_candidate_obj("data02501.arc", "broken_entry", bmp_24),
        make_candidate_obj("data02500.arc", "valid_24", bmp_24),
    };
    snu::write_file(sb.candidates_json().u8string(),
                    bj::serialize(bj::value(candidates)));

    cg::CgOptions opt;
    opt.game_dir = sb.game_dir().u8string();
    opt.project_dir = sb.project_dir().u8string();
    opt.repack_only = true;
    ASSERT_EQ(cg::run_find_narrative_cg(opt), 0);

    // data02503.arc untouched (alpha could not be preserved -> skipped, no backup created).
    EXPECT_EQ(snu::read_file(arc_2503.u8string()), pristine_2503);
    EXPECT_FALSE(fs::exists(bak_2503));

    // data02501.arc untouched (corrupt archive isolated, no backup or tmp leftovers).
    EXPECT_EQ(snu::read_file(arc_2501.u8string()), corrupt_2501);
    EXPECT_FALSE(fs::exists(bak_2501));
    EXPECT_FALSE(fs::exists(sb.game_dir() / fs::u8path("data02501.arc.tmp")));

    // data02500.arc repacked cleanly to native DSC FORMAT 1.00.
    ASSERT_TRUE(fs::exists(bak_2500));
    EXPECT_EQ(snu::read_file(bak_2500.u8string()), pristine_2500);
    const Bytes out_valid =
        bgi::read_entry_by_name(arc_2500.u8string(), "valid_24", false);
    ASSERT_TRUE(bgi::is_dsc(out_valid));
    EXPECT_FALSE(starts_with_bm(out_valid));
    const bgi::SysGrpImage dec_valid =
        bgi::decode_sysgrp(bgi::dsc_decompress(out_valid));
    EXPECT_EQ(dec_valid.orig_bpp, kBpp24);
    EXPECT_EQ(dec_valid.image, patched_valid);
}
