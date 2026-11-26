// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Narrative-CG step: the DDP2/DDP3 repacker and the deterministic helpers of
// 04_find_narrative_cg.
//
// The archive tests build their fixtures in memory rather than leaning on the
// installed game, so they run anywhere.  The end-to-end byte comparison over
// the real 800 MB archives lives in the verification harness in analysys/.
#include "cg/cg_pipeline.h"
#include "common/util.h"
#include "ddp/ddp_archive.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace bj = boost::json;
namespace fs = std::filesystem;
using namespace shin;

namespace {

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name_utf8)
        : path_(fs::temp_directory_path() / fs::u8path(name_utf8)) {
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

    std::string file_u8(std::string_view file_name_utf8) const {
        return (path_ / fs::u8path(file_name_utf8)).u8string();
    }

private:
    fs::path path_;
};

void put_u32le(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

std::uint32_t get_u32le(const Bytes& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

Bytes payload(std::uint8_t tag, std::size_t n) { return Bytes(n, tag); }

// Two entries, each stored with packed != 0.
Bytes make_ddp2(const Bytes& a, const Bytes& b) {
    Bytes out;
    out.insert(out.end(), {'D', 'D', 'P', '2'});
    put_u32le(out, 2);
    out.insert(out.end(), 24, 0);  // rest of the 0x20 header
    const std::uint32_t data_start = 0x20 + 2 * 16;
    put_u32le(out, data_start);
    put_u32le(out, static_cast<std::uint32_t>(a.size() * 2));  // unpacked (arbitrary)
    put_u32le(out, static_cast<std::uint32_t>(a.size()));
    put_u32le(out, 0);
    put_u32le(out, data_start + static_cast<std::uint32_t>(a.size()));
    put_u32le(out, static_cast<std::uint32_t>(b.size() * 2));
    put_u32le(out, static_cast<std::uint32_t>(b.size()));
    put_u32le(out, 0);
    out.insert(out.end(), a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

// One section, two named entries, data starting at a fixed offset.
Bytes make_ddp3(const std::string& name_a, const Bytes& a, const std::string& name_b,
                const Bytes& b) {
    auto name_utf16 = [](const std::string& s) {
        Bytes out;
        for (char c : s) {
            out.push_back(static_cast<std::uint8_t>(c));
            out.push_back(0);
        }
        return out;
    };
    const Bytes na = name_utf16(name_a), nb = name_utf16(name_b);
    const std::uint8_t sz_a = static_cast<std::uint8_t>(17 + na.size());
    const std::uint8_t sz_b = static_cast<std::uint8_t>(17 + nb.size());
    const std::uint32_t block = sz_a + sz_b;
    const std::uint32_t sec_off = 0x20 + 8;   // right after the 1-section table
    const std::uint32_t data_start = 0x200;   // leaves a zero-padding gap

    Bytes out;
    out.insert(out.end(), {'D', 'D', 'P', '3'});
    put_u32le(out, 1);           // one section
    put_u32le(out, data_start);  // field8
    out.insert(out.end(), 20, 0);
    put_u32le(out, block);
    put_u32le(out, sec_off);

    out.push_back(sz_a);
    put_u32le(out, data_start);
    put_u32le(out, static_cast<std::uint32_t>(a.size() * 2));
    put_u32le(out, static_cast<std::uint32_t>(a.size()));
    put_u32le(out, 0);
    out.insert(out.end(), na.begin(), na.end());

    out.push_back(sz_b);
    put_u32le(out, data_start + static_cast<std::uint32_t>(a.size()));
    put_u32le(out, static_cast<std::uint32_t>(b.size() * 2));
    put_u32le(out, static_cast<std::uint32_t>(b.size()));
    put_u32le(out, 0);
    out.insert(out.end(), nb.begin(), nb.end());

    out.insert(out.end(), data_start - out.size(), 0);
    out.insert(out.end(), a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

}  // namespace

TEST(ShsWrap, is_a_literal_block_with_a_big_endian_length) {
    const Bytes wrapped = ddp::shs_wrap(Bytes{1, 2, 3});
    ASSERT_EQ(wrapped.size(), 8u);
    EXPECT_EQ(wrapped[0], 0x1F);
    EXPECT_EQ(wrapped[1], 0x00);
    EXPECT_EQ(wrapped[2], 0x00);
    EXPECT_EQ(wrapped[3], 0x00);
    EXPECT_EQ(wrapped[4], 0x03) << "length is BIG-endian";
    EXPECT_EQ(wrapped[5], 1);

    // The game's own decompressor must read it back unchanged -- control 0x1F
    // is the only path that costs a mere 5 bytes.
    EXPECT_EQ(ddp::shs_decompress(wrapped, 3), (Bytes{1, 2, 3}));
}

TEST(DdpArchive, ddp2_names_entries_by_index) {
    const ScopedTempDir scratch("shin_ut_cg_a");
    const std::string p = scratch.file_u8("ut_cg_a.dat");
    write_file(p, make_ddp2(payload(0xAA, 10), payload(0xBB, 20)));
    const auto entries = ddp::list_archive(p);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "ut_cg_a#00000");
    EXPECT_EQ(entries[1].name, "ut_cg_a#00001");
    EXPECT_EQ(entries[1].packed, 20u);
}

TEST(DdpArchive, ddp3_reads_names_out_of_the_entry_blocks) {
    const ScopedTempDir scratch("shin_ut_cg_b");
    const std::string p = scratch.file_u8("ut_cg_b.dat");
    write_file(p, make_ddp3("bg_one", payload(0xAA, 10), "bg_two", payload(0xBB, 20)));
    const auto entries = ddp::list_archive(p);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "bg_one");
    EXPECT_EQ(entries[1].name, "bg_two");
    EXPECT_EQ(entries[1].offset, 0x200u + 10u);
}

TEST(DdpArchive, rejects_a_foreign_magic) {
    const ScopedTempDir scratch("shin_ut_cg_c");
    const std::string p = scratch.file_u8("ut_cg_c.dat");
    write_file(p, Bytes{'N', 'O', 'P', 'E', 0, 0, 0, 0});
    EXPECT_THROW(ddp::list_archive(p), std::runtime_error);
    EXPECT_THROW(ddp::repack(p, scratch.file_u8("ut_cg_c.out"), {}), std::runtime_error);
}

// No replacements -> the rebuild must reproduce the input exactly, or every
// unpatched entry in an 800 MB archive is at risk.
TEST(DdpArchive, ddp2_identity_repack_is_byte_exact) {
    const ScopedTempDir scratch("shin_ut_cg_d");
    const std::string in = scratch.file_u8("ut_cg_d.dat");
    const std::string out = scratch.file_u8("ut_cg_d.out");
    const Bytes original = make_ddp2(payload(0xAA, 10), payload(0xBB, 20));
    write_file(in, original);
    ddp::repack(in, out, {});
    EXPECT_EQ(read_file(out), original);
}

TEST(DdpArchive, ddp3_identity_repack_is_byte_exact) {
    const ScopedTempDir scratch("shin_ut_cg_e");
    const std::string in = scratch.file_u8("ut_cg_e.dat");
    const std::string out = scratch.file_u8("ut_cg_e.out");
    const Bytes original = make_ddp3("bg_one", payload(0xAA, 10), "bg_two", payload(0xBB, 20));
    write_file(in, original);
    ddp::repack(in, out, {});
    EXPECT_EQ(read_file(out), original);
}

// A replacement grows entry 0, so entry 1's data offset has to move with it.
TEST(DdpArchive, ddp2_replacement_rewrites_the_following_offsets) {
    const ScopedTempDir scratch("shin_ut_cg_f");
    const std::string in = scratch.file_u8("ut_cg_f.dat");
    const std::string out = scratch.file_u8("ut_cg_f.out");
    write_file(in, make_ddp2(payload(0xAA, 10), payload(0xBB, 20)));
    const Bytes fresh(64, 0xCC);
    ddp::repack(in, out, {{"ut_cg_f#00000", fresh}});

    const Bytes got = read_file(out);
    const std::uint32_t data_start = 0x20 + 2 * 16;
    EXPECT_EQ(get_u32le(got, 0x20), data_start);
    EXPECT_EQ(get_u32le(got, 0x24), 64u) << "unpacked = the RAW size, not the wrapped one";
    EXPECT_EQ(get_u32le(got, 0x28), 69u) << "packed = raw + the 5-byte Shs wrapper";
    EXPECT_EQ(get_u32le(got, 0x30), data_start + 69u) << "entry 1 moved by the delta";
    // Entry 1's bytes are still there, verbatim.
    EXPECT_EQ(Bytes(got.end() - 20, got.end()), payload(0xBB, 20));
}

TEST(DdpArchive, ddp3_replacement_keeps_the_section_layout) {
    const ScopedTempDir scratch("shin_ut_cg_g");
    const std::string in = scratch.file_u8("ut_cg_g.dat");
    const std::string out = scratch.file_u8("ut_cg_g.out");
    const Bytes original = make_ddp3("bg_one", payload(0xAA, 10), "bg_two", payload(0xBB, 20));
    write_file(in, original);
    const Bytes fresh(64, 0xCC);
    ddp::repack(in, out, {{"bg_one", fresh}});

    const Bytes got = read_file(out);
    // The header, the section table and every entry NAME are untouched; only
    // the offset/size fields inside the entries move.
    EXPECT_EQ(Bytes(got.begin(), got.begin() + 0x28), Bytes(original.begin(), original.begin() + 0x28));
    const auto entries = ddp::list_archive(out);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "bg_one");
    EXPECT_EQ(entries[0].unpacked, 64u);
    EXPECT_EQ(entries[0].packed, 69u);
    EXPECT_EQ(entries[0].offset, 0x200u);
    EXPECT_EQ(entries[1].offset, 0x200u + 69u);
    EXPECT_EQ(entries[1].packed, 20u);
    // The data section still starts exactly at field8.
    EXPECT_EQ(got[0x200], 0x1F) << "the replacement's Shs control byte";
}

// 04_find_narrative_cg helpers

TEST(CgHelpers, safe_name_replaces_only_the_invalid_characters) {
    EXPECT_EQ(cg::detail::safe_name("bg_op05_5"), "bg_op05_5");
    EXPECT_EQ(cg::detail::safe_name("a/b\\c:d*e?f\"g<h>i|j"), "a_b_c_d_e_f_g_h_i_j");
    EXPECT_EQ(cg::detail::safe_name(std::string("x\x01y")), "x_y");
}

TEST(CgHelpers, parse_extract_line_wants_exactly_three_fields) {
    cg::detail::ExtractLine e;
    ASSERT_TRUE(cg::detail::parse_extract_line("bg_one\t800\t600", &e));
    EXPECT_EQ(e.name, "bg_one");
    EXPECT_EQ(e.width, 800);
    EXPECT_EQ(e.height, 600);

    EXPECT_FALSE(cg::detail::parse_extract_line("bg_one\t800", &e));
    EXPECT_FALSE(cg::detail::parse_extract_line("just a message", &e));
    EXPECT_FALSE(cg::detail::parse_extract_line("a\tb\tc\td", &e));
    // Three fields but a non-numeric size is a hard error, as int() would be.
    EXPECT_THROW(cg::detail::parse_extract_line("bg\twide\t600", &e), std::runtime_error);
}

// re.search(r'\{.*?\}', DOTALL) is NON-greedy: it stops at the FIRST '}'.
TEST(CgHelpers, first_json_object_text_is_non_greedy) {
    const auto v = cg::detail::first_json_object_text("noise {\"a\": 1} tail {\"b\": 2}");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "{\"a\": 1}");
    EXPECT_FALSE(cg::detail::first_json_object_text("no braces").has_value());
}

// The array walker must balance brackets, not run to the last ']' in the reply.
TEST(CgHelpers, balanced_json_array_text_ignores_trailing_prose) {
    const auto v = cg::detail::balanced_json_array_text(
        "[{\"bbox\": [1, 2, 3, 4]}] -- note the [brackets] in this sentence");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "[{\"bbox\": [1, 2, 3, 4]}]");

    // A bracket inside a string literal must not change the depth.
    const auto s = cg::detail::balanced_json_array_text("[{\"text_en\": \"a ] b\"}]");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "[{\"text_en\": \"a ] b\"}]");

    EXPECT_FALSE(cg::detail::balanced_json_array_text("[unterminated").has_value());
}

TEST(CgHelpers, detect_hit_needs_both_the_flag_and_the_confidence) {
    auto obj = [](const char* s) { return bj::parse(s).get_object(); };
    EXPECT_TRUE(cg::detail::detect_hit(obj(R"({"has_narrative_text": true, "confidence": 0.7})")));
    EXPECT_FALSE(cg::detail::detect_hit(obj(R"({"has_narrative_text": true, "confidence": 0.69})")));
    EXPECT_FALSE(cg::detail::detect_hit(obj(R"({"has_narrative_text": false, "confidence": 1})")));
    // Missing confidence counts as 0, and 0 >= 0.7 is false.
    EXPECT_FALSE(cg::detail::detect_hit(obj(R"({"has_narrative_text": true})")));
    // A non-numeric confidence is a malformed response: throw, don't guess.
    EXPECT_THROW(
        cg::detail::detect_hit(obj(R"({"has_narrative_text": true, "confidence": "high"})")),
        std::runtime_error);
}

TEST(CgHelpers, floor_div_rounds_toward_negative_infinity) {
    EXPECT_EQ(cg::detail::floor_div(7, 2), 3);
    EXPECT_EQ(cg::detail::floor_div(-7, 2), -4) << "C++ / would truncate to -3";
    EXPECT_EQ(cg::detail::floor_div(-1, 2), -1);
}

TEST(CgHelpers, parse_hex_falls_back_on_the_background_tone) {
    using Rgba = std::array<std::uint8_t, 4>;
    EXPECT_EQ(cg::detail::parse_hex("#2a1a0e", "light"), (Rgba{0x2A, 0x1A, 0x0E, 255}));
    EXPECT_EQ(cg::detail::parse_hex("2a1a0e", "light"), (Rgba{0x2A, 0x1A, 0x0E, 255}));
    EXPECT_EQ(cg::detail::parse_hex("nope", "dark"), (Rgba{255, 255, 255, 255}));
    EXPECT_EQ(cg::detail::parse_hex("nope", "light"), (Rgba{30, 30, 30, 255}));
}

TEST(DdpArchive, ddp2_repack_from_backup_uses_explicit_entry_stem) {
    const ScopedTempDir scratch("shin_ut_cg_bak_stem");
    const std::string bak = scratch.file_u8("sample.dat.backup");
    const std::string out = scratch.file_u8("sample.dat");
    write_file(bak, make_ddp2(payload(0xAA, 10), payload(0xBB, 20)));
    const Bytes fresh(32, 0xCC);
    ddp::repack(bak, out, {{"sample#00000", fresh}}, "sample");

    const Bytes got = read_file(out);
    const std::uint32_t data_start = 0x20 + 2 * 16;
    EXPECT_EQ(get_u32le(got, 0x24), 32u);
    EXPECT_EQ(get_u32le(got, 0x28), 37u);
    EXPECT_EQ(get_u32le(got, 0x30), data_start + 37u);
    EXPECT_EQ(Bytes(got.end() - 20, got.end()), payload(0xBB, 20));
}

TEST(CgImageAlpha, decodes_and_encodes_32bpp_bmp_with_exact_straight_alpha) {
    // Hand-constructed 2x2 Windows 32-bpp BI_RGB BMP (54-byte header + 16-byte BGRA):
    //   y=0 (top):    (R=0x00,G=0x7F,B=0x80,A=0x00), (R=0xFF,G=0x10,B=0x20,A=0x80)
    //   y=1 (bottom): (R=0x11,G=0x22,B=0x33,A=0x7F), (R=0xAA,G=0xBB,B=0xCC,A=0xFF)
    const Bytes pinned_32bpp_bmp = {
        'B',  'M',  0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
        0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // Bottom row (y=1) in BGRA:
        0x33, 0x22, 0x11, 0x7F, 0xCC, 0xBB, 0xAA, 0xFF,
        // Top row (y=0) in BGRA:
        0x80, 0x7F, 0x00, 0x00, 0x20, 0x10, 0xFF, 0x80,
    };

    const auto decoded = cg::decode_image(pinned_32bpp_bmp);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, 2);
    EXPECT_EQ(decoded->height, 2);
    EXPECT_EQ(decoded->channels, 4);

    const Bytes expected_rgba = {
        0x00, 0x7F, 0x80, 0x00, 0xFF, 0x10, 0x20, 0x80,
        0x11, 0x22, 0x33, 0x7F, 0xAA, 0xBB, 0xCC, 0xFF,
    };
    EXPECT_EQ(decoded->px, expected_rgba);
    EXPECT_EQ(cg::encode_bmp(*decoded), pinned_32bpp_bmp);

    // 24-bpp BMP with non-4-aligned width (w=3 -> 9 bytes + 3 padding bytes per row).
    cg::Image rgb;
    rgb.width = 3;
    rgb.height = 2;
    rgb.channels = 3;
    rgb.px = {
        0x00, 0x7F, 0x80, 0xFF, 0x10, 0x20, 0x30, 0x40, 0x50,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
    };
    const Bytes bmp24 = cg::encode_bmp(rgb);
    ASSERT_EQ(bmp24.size(), 54u + 2u * 12u);
    EXPECT_EQ(bmp24[0x1Cu], 24u);
    const auto decoded24 = cg::decode_image(bmp24);
    ASSERT_TRUE(decoded24.has_value());
    EXPECT_EQ(decoded24->channels, 3);
    EXPECT_EQ(decoded24->px, rgb.px);
}

TEST(CgImageAlpha, porter_duff_straight_alpha_blend_matches_hand_calculated_vectors) {
    struct BlendCase {
        const char* label;
        int channels;
        std::array<std::uint8_t, 4> dst;
        std::array<std::uint8_t, 4> ink;
        std::uint8_t coverage;
        std::array<std::uint8_t, 4> expected;
    };

    constexpr BlendCase kCases[] = {
        {"transparent_dst_no_dark_halo", 4, {0, 0, 0, 0}, {255, 255, 255, 255}, 128,
         {255, 255, 255, 128}},
        {"transparent_dst_ignores_invisible_rgb", 4, {10, 20, 30, 0}, {240, 120, 60, 128}, 255,
         {240, 120, 60, 128}},
        {"semi_transparent_over_semi_transparent", 4, {0, 200, 100, 128}, {200, 100, 50, 255}, 128,
         {134, 133, 67, 192}},
        {"semi_transparent_over_opaque", 4, {40, 80, 120, 255}, {200, 100, 50, 255}, 128,
         {120, 90, 85, 255}},
        {"zero_coverage_leaves_dst_unchanged", 4, {12, 34, 56, 78}, {255, 255, 255, 255}, 0,
         {12, 34, 56, 78}},
        {"sign_bit_0x7f_coverage_over_transparent", 4, {0, 0, 0, 0}, {255, 0, 128, 255}, 0x7F,
         {255, 0, 128, 0x7F}},
        {"opaque_sign_bit_ink_over_semi_dst", 4, {12, 34, 56, 78}, {0x80, 0x7F, 0xFF, 0xFF}, 0xFF,
         {0x80, 0x7F, 0xFF, 0xFF}},
        {"rgb_3ch_opaque_blend", 3, {40, 80, 120, 0}, {200, 100, 50, 255}, 128,
         {120, 90, 85, 0}},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        std::array<std::uint8_t, 4> px = tc.dst;
        cg::blend_pixel_over(px.data(), tc.channels, tc.ink, tc.coverage);
        EXPECT_EQ(px[0], tc.expected[0]);
        EXPECT_EQ(px[1], tc.expected[1]);
        EXPECT_EQ(px[2], tc.expected[2]);
        if (tc.channels == 4) {
            EXPECT_EQ(px[3], tc.expected[3]);
        }
    }
}

TEST(CgImageAlpha, render_translation_preserves_transparent_and_translucent_rgba_backgrounds) {
    // 1. Fully transparent 4-channel canvas with dark text pixel inside bbox ->
    //    erasure must clear to RGBA(0,0,0,0) (never opaque alpha 255), and white
    //    glyph pixels rendered on top must have pure white RGB (255,255,255) with
    //    anti-aliased straight alpha (zero black fringe).
    cg::Image transparent;
    transparent.width = 120;
    transparent.height = 48;
    transparent.channels = 4;
    transparent.px.assign(120u * 48u * 4u, 0u);
    transparent.at(60, 24)[0] = 10u;
    transparent.at(60, 24)[1] = 20u;
    transparent.at(60, 24)[2] = 30u;
    transparent.at(60, 24)[3] = 255u;

    const auto bg_clear = cg::detail::sample_bg_rgba(transparent, 8, 8, 112, 40);
    EXPECT_EQ(bg_clear, (std::array<std::uint8_t, 4>{0, 0, 0, 0}));

    const bj::array regions = bj::parse(
        R"([{"text_en": "Save", "bbox": [8, 8, 112, 40], "text_color": "#ffffff", "bg_tone": "dark", "font_size": 16}])")
                                  .get_array();
    const cg::Image rendered = cg::detail::render_translation(transparent, regions);
    ASSERT_EQ(rendered.channels, 4);

    // Corner inside erased bbox must remain transparent (not forced to alpha 255).
    EXPECT_EQ(rendered.at(8, 8)[3], 0u);
    std::size_t visible_glyph_pixels = 0u;
    for (int y = 8; y <= 40; ++y) {
        for (int x = 8; x <= 112; ++x) {
            const std::uint8_t* p = rendered.at(x, y);
            if (p[3] > 0u) {
                ++visible_glyph_pixels;
                EXPECT_EQ(p[0], 255u);
                EXPECT_EQ(p[1], 255u);
                EXPECT_EQ(p[2], 255u);
            }
        }
    }
    EXPECT_GT(visible_glyph_pixels, 0u);

    // 2. Translucent panel RGBA(20, 40, 60, 160) -> sample_bg_rgba preserves alpha 160.
    cg::Image translucent;
    translucent.width = 40;
    translucent.height = 24;
    translucent.channels = 4;
    translucent.px.resize(40u * 24u * 4u);
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 40; ++x) {
            std::uint8_t* p = translucent.at(x, y);
            p[0] = 20u;
            p[1] = 40u;
            p[2] = 60u;
            p[3] = 160u;
        }
    }
    const auto bg_semi = cg::detail::sample_bg_rgba(translucent, 4, 4, 36, 20);
    EXPECT_EQ(bg_semi, (std::array<std::uint8_t, 4>{20, 40, 60, 160}));
}
