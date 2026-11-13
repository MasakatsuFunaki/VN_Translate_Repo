// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// BCmkri GPK+GTB reader/repacker tests.
//
// The repack path is the only fully byte-verifiable part of the narrative-CG
// pipeline (it never re-encodes an image), so these tests carry most of the
// weight: repack_identity_is_byte_exact proves chunk sizing, entry ordering,
// offset regeneration and the whole GTB writer in one assertion.
#include "gpk/gpk_archive.h"

#include "common/util.h"
#include "scoped_temp_dir.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>

using namespace mgi;
using namespace mgi::gpk;

namespace {

constexpr std::uint32_t kByteShift1 = 8u;
constexpr std::uint32_t kByteShift2 = 16u;
constexpr std::uint32_t kByteShift3 = 24u;

constexpr std::size_t kTypeFieldBytes = 16u;
constexpr std::size_t kTagPrefixLen = 4u;
constexpr std::size_t kMetaFieldStart = 16u;
constexpr std::size_t kMetaFieldEnd = 32u;
constexpr std::size_t kPackedSizeOffset = 0x20u;
constexpr std::size_t kWidthOffset = 0x24u;
constexpr std::size_t kHeightOffset = 0x28u;
constexpr std::size_t kPadStartOffset = 0x2Cu;
constexpr std::size_t kPacktypeOffset = 0x30u;
constexpr std::size_t kPacktypeBytes = 16u;

constexpr std::size_t kGtbAlignmentBytes = 8u;
constexpr std::size_t kU32FieldBytes = 4u;
constexpr std::size_t kU64FieldBytes = 8u;
constexpr std::size_t kTruncatedHeaderBytes = GPK_HEADER_SIZE - 1u;
constexpr std::uint8_t kPngDummyFillByte = 0x5Au;

void put_u32(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> kByteShift1));
    b.push_back(static_cast<std::uint8_t>(v >> kByteShift2));
    b.push_back(static_cast<std::uint8_t>(v >> kByteShift3));
}

// A 64-byte header with the given tag/dims, followed by `payload`.
Bytes make_entry(const char* tag4,
                 std::uint32_t packed_size,
                 std::uint32_t w,
                 std::uint32_t h,
                 const Bytes& payload,
                 std::uint8_t meta_fill = 0x00u) {
    Bytes hdr(GPK_HEADER_SIZE, 0u);
    for (std::size_t i = 0; i < kTypeFieldBytes; ++i) {
        hdr[i] = i < kTagPrefixLen ? static_cast<std::uint8_t>(tag4[i]) : ' ';
    }
    for (std::size_t i = kMetaFieldStart; i < kMetaFieldEnd; ++i) {
        hdr[i] = meta_fill;
    }
    const auto put = [&](std::size_t o, std::uint32_t v) {
        hdr[o] = static_cast<std::uint8_t>(v);
        hdr[o + 1] = static_cast<std::uint8_t>(v >> kByteShift1);
        hdr[o + 2] = static_cast<std::uint8_t>(v >> kByteShift2);
        hdr[o + 3] = static_cast<std::uint8_t>(v >> kByteShift3);
    };
    put(kPackedSizeOffset, packed_size);
    put(kWidthOffset, w);
    put(kHeightOffset, h);
    for (std::size_t i = kPadStartOffset; i < kPacktypeOffset; ++i) {
        hdr[i] = ' ';
    }
    for (std::size_t i = 0; i < kPacktypeBytes; ++i) {
        hdr[kPacktypeOffset + i] = PACKTYPE_DEFAULT[i];
    }
    hdr.insert(hdr.end(), payload.begin(), payload.end());
    return hdr;
}

Bytes fake_png(std::uint32_t w, std::uint32_t h, std::size_t extra = 0) {
    Bytes d = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au,
               0u, 0u, 0u, 13u, 'I', 'H', 'D', 'R'};
    d.push_back(static_cast<std::uint8_t>(w >> kByteShift3));
    d.push_back(static_cast<std::uint8_t>(w >> kByteShift2));
    d.push_back(static_cast<std::uint8_t>(w >> kByteShift1));
    d.push_back(static_cast<std::uint8_t>(w));
    d.push_back(static_cast<std::uint8_t>(h >> kByteShift3));
    d.push_back(static_cast<std::uint8_t>(h >> kByteShift2));
    d.push_back(static_cast<std::uint8_t>(h >> kByteShift1));
    d.push_back(static_cast<std::uint8_t>(h));
    d.resize(d.size() + extra, kPngDummyFillByte);
    return d;
}

// Four entries, the third carrying a deliberately bogus packed_size.
struct ArchiveFixture {
    test_util::ScopedTempDir temp_dir;
    std::string gpk;
    std::string gtb;
    std::vector<GtbEntry> entries;

    explicit ArchiveFixture(std::string_view prefix)
        : temp_dir(prefix),
          gpk(temp_dir.join_u8("a.gpk")),
          gtb(temp_dir.join_u8("a.gtb")) {
        Bytes gpk_bytes;
        const auto add = [&](const std::string& name, const Bytes& chunk) {
            entries.push_back({name, static_cast<std::uint32_t>(gpk_bytes.size())});
            gpk_bytes.insert(gpk_bytes.end(), chunk.begin(), chunk.end());
        };
        add("one", make_entry("PNG ", 24, 3, 4, fake_png(3, 4)));
        add("two", make_entry("BMP ", 6, 2, 2, Bytes{'B', 'M', 0, 0, 0, 0}));
        add("cond", make_entry("BIN ", 0xFFFFFFFFu, 0, 0, Bytes{9, 9, 9, 9}));
        add("four", make_entry("PNG ", 34, 5, 6, fake_png(5, 6, 10)));

        write_file(gpk, gpk_bytes);
        write_gtb(gtb, entries, true);
    }
};

}  // namespace

TEST(GpkGtb, gtb_round_trip_64bit) {
    const test_util::ScopedTempDir temp_dir("mgi_ut_gtb_round_trip");
    const std::string p1 = temp_dir.join_u8("a.gtb");
    const std::string p2 = temp_dir.join_u8("b.gtb");
    const std::vector<GtbEntry> entries = {{"alpha", 0}, {"bravo", 128}, {"charlie", 512}};

    write_gtb(p1, entries, true);
    const Gtb g = read_gtb(p1);
    ASSERT_EQ(g.count, 3u);
    ASSERT_TRUE(g.has_64bit);
    EXPECT_EQ(g.entries[0].name, "alpha");
    EXPECT_EQ(g.entries[2].offset, 512u);

    write_gtb(p2, g.entries, g.has_64bit);
    EXPECT_EQ(read_file(p1), read_file(p2));

    // Layout pinning: 4 + 3*4 + 3*4 + names, padded to 8, then 3*u64, 8 zeroes,
    // then the trailer.
    const Bytes raw = read_file(p1);
    const std::size_t name_len = 6u + 6u + 8u;  // NUL-terminated
    std::size_t base = kU32FieldBytes + 3u * kU32FieldBytes + 3u * kU32FieldBytes + name_len;
    while (base % kGtbAlignmentBytes != 0u) {
        ++base;
    }
    ASSERT_EQ(raw.size(), base + 3u * kU64FieldBytes + kU64FieldBytes + TRAILER_MARKER.size());
    EXPECT_EQ(0, std::memcmp(raw.data() + raw.size() - TRAILER_MARKER.size(),
                             TRAILER_MARKER.data(),
                             TRAILER_MARKER.size()));
}

TEST(GpkGtb, gtb_alignment_pad_is_variable) {
    const test_util::ScopedTempDir temp_dir("mgi_ut_gtb_alignment");
    for (const std::string& name : {std::string("a"), std::string("ab")}) {
        const std::string p = temp_dir.join_u8(name + ".gtb");
        write_gtb(p, {{name, 7}}, true);
        const Bytes raw = read_file(p);
        const std::size_t body = kU32FieldBytes * 3u + name.size() + 1u;
        std::size_t hi = body;
        while (hi % kGtbAlignmentBytes != 0u) {
            ++hi;
        }
        EXPECT_EQ(raw.size(), hi + kU64FieldBytes * 2u + TRAILER_MARKER.size());
        EXPECT_EQ(raw[hi], 7u);  // low byte of the u64 mirror
        EXPECT_EQ(read_gtb(p).entries[0].offset, 7u);
    }
}

TEST(GpkGtb, gtb_has_64bit_false_omits_tail) {
    const test_util::ScopedTempDir temp_dir("mgi_ut_gtb_no_tail");
    const std::string p = temp_dir.join_u8("n.gtb");
    write_gtb(p, {{"x", 3}}, false);
    const Bytes raw = read_file(p);
    EXPECT_EQ(raw.size(), kU32FieldBytes * 3u + 2u);  // count + off tables + "x\0"
    EXPECT_FALSE(read_gtb(p).has_64bit);
}

TEST(GpkGtb, gtb_name_cp932_replace_round_trip) {
    const test_util::ScopedTempDir temp_dir("mgi_ut_gtb_cp932");
    const std::string p = temp_dir.join_u8("jp.gtb");
    const std::string jp = "日本";
    const std::string unmapped = "₿";  // U+20BF, no CP932 mapping
    write_gtb(p, {{jp, 0}, {unmapped, 64}}, true);
    const Gtb g = read_gtb(p);
    EXPECT_EQ(g.entries[0].name, jp);
    EXPECT_EQ(g.entries[1].name, "?");  // unmappable -> '?' on the encode side
}

TEST(GpkGtb, gtb_unterminated_name_throws) {
    const test_util::ScopedTempDir temp_dir("mgi_ut_gtb_unterminated");
    const std::string p = temp_dir.join_u8("bad.gtb");
    Bytes raw;
    put_u32(raw, 1);
    put_u32(raw, 0);  // name offset
    put_u32(raw, 0);  // data offset
    raw.insert(raw.end(), {'a', 'b', 'c'});  // no NUL
    write_file(p, raw);
    try {
        read_gtb(p);
        FAIL() << "expected a throw";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("unterminated name at offset 0"),
                  std::string::npos)
            << e.what();
    }
}

TEST(GpkEntry, entry_header_fields) {
    Bytes buf = make_entry("PNG ", 5, 640, 480, Bytes{1, 2, 3, 4, 5}, 0xABu);
    buf[3] = ' ';
    buf[4] = 0x00u;  // trailing NUL/space mix in the tag
    const EntryHeader h = read_entry_header(buf, 0);
    EXPECT_EQ(h.type_tag, "PNG");
    EXPECT_EQ(h.packed_size, 5u);
    EXPECT_EQ(h.width, 640u);
    EXPECT_EQ(h.height, 480u);
    EXPECT_EQ(h.meta[0], 0xABu);
    EXPECT_EQ(h.packtype, PACKTYPE_DEFAULT);
}

TEST(GpkEntry, entry_header_truncated_throws) {
    const Bytes buf(kTruncatedHeaderBytes, 0u);
    try {
        read_entry_header(buf, 0);
        FAIL() << "expected a throw";
    } catch (const std::exception& e) {
        EXPECT_EQ(std::string(e.what()), "truncated entry header at offset 0x0");
    }
}

TEST(GpkEntry, extract_entry_clamps_at_eof) {
    Bytes buf = make_entry("PNG ", 999, 1, 1, Bytes{7, 7, 7});
    const Bytes payload = extract_entry(buf, 0);
    EXPECT_EQ(payload, (Bytes{7, 7, 7}));  // clamped at EOF, no throw
}

TEST(GpkEntry, detect_type_tag_and_dims) {
    const auto tag = [](const Bytes& b) {
        const auto t = detect_type_tag(b);
        return std::string(reinterpret_cast<const char*>(t.data()), kTypeFieldBytes);
    };
    EXPECT_EQ(tag(fake_png(3, 4)), "PNG             ");
    EXPECT_EQ(detect_dims(fake_png(1280, 720)), std::make_pair(1280u, 720u));

    // Only the 4-byte prefix gates _png_dims, but the 8-byte magic gates the tag.
    Bytes half_png = fake_png(2, 2);
    half_png[4] = 0x00u;
    EXPECT_EQ(tag(half_png), "BIN             ");
    EXPECT_EQ(detect_dims(half_png), std::make_pair(2u, 2u));

    Bytes bmp(40, 0u);
    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[18] = 20u;    // width  = 20   (LE i32)
    bmp[22] = 0xF6u;  // height = -10  (bottom-up)
    bmp[23] = 0xFFu;
    bmp[24] = 0xFFu;
    bmp[25] = 0xFFu;
    EXPECT_EQ(tag(bmp), "BMP             ");
    EXPECT_EQ(detect_dims(bmp), std::make_pair(20u, 10u));  // abs(height)

    EXPECT_EQ(tag(Bytes{0xFFu, 0xD8u, 0x00u}), "JPG             ");
    EXPECT_EQ(detect_dims(Bytes{0xFFu, 0xD8u, 0x00u}), std::make_pair(0u, 0u));
    EXPECT_EQ(tag(Bytes{'G', 'I', 'F', '8', '9'}), "GIF             ");
    EXPECT_EQ(tag(Bytes{1, 2, 3, 4}), "BIN             ");
}

TEST(GpkEntry, build_entry_falls_back_to_original_dims) {
    EntryHeader orig;
    orig.width = 111;
    orig.height = 222;
    orig.meta.fill(0xCDu);
    orig.packtype = PACKTYPE_DEFAULT;

    const Bytes junk = {1, 2, 3, 4, 5};
    const Bytes built = build_entry(orig, junk);
    const EntryHeader back = read_entry_header(built, 0);
    EXPECT_EQ(back.type_tag, "BIN");
    EXPECT_EQ(back.width, 111u);
    EXPECT_EQ(back.height, 222u);
    EXPECT_EQ(back.packed_size, junk.size());
    EXPECT_EQ(back.meta[3], 0xCDu);
    EXPECT_EQ(back.packtype, PACKTYPE_DEFAULT);
    for (std::size_t i = kPadStartOffset; i < kPacktypeOffset; ++i) {
        EXPECT_EQ(built[i], 0x20u);
    }

    const Bytes png = fake_png(64, 32, 10);
    const EntryHeader over = read_entry_header(build_entry(orig, png), 0);
    EXPECT_EQ(over.width, 64u);
    EXPECT_EQ(over.height, 32u);
}

TEST(GpkRepack, repack_identity_is_byte_exact) {
    const ArchiveFixture f("mgi_ut_repack_identity");
    const std::string out_gpk = f.temp_dir.join_u8("out.gpk");
    const std::string out_gtb = f.temp_dir.join_u8("out.gtb");
    repack(f.gpk, f.gtb, out_gpk, out_gtb, {});
    EXPECT_EQ(read_file(f.gpk), read_file(out_gpk));
    EXPECT_EQ(read_file(f.gtb), read_file(out_gtb));
}

TEST(GpkRepack, repack_chunk_size_ignores_packed_size) {
    const ArchiveFixture f("mgi_ut_repack_chunk_size");
    const std::string out_gpk = f.temp_dir.join_u8("out.gpk");
    const std::string out_gtb = f.temp_dir.join_u8("out.gtb");
    repack(f.gpk, f.gtb, out_gpk, out_gtb, {});
    // 'cond' claims packed_size 0xFFFFFFFF but occupies exactly 68 bytes; a
    // packed_size-driven copy would blow past the file.
    const Gtb g = read_gtb(out_gtb);
    EXPECT_EQ(g.entries[3].offset - g.entries[2].offset, 68u);
}

TEST(GpkRepack, repack_with_replacement_shifts_offsets) {
    const ArchiveFixture f("mgi_ut_repack_replacement");
    const std::string out_gpk = f.temp_dir.join_u8("out.gpk");
    const std::string out_gtb = f.temp_dir.join_u8("out.gtb");
    const Bytes big = fake_png(11, 22, 500);
    repack(f.gpk, f.gtb, out_gpk, out_gtb, {{"one", big}});

    const Archive arc = read_archive(out_gpk, out_gtb);
    ASSERT_EQ(arc.entries.size(), 4u);
    EXPECT_EQ(arc.entries[0].width, 11u);
    EXPECT_EQ(arc.entries[0].height, 22u);
    EXPECT_EQ(extract_entry(arc.gpk_buf, arc.entries[0].offset), big);
    // Every subsequent entry moved by exactly the size delta, and its payload
    // survives unchanged.
    EXPECT_EQ(arc.entries[1].offset, GPK_HEADER_SIZE + big.size());
    const Archive src = read_archive(f.gpk, f.gtb);
    EXPECT_EQ(extract_entry(arc.gpk_buf, arc.entries[3].offset),
              extract_entry(src.gpk_buf, src.entries[3].offset));

    // The 64-bit mirror table must agree with the 32-bit one.
    const Bytes raw = read_file(out_gtb);
    std::size_t base = kU32FieldBytes + 4u * kU32FieldBytes + 4u * kU32FieldBytes;
    for (const auto& e : arc.gtb.entries) {
        base += e.name.size() + 1u;
    }
    while (base % kGtbAlignmentBytes != 0u) {
        ++base;
    }
    for (std::size_t i = 0; i < 4u; ++i) {
        std::uint64_t v = 0;
        for (int b = 7; b >= 0; --b) {
            v = (v << kByteShift1) | raw[base + i * kU64FieldBytes + static_cast<std::size_t>(b)];
        }
        EXPECT_EQ(v, arc.gtb.entries[i].offset);
    }
}

TEST(GpkRepack, repack_duplicate_offsets) {
    const test_util::ScopedTempDir temp_dir("mgi_ut_repack_dup_offsets");
    const std::string gpk = temp_dir.join_u8("d.gpk");
    const std::string gtb = temp_dir.join_u8("d.gtb");
    Bytes buf = make_entry("PNG ", 24, 3, 4, fake_png(3, 4));
    const Bytes second = make_entry("PNG ", 24, 7, 8, fake_png(7, 8));
    const std::uint32_t off2 = static_cast<std::uint32_t>(buf.size());
    buf.insert(buf.end(), second.begin(), second.end());
    write_file(gpk, buf);
    // Two names pointing at the SAME offset, then a third at the real second.
    write_gtb(gtb, {{"a", 0}, {"b", 0}, {"c", off2}}, true);

    const std::string out_gpk = temp_dir.join_u8("o.gpk");
    const std::string out_gtb = temp_dir.join_u8("o.gtb");
    repack(gpk, gtb, out_gpk, out_gtb, {});

    const Archive arc = read_archive(out_gpk, out_gtb);
    ASSERT_EQ(arc.entries.size(), 3u);
    // The duplicate-collapsing zip() maps offset 0 to off2, so both 'a' and 'b'
    // copy the SAME chunk and the output grows by one entry's worth.
    EXPECT_EQ(arc.entries[1].offset, off2);
    EXPECT_EQ(extract_entry(arc.gpk_buf, arc.entries[0].offset),
              extract_entry(arc.gpk_buf, arc.entries[1].offset));
    EXPECT_EQ(arc.entries[2].width, 7u);
}
