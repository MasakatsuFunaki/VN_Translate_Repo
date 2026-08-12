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
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <boost/json.hpp>

#include "cg/cg_pipeline.h"
#include "common/util.h"
#include "ddp/ddp_archive.h"

namespace bj = boost::json;
namespace fs = std::filesystem;
using namespace shin;

namespace {

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

std::string temp_path(const std::string& name) {
    return (fs::temp_directory_path() / ("ut_cg_" + name)).u8string();
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
    const std::string p = temp_path("a.dat");
    write_file(p, make_ddp2(payload(0xAA, 10), payload(0xBB, 20)));
    const auto entries = ddp::list_archive(p);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "ut_cg_a#00000");
    EXPECT_EQ(entries[1].name, "ut_cg_a#00001");
    EXPECT_EQ(entries[1].packed, 20u);
    fs::remove(fs::u8path(p));
}

TEST(DdpArchive, ddp3_reads_names_out_of_the_entry_blocks) {
    const std::string p = temp_path("b.dat");
    write_file(p, make_ddp3("bg_one", payload(0xAA, 10), "bg_two", payload(0xBB, 20)));
    const auto entries = ddp::list_archive(p);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "bg_one");
    EXPECT_EQ(entries[1].name, "bg_two");
    EXPECT_EQ(entries[1].offset, 0x200u + 10u);
    fs::remove(fs::u8path(p));
}

TEST(DdpArchive, rejects_a_foreign_magic) {
    const std::string p = temp_path("c.dat");
    write_file(p, Bytes{'N', 'O', 'P', 'E', 0, 0, 0, 0});
    EXPECT_THROW(ddp::list_archive(p), std::runtime_error);
    EXPECT_THROW(ddp::repack(p, temp_path("c.out"), {}), std::runtime_error);
    fs::remove(fs::u8path(p));
}

// No replacements -> the rebuild must reproduce the input exactly, or every
// unpatched entry in an 800 MB archive is at risk.
TEST(DdpArchive, ddp2_identity_repack_is_byte_exact) {
    const std::string in = temp_path("d.dat"), out = temp_path("d.out");
    const Bytes original = make_ddp2(payload(0xAA, 10), payload(0xBB, 20));
    write_file(in, original);
    ddp::repack(in, out, {});
    EXPECT_EQ(read_file(out), original);
    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

TEST(DdpArchive, ddp3_identity_repack_is_byte_exact) {
    const std::string in = temp_path("e.dat"), out = temp_path("e.out");
    const Bytes original = make_ddp3("bg_one", payload(0xAA, 10), "bg_two", payload(0xBB, 20));
    write_file(in, original);
    ddp::repack(in, out, {});
    EXPECT_EQ(read_file(out), original);
    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

// A replacement grows entry 0, so entry 1's data offset has to move with it.
TEST(DdpArchive, ddp2_replacement_rewrites_the_following_offsets) {
    const std::string in = temp_path("f.dat"), out = temp_path("f.out");
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
    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

TEST(DdpArchive, ddp3_replacement_keeps_the_section_layout) {
    const std::string in = temp_path("g.dat"), out = temp_path("g.out");
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
    fs::remove(fs::u8path(in));
    fs::remove(fs::u8path(out));
}

// ---------------------------------------------------------------------------
// 04_find_narrative_cg helpers
// ---------------------------------------------------------------------------

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
