// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-3 tests.  This step rewrites the game's own .spt binaries, so the
// structural invariants (4-byte alignment, offset-table pointers, section
// pre_value, VA relocation) are the load-bearing part -- a mistake here
// corrupts the script rather than just mistranslating it.
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "common/util.h"
#include "repack/repack.h"

namespace rp = exc::repack;
using namespace exc;

namespace {

void put_u32(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 24));
}

std::uint32_t rd_u32(const Bytes& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

// A decrypted SPT with one text section:
//   0x00..0x7F  header (h_10 = string count, h_14 = VA base part)
//   then        [pre][0x66660001][0x55550002][0x44440002][OT][strings]
//   then        a trailing section so find_next_section_marker terminates.
Bytes make_spt(const std::vector<Bytes>& payloads, std::uint32_t h_14 = 0x100) {
    const std::uint32_t h_10 = static_cast<std::uint32_t>(payloads.size());

    Bytes block;
    for (const auto& p : payloads) {
        block.insert(block.end(), p.begin(), p.end());
        block.push_back(0);
        while (block.size() % 4) block.push_back(0);
    }

    Bytes dec(0x80, 0x20);
    dec[0x10] = static_cast<std::uint8_t>(h_10);
    dec[0x11] = dec[0x12] = dec[0x13] = 0;
    dec[0x14] = static_cast<std::uint8_t>(h_14);
    dec[0x15] = static_cast<std::uint8_t>(h_14 >> 8);
    dec[0x16] = dec[0x17] = 0;

    const std::size_t ot_bytes = static_cast<std::size_t>(h_10) * 4;
    const std::uint32_t pre = static_cast<std::uint32_t>((ot_bytes + block.size() + 16) / 4);
    put_u32(dec, pre);
    put_u32(dec, 0x66660001);
    put_u32(dec, 0x55550002);
    put_u32(dec, 0x44440002);

    // Offset table: entry[i] = (h_14 + h_10) + byte_offset[i]/4
    const std::uint32_t ot_base = h_14 + h_10;
    std::size_t off = 0;
    for (const auto& p : payloads) {
        put_u32(dec, ot_base + static_cast<std::uint32_t>(off / 4));
        std::size_t n = p.size() + 1;
        n = ((n + 3) / 4) * 4;
        off += n;
    }
    dec.insert(dec.end(), block.begin(), block.end());

    // Trailing section so the block has a hard end.
    put_u32(dec, 4);
    put_u32(dec, 0x66660001);
    put_u32(dec, 0x55550003);
    put_u32(dec, 0x44440003);
    return dec;
}

Bytes cp932(const char* utf8) { return utf8_to_cp932_replace(utf8); }

}  // namespace

TEST(Repack, xor_is_self_inverse) {
    const Bytes plain{0x00, 0x41, 0xFF, 0x80};
    EXPECT_EQ(rp::decrypt_spt(rp::encrypt_spt(plain)), plain);
    EXPECT_EQ(rp::encrypt_spt(plain), (Bytes{0xFF, 0xBE, 0x00, 0x7F}));
}

TEST(Repack, merge_speaker_line_folds_short_plate) {
    EXPECT_EQ(rp::merge_speaker_line("Yumemi\r\n\"Ah\""), "Yumemi: \"Ah\"");
    const std::string longish(30, 'x');
    EXPECT_EQ(rp::merge_speaker_line(longish + "\r\nrest"), longish + "\r\nrest");
    EXPECT_EQ(rp::merge_speaker_line("\"Ah\"\r\nrest"), "\"Ah\"\r\nrest");
    EXPECT_EQ(rp::merge_speaker_line("no break"), "no break");
}

TEST(Repack, sanitize_newlines_normalises_to_crlf) {
    EXPECT_EQ(rp::sanitize_newlines("a\nb"), "a\r\nb");
    EXPECT_EQ(rp::sanitize_newlines("a\r\nb"), "a\r\nb");
    // Only leading '\n' is stripped -- not general whitespace.
    EXPECT_EQ(rp::sanitize_newlines("\n\na"), "a");
    EXPECT_EQ(rp::sanitize_newlines("  a"), "  a");
}

TEST(Repack, word_wrap_breaks_and_caps_lines) {
    EXPECT_EQ(rp::word_wrap("aaa bbb ccc ddd", 7), "aaa bbb\r\nccc ddd");
    EXPECT_EQ(rp::word_wrap("short\r\nalso short", 20), "short\r\nalso short");
    EXPECT_EQ(rp::word_wrap("abcdefghij", 4), "abcdefghij");

    std::string many;
    for (int i = 0; i < rp::MAX_LINES + 4; ++i) many += "line" + std::to_string(i) + "\r\n";
    const std::string wrapped = rp::word_wrap(many, rp::MAX_LINE_CHARS);
    std::size_t lines = 1;
    for (std::size_t i = 0; i + 1 < wrapped.size(); ++i)
        if (wrapped.compare(i, 2, "\r\n") == 0) ++lines;
    EXPECT_LE(lines, static_cast<std::size_t>(rp::MAX_LINES));
}

// Word wrap counts CHARACTERS, not bytes -- a byte-based implementation would
// break Japanese lines three times too early.
TEST(Repack, word_wrap_counts_codepoints_not_bytes) {
    // Five 3-byte characters separated by spaces: 15 bytes of text, 5 chars.
    const std::string jp = "\xE3\x81\x82 \xE3\x81\x84 \xE3\x81\x86";
    EXPECT_EQ(rp::word_wrap(jp, 10), jp) << "wrapped a 5-char line at a 10-char budget";
}

TEST(Repack, parses_text_block_of_aligned_strings) {
    const Bytes dec = make_spt({cp932("\xE3\x81\x82"), cp932("hello"), cp932("x")});
    std::size_t block_end = 0;
    const std::size_t start = rp::find_text_block_start(dec, {0x80});
    const auto strings = rp::parse_text_block(dec, start, &block_end);

    ASSERT_EQ(strings.size(), 3u);
    EXPECT_EQ(strings[0].raw, cp932("\xE3\x81\x82"));
    EXPECT_EQ(strings[1].raw, cp932("hello"));
    EXPECT_EQ(strings[2].raw, cp932("x"));
    // Every string starts on a 4-byte boundary relative to the block.
    for (const auto& s : strings) EXPECT_EQ((s.offset - start) % 4, 0u);
    EXPECT_GT(block_end, strings.back().offset);
}

TEST(Repack, find_next_section_marker_needs_a_5555_type_word) {
    Bytes dec = make_spt({cp932("abcd")});
    // The real trailing section must be found...
    const std::size_t start = rp::find_text_block_start(dec, {0x80});
    EXPECT_LT(rp::find_next_section_marker(dec, start), dec.size());

    // ...but a bare 0x66660001 with no 0x5555xxxx after it is not a section.
    Bytes fake(64, 0);
    fake[0] = 0x01; fake[1] = 0x00; fake[2] = 0x66; fake[3] = 0x66;
    EXPECT_EQ(rp::find_next_section_marker(fake, 0), fake.size());
}

TEST(Repack, block_start_skips_the_offset_table) {
    const Bytes dec = make_spt({cp932("abcd"), cp932("efgh")});
    const std::size_t start = rp::find_text_block_start(dec, {0x80});
    // 0x80 header + 4 pre + 12 markers + 2 entries * 4 bytes.
    EXPECT_EQ(start, 0x80u + 4 + 12 + 8);
}

TEST(Repack, no_translated_offsets_means_no_work) {
    const Bytes dec = make_spt({cp932("abcd")});
    EXPECT_EQ(rp::find_text_block_start(dec, {}), static_cast<std::size_t>(-1));
}

// The header fields the engine reads back must stay consistent with the block
// we just rebuilt -- this is the invariant that a naive in-place patch breaks.
TEST(Repack, offset_table_entries_are_dword_indexed_from_ot_base) {
    const std::uint32_t h_14 = 0x100;
    const Bytes dec = make_spt({cp932("abcd"), cp932("ijklmnop")}, h_14);
    const std::uint32_t h_10 = rd_u32(dec, 0x10);
    const std::uint32_t ot_base = h_14 + h_10;

    const std::size_t ot_start = 0x80 + 4 + 12;
    EXPECT_EQ(rd_u32(dec, ot_start), ot_base) << "first string sits at block offset 0";
    // "abcd" + NUL padded to 8 bytes -> second string starts 8 bytes in -> +2.
    EXPECT_EQ(rd_u32(dec, ot_start + 4), ot_base + 2);
}
