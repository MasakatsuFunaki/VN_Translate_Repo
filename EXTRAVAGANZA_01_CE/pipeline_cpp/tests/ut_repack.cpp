// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for SPT repacking: structural invariants (4-byte alignment,
// offset-table pointers, section pre_value, VA relocation), speaker-line
// merging, newline normalization, and codepoint-aware word wrapping.
#include "repack/repack.h"

#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rp = exc::repack;
using namespace exc;

namespace {

constexpr unsigned int kByte1Shift = 8;
constexpr unsigned int kByte2Shift = 16;
constexpr unsigned int kByte3Shift = 24;
constexpr std::size_t kByteIndex0 = 0;
constexpr std::size_t kByteIndex1 = 1;
constexpr std::size_t kByteIndex2 = 2;
constexpr std::size_t kByteIndex3 = 3;
constexpr std::size_t kDwordAlignment = 4;
constexpr std::size_t kSptHeaderSize = 0x80;
constexpr std::uint8_t kHeaderFillByte = 0x20;
constexpr std::size_t kCountHeaderOffset = 0x10;
constexpr std::size_t kVaBaseHeaderOffset = 0x14;
constexpr std::uint32_t kDefaultVaBase = 0x100;
constexpr std::size_t kSectionMarkerBytes = 16;
constexpr std::uint32_t kMarker6666 = 0x66660001u;
constexpr std::uint32_t kTextType5555 = 0x55550002u;
constexpr std::uint32_t kTextTag4444 = 0x44440002u;
constexpr std::uint32_t kTrailingPreWords = 4;
constexpr std::uint32_t kTrailingType5555 = 0x55550003u;
constexpr std::uint32_t kTrailingTag4444 = 0x44440003u;
constexpr std::size_t kLongPlateLength = 30;
constexpr int kWrapBudget7 = 7;
constexpr int kWrapBudget20 = 20;
constexpr int kWrapBudget4 = 4;
constexpr int kWrapBudget10 = 10;
constexpr int kExtraOverflowLines = 4;
constexpr std::size_t kCrlfLength = 2;
constexpr std::size_t kFakeBufferSize = 64;
constexpr std::uint8_t kFakeLowWordByte0 = 0x01;
constexpr std::uint8_t kFakeMarkerByte = 0x66;
constexpr std::size_t kTwoPayloadCount = 2;
constexpr std::uint32_t kSecondStringDwordOffset = 2;

void put_u32(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> kByte1Shift));
    b.push_back(static_cast<std::uint8_t>(v >> kByte2Shift));
    b.push_back(static_cast<std::uint8_t>(v >> kByte3Shift));
}

std::uint32_t rd_u32(const Bytes& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off + kByteIndex0]) |
           (static_cast<std::uint32_t>(b[off + kByteIndex1]) << kByte1Shift) |
           (static_cast<std::uint32_t>(b[off + kByteIndex2]) << kByte2Shift) |
           (static_cast<std::uint32_t>(b[off + kByteIndex3]) << kByte3Shift);
}

Bytes make_spt(const std::vector<Bytes>& payloads, std::uint32_t h_14 = kDefaultVaBase) {
    const std::uint32_t h_10 = static_cast<std::uint32_t>(payloads.size());

    Bytes block;
    for (const auto& p : payloads) {
        block.insert(block.end(), p.begin(), p.end());
        block.push_back(0);
        while (block.size() % kDwordAlignment != 0) {
            block.push_back(0);
        }
    }

    Bytes dec(kSptHeaderSize, kHeaderFillByte);
    dec[kCountHeaderOffset + kByteIndex0] = static_cast<std::uint8_t>(h_10);
    dec[kCountHeaderOffset + kByteIndex1] = 0;
    dec[kCountHeaderOffset + kByteIndex2] = 0;
    dec[kCountHeaderOffset + kByteIndex3] = 0;
    dec[kVaBaseHeaderOffset + kByteIndex0] = static_cast<std::uint8_t>(h_14);
    dec[kVaBaseHeaderOffset + kByteIndex1] = static_cast<std::uint8_t>(h_14 >> kByte1Shift);
    dec[kVaBaseHeaderOffset + kByteIndex2] = 0;
    dec[kVaBaseHeaderOffset + kByteIndex3] = 0;

    const std::size_t ot_bytes = static_cast<std::size_t>(h_10) * kDwordAlignment;
    const std::uint32_t pre = static_cast<std::uint32_t>(
        (ot_bytes + block.size() + kSectionMarkerBytes) / kDwordAlignment);
    put_u32(dec, pre);
    put_u32(dec, kMarker6666);
    put_u32(dec, kTextType5555);
    put_u32(dec, kTextTag4444);

    const std::uint32_t ot_base = h_14 + h_10;
    std::size_t off = 0;
    for (const auto& p : payloads) {
        put_u32(dec, ot_base + static_cast<std::uint32_t>(off / kDwordAlignment));
        std::size_t n = p.size() + 1;
        n = ((n + (kDwordAlignment - 1)) / kDwordAlignment) * kDwordAlignment;
        off += n;
    }
    dec.insert(dec.end(), block.begin(), block.end());

    put_u32(dec, kTrailingPreWords);
    put_u32(dec, kMarker6666);
    put_u32(dec, kTrailingType5555);
    put_u32(dec, kTrailingTag4444);
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
    const std::string longish(kLongPlateLength, 'x');
    EXPECT_EQ(rp::merge_speaker_line(longish + "\r\nrest"), longish + "\r\nrest");
    EXPECT_EQ(rp::merge_speaker_line("\"Ah\"\r\nrest"), "\"Ah\"\r\nrest");
    EXPECT_EQ(rp::merge_speaker_line("no break"), "no break");
}

TEST(Repack, sanitize_newlines_normalises_to_crlf) {
    EXPECT_EQ(rp::sanitize_newlines("a\nb"), "a\r\nb");
    EXPECT_EQ(rp::sanitize_newlines("a\r\nb"), "a\r\nb");
    EXPECT_EQ(rp::sanitize_newlines("\n\na"), "a");
    EXPECT_EQ(rp::sanitize_newlines("  a"), "  a");
}

TEST(Repack, reflow_lines_joins_internal_breaks_and_preserves_trailing_crlf) {
    EXPECT_EQ(
        rp::word_wrap(rp::reflow_lines(
            "Mizuna, who had been fighting the Distortions as the Priestess of Genbu,\r\n"
            "had been captured by a man calling himself Shirou Kitabatake.\r\n")),
        "Mizuna, who had been fighting the Distortions as the\r\n"
        "Priestess of Genbu, had been captured by a man calling\r\n"
        "himself Shirou Kitabatake.\r\n");
    EXPECT_EQ(rp::reflow_lines("para1\r\n\r\npara2\r\n"), "para1\r\n\r\npara2\r\n");
    EXPECT_EQ(rp::reflow_lines("line1 \r\n  line2"), "line1 line2");
}

TEST(Repack, word_wrap_breaks_and_caps_lines) {
    EXPECT_EQ(rp::word_wrap("aaa bbb ccc ddd", kWrapBudget7), "aaa bbb\r\nccc ddd");
    EXPECT_EQ(rp::word_wrap("short\r\nalso short", kWrapBudget20), "short\r\nalso short");
    EXPECT_EQ(rp::word_wrap("abcdefghij", kWrapBudget4), "abcdefghij");

    std::string many;
    for (int i = 0; i < rp::MAX_LINES + kExtraOverflowLines; ++i) {
        many += "line" + std::to_string(i) + "\r\n";
    }
    const std::string wrapped = rp::word_wrap(many, rp::MAX_LINE_CHARS);
    std::size_t lines = 1;
    for (std::size_t i = 0; i + 1 < wrapped.size(); ++i) {
        if (wrapped.compare(i, kCrlfLength, "\r\n") == 0) {
            ++lines;
        }
    }
    EXPECT_LE(lines, static_cast<std::size_t>(rp::MAX_LINES));
}

TEST(Repack, word_wrap_counts_codepoints_not_bytes) {
    const std::string jp = "あ い う";
    EXPECT_EQ(rp::word_wrap(jp, kWrapBudget10), jp)
        << "wrapped a 5-char line at a 10-char budget";
}

TEST(Repack, parses_text_block_of_aligned_strings) {
    const Bytes dec = make_spt({cp932("あ"), cp932("hello"), cp932("x")});
    std::size_t block_end = 0;
    const std::size_t start = rp::find_text_block_start(dec, {kSptHeaderSize});
    const auto strings = rp::parse_text_block(dec, start, &block_end);

    ASSERT_EQ(strings.size(), 3u);
    EXPECT_EQ(strings[0].raw, cp932("あ"));
    EXPECT_EQ(strings[1].raw, cp932("hello"));
    EXPECT_EQ(strings[2].raw, cp932("x"));
    for (const auto& s : strings) {
        EXPECT_EQ((s.offset - start) % kDwordAlignment, 0u);
    }
    EXPECT_GT(block_end, strings.back().offset);
}

TEST(Repack, find_next_section_marker_needs_a_5555_type_word) {
    Bytes dec = make_spt({cp932("abcd")});
    const std::size_t start = rp::find_text_block_start(dec, {kSptHeaderSize});
    EXPECT_LT(rp::find_next_section_marker(dec, start), dec.size());

    Bytes fake(kFakeBufferSize, 0);
    fake[kByteIndex0] = kFakeLowWordByte0;
    fake[kByteIndex1] = 0x00;
    fake[kByteIndex2] = kFakeMarkerByte;
    fake[kByteIndex3] = kFakeMarkerByte;
    EXPECT_EQ(rp::find_next_section_marker(fake, 0), fake.size());
}

TEST(Repack, block_start_skips_the_offset_table) {
    const Bytes dec = make_spt({cp932("abcd"), cp932("efgh")});
    const std::size_t start = rp::find_text_block_start(dec, {kSptHeaderSize});
    EXPECT_EQ(start,
              kSptHeaderSize + kSectionMarkerBytes + kTwoPayloadCount * kDwordAlignment);
}

TEST(Repack, no_translated_offsets_means_no_work) {
    const Bytes dec = make_spt({cp932("abcd")});
    EXPECT_EQ(rp::find_text_block_start(dec, {}), static_cast<std::size_t>(-1));
}

TEST(Repack, offset_table_entries_are_dword_indexed_from_ot_base) {
    const Bytes dec = make_spt({cp932("abcd"), cp932("ijklmnop")}, kDefaultVaBase);
    const std::uint32_t h_10 = rd_u32(dec, kCountHeaderOffset);
    const std::uint32_t ot_base = kDefaultVaBase + h_10;

    const std::size_t ot_start = kSptHeaderSize + kSectionMarkerBytes;
    EXPECT_EQ(rd_u32(dec, ot_start), ot_base) << "first string sits at block offset 0";
    EXPECT_EQ(rd_u32(dec, ot_start + kDwordAlignment),
              ot_base + kSecondStringDwordOffset);
}
