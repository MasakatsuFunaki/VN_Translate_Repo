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

#include "cg/image.h"
#include "common/util.h"
#include "scoped_temp_dir.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <stdexcept>
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

namespace {

void put_u16_local(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> kByteShift1));
}

void put_i32_local(Bytes& b, std::int32_t v) {
    put_u32(b, static_cast<std::uint32_t>(v));
}

std::uint16_t rd_u16_local(const Bytes& b, std::size_t off) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(b[off]) |
        (static_cast<std::uint16_t>(b[off + 1u]) << kByteShift1));
}

std::uint32_t rd_u32_local(const Bytes& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1u]) << kByteShift1) |
           (static_cast<std::uint32_t>(b[off + 2u]) << kByteShift2) |
           (static_cast<std::uint32_t>(b[off + 3u]) << kByteShift3);
}

std::int32_t rd_i32_local(const Bytes& b, std::size_t off) {
    return static_cast<std::int32_t>(rd_u32_local(b, off));
}

// Synthetic PACKTYPE=2A DWQ with configurable sparse mask palette.
Bytes make_sparse_2a_dwq(const DwqRgbaImage& img,
                         const std::vector<std::uint8_t>& mask_pal_values,
                         std::uint16_t bpp_field = 24u) {
    constexpr std::size_t kBmpHdrBytes = 54u;
    const std::size_t stride = ((static_cast<std::size_t>(img.width) * 3u) + 3u) & ~std::size_t(3u);
    const std::size_t bmp_px = stride * static_cast<std::size_t>(img.height);
    const std::size_t bmp_total = kBmpHdrBytes + bmp_px;

    Bytes dwq(GPK_HEADER_SIZE, ' ');
    std::memcpy(dwq.data(), "BMP+MASK        ", 16u);
    for (std::size_t m = 0; m < 4u; ++m) {
        const std::size_t o = 0x10u + m * 4u;
        dwq[o] = 1u;
        dwq[o + 1u] = 0u;
        dwq[o + 2u] = 0u;
        dwq[o + 3u] = 0u;
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
    wr32(0x24u, static_cast<std::uint32_t>(img.width));
    wr16(0x28u, static_cast<std::uint16_t>(img.height));
    wr16(0x2Au, bpp_field);
    std::memcpy(dwq.data() + 0x30u, "PACKTYPE=2A     ", 16u);

    // 24-bit top-down BMP payload.
    dwq.push_back('B');
    dwq.push_back('M');
    put_u32(dwq, static_cast<std::uint32_t>(bmp_total));
    put_u16_local(dwq, 0u);
    put_u16_local(dwq, 0u);
    put_u32(dwq, static_cast<std::uint32_t>(kBmpHdrBytes));
    put_u32(dwq, 40u);
    put_i32_local(dwq, img.width);
    put_i32_local(dwq, img.height);
    put_u16_local(dwq, 1u);
    put_u16_local(dwq, 24u);
    put_u32(dwq, 0u);
    put_u32(dwq, static_cast<std::uint32_t>(bmp_px));
    put_i32_local(dwq, 0);
    put_i32_local(dwq, 0);
    put_u32(dwq, 0u);
    put_u32(dwq, 0u);

    Bytes bgr(bmp_px, 0u);
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const std::size_t si = (static_cast<std::size_t>(y) * img.width + x) * 4u;
            const std::size_t di = static_cast<std::size_t>(y) * stride + x * 3u;
            bgr[di + 0u] = img.pixels[si + 2u];
            bgr[di + 1u] = img.pixels[si + 1u];
            bgr[di + 2u] = img.pixels[si + 0u];
        }
    }
    dwq.insert(dwq.end(), bgr.begin(), bgr.end());

    // Sparse-palette 8-bit mask BMP + RLE-XOR alpha index stream.
    const std::size_t total_px =
        static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height);
    Bytes alpha_indices(total_px, 0u);
    for (std::size_t i = 0; i < total_px; ++i) {
        const std::uint8_t a = img.pixels[i * 4u + 3u];
        for (std::size_t p = 0; p < mask_pal_values.size(); ++p) {
            if (mask_pal_values[p] == a) {
                alpha_indices[i] = static_cast<std::uint8_t>(p);
                break;
            }
        }
    }
    const Bytes rle = encode_rle_xor_mask(alpha_indices.data(), img.width, img.height);
    const std::size_t pal_bytes = mask_pal_values.size() * 4u;
    const std::uint32_t mask_bf_off = static_cast<std::uint32_t>(kBmpHdrBytes + pal_bytes);

    dwq.push_back('B');
    dwq.push_back('M');
    put_u32(dwq, static_cast<std::uint32_t>(mask_bf_off + total_px));
    put_u16_local(dwq, 0u);
    put_u16_local(dwq, 0u);
    put_u32(dwq, mask_bf_off);
    put_u32(dwq, 40u);
    put_i32_local(dwq, img.width);
    put_i32_local(dwq, img.height);
    put_u16_local(dwq, 1u);
    put_u16_local(dwq, 8u);
    put_u32(dwq, 0u);
    put_u32(dwq, static_cast<std::uint32_t>(total_px));
    put_i32_local(dwq, 0);
    put_i32_local(dwq, 0);
    put_u32(dwq, static_cast<std::uint32_t>(mask_pal_values.size()));
    put_u32(dwq, 0u);
    for (std::uint8_t v : mask_pal_values) {
        dwq.push_back(v);
        dwq.push_back(v);
        dwq.push_back(v);
        dwq.push_back(0u);
    }
    dwq.insert(dwq.end(), rle.begin(), rle.end());
    return dwq;
}

Bytes make_3a_dwq(const DwqRgbaImage& img) {
    constexpr std::size_t kBmpHdrBytes = 54u;
    const std::size_t total_px =
        static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height);

    // 4-color palette: {0,0,0}, {200,40,20}, {10,180,90}, {50,60,220}
    const std::array<std::array<std::uint8_t, 3>, 4> pal = {{
        {0u, 0u, 0u},
        {200u, 40u, 20u},
        {10u, 180u, 90u},
        {50u, 60u, 220u},
    }};
    Bytes color_indices(total_px, 0u);
    Bytes alpha(total_px, 0u);
    for (std::size_t i = 0; i < total_px; ++i) {
        const std::uint8_t r = img.pixels[i * 4u + 0u];
        const std::uint8_t g = img.pixels[i * 4u + 1u];
        const std::uint8_t b = img.pixels[i * 4u + 2u];
        alpha[i] = img.pixels[i * 4u + 3u];
        for (std::size_t p = 0; p < pal.size(); ++p) {
            if (pal[p][0] == r && pal[p][1] == g && pal[p][2] == b) {
                color_indices[i] = static_cast<std::uint8_t>(p);
                break;
            }
        }
    }
    const Bytes color_rle = encode_rle_xor_mask(color_indices.data(), img.width, img.height);
    const std::uint32_t color_bf_off = static_cast<std::uint32_t>(kBmpHdrBytes + pal.size() * 4u);
    const std::uint32_t color_section = color_bf_off + static_cast<std::uint32_t>(color_rle.size());

    Bytes dwq(GPK_HEADER_SIZE, ' ');
    std::memcpy(dwq.data(), "PACKBMP         ", 16u);
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
    wr32(0x20u, color_section);
    wr32(0x24u, static_cast<std::uint32_t>(img.width));
    wr16(0x28u, static_cast<std::uint16_t>(img.height));
    wr16(0x2Au, 8u);
    std::memcpy(dwq.data() + 0x30u, "PACKTYPE=3A     ", 16u);

    // 8-bit palettized RLE-XOR color BMP.
    dwq.push_back('B');
    dwq.push_back('M');
    put_u32(dwq, color_section);
    put_u16_local(dwq, 0u);
    put_u16_local(dwq, 0u);
    put_u32(dwq, color_bf_off);
    put_u32(dwq, 40u);
    put_i32_local(dwq, img.width);
    put_i32_local(dwq, img.height);
    put_u16_local(dwq, 1u);
    put_u16_local(dwq, 8u);
    put_u32(dwq, 0u);
    put_u32(dwq, static_cast<std::uint32_t>(total_px));
    put_i32_local(dwq, 0);
    put_i32_local(dwq, 0);
    put_u32(dwq, static_cast<std::uint32_t>(pal.size()));
    put_u32(dwq, 0u);
    for (const auto& c : pal) {
        dwq.push_back(c[2]);
        dwq.push_back(c[1]);
        dwq.push_back(c[0]);
        dwq.push_back(0u);
    }
    dwq.insert(dwq.end(), color_rle.begin(), color_rle.end());

    // 256-entry identity mask BMP.
    const Bytes mask_rle = encode_rle_xor_mask(alpha.data(), img.width, img.height);
    dwq.push_back('B');
    dwq.push_back('M');
    put_u32(dwq, static_cast<std::uint32_t>(kBmpHdrBytes + 1024u + total_px));
    put_u16_local(dwq, 0u);
    put_u16_local(dwq, 0u);
    put_u32(dwq, static_cast<std::uint32_t>(kBmpHdrBytes + 1024u));
    put_u32(dwq, 40u);
    put_i32_local(dwq, img.width);
    put_i32_local(dwq, img.height);
    put_u16_local(dwq, 1u);
    put_u16_local(dwq, 8u);
    put_u32(dwq, 0u);
    put_u32(dwq, static_cast<std::uint32_t>(total_px));
    put_i32_local(dwq, 0);
    put_i32_local(dwq, 0);
    put_u32(dwq, 256u);
    put_u32(dwq, 0u);
    for (std::uint32_t i = 0; i < 256u; ++i) {
        const auto g = static_cast<std::uint8_t>(i);
        dwq.push_back(g);
        dwq.push_back(g);
        dwq.push_back(g);
        dwq.push_back(0u);
    }
    dwq.insert(dwq.end(), mask_rle.begin(), mask_rle.end());
    return dwq;
}

}  // namespace

TEST(GpkEntry, header_preserves_u16_height_u16_bpp_type_raw_and_png_crop_margins) {
    Bytes raw(GPK_HEADER_SIZE, 0u);
    std::memcpy(raw.data(), "BMP+MASK        ", 16u);
    for (std::size_t m = 0; m < 4u; ++m) {
        const std::size_t o = 0x10u + m * 4u;
        raw[o] = 99u;
    }
    raw[0x20u] = 4u;  // packed_size = 4
    raw[0x24u] = 10u; // width = 10
    raw[0x28u] = 8u;  // u16 height = 8
    raw[0x2Au] = 32u; // u16 bpp = 32 (non-zero!)
    raw[0x2Cu] = 'X';
    raw[0x2Du] = 'Y';
    raw[0x2Eu] = 'Z';
    raw[0x2Fu] = 'W';
    std::memcpy(raw.data() + 0x30u, PACKTYPE_DEFAULT.data(), 16u);

    const EntryHeader parsed = read_entry_header(raw, 0u);
    EXPECT_EQ(parsed.type_tag, "BMP+MASK");
    EXPECT_EQ(parsed.width, 10u);
    EXPECT_EQ(parsed.height, 8u);
    EXPECT_EQ(parsed.bpp, 32u);

    // Real 4-channel RGBA PNG with active pixels only at (x=3..5, y=2..6) in 10x8:
    // expected margins = {top=2, left=3, right=4, bottom=1}.
    cg::Image rgba;
    rgba.width = 10;
    rgba.height = 8;
    rgba.channels = 4;
    rgba.px.assign(10u * 8u * 4u, 0u);
    for (int y = 2; y <= 6; ++y) {
        for (int x = 3; x <= 5; ++x) {
            std::uint8_t* p = rgba.at(x, y);
            p[0] = 120u;
            p[1] = 200u;
            p[2] = 60u;
            p[3] = 255u;
        }
    }
    const Bytes png = cg::encode_png(rgba);
    const Bytes built = build_entry(parsed, png);

    EXPECT_EQ(std::memcmp(built.data(), "BMP+MASK        ", 16u), 0);
    EXPECT_EQ(rd_i32_local(built, 0x10u), 2);
    EXPECT_EQ(rd_i32_local(built, 0x14u), 3);
    EXPECT_EQ(rd_i32_local(built, 0x18u), 4);
    EXPECT_EQ(rd_i32_local(built, 0x1Cu), 1);
    EXPECT_EQ(rd_u16_local(built, 0x28u), 8u);
    EXPECT_EQ(rd_u16_local(built, 0x2Au), 32u);
    EXPECT_EQ(built[0x2Cu], 'X');
    EXPECT_EQ(built[0x2Fu], 'W');

    const EntryHeader back = read_entry_header(built, 0u);
    EXPECT_EQ(back.height, 8u);
    EXPECT_EQ(back.bpp, 32u);
}

TEST(GpkGtb, over2g_u64_offsets_above_4gib_round_trip_without_truncation) {
    const test_util::ScopedTempDir temp_dir("mgi_ut_gtb_over2g_u64");
    const std::string p = temp_dir.join_u8("big.gtb");
    constexpr std::uint64_t kOff1 = 0x100000480ull;
    constexpr std::uint64_t kOff2 = 0x200001200ull;

    const std::vector<GtbEntry> entries = {
        {"first", 0ull},
        {"above_4g", kOff1},
        {"above_8g", kOff2},
    };
    write_gtb(p, entries, true);

    const Gtb back = read_gtb(p);
    ASSERT_TRUE(back.has_64bit);
    ASSERT_EQ(back.entries.size(), 3u);
    EXPECT_EQ(back.entries[0].offset, 0ull);
    EXPECT_EQ(back.entries[1].offset, kOff1);
    EXPECT_EQ(back.entries[2].offset, kOff2);

    // Reject writing >4GiB offsets without over2G! 64-bit table.
    const std::string bad_no64 = temp_dir.join_u8("no64.gtb");
    EXPECT_THROW(write_gtb(bad_no64, entries, false), std::runtime_error);

    // Reject desynced low-32 bits between u32 and u64 tables.
    Bytes tampered = read_file(p);
    const std::size_t u64_entry1_pos = tampered.size() - 16u - 2u * 8u;
    tampered[u64_entry1_pos] ^= 0xFFu;
    const std::string bad_desync = temp_dir.join_u8("desync.gtb");
    write_file(bad_desync, tampered);
    EXPECT_THROW(read_gtb(bad_desync), std::runtime_error);
}

TEST(GpkEntry, packtype_2a_sparse_mask_and_3a_preserve_alpha_masks_and_headers) {
    // 1. Sparse-palette PACKTYPE=2A (4-entry mask palette: bfOffBits = 70 < 1078)
    // exercising sign-bit alpha values 0x00, 0x7F, 0x80, 0xFF and hidden BGR.
    DwqRgbaImage orig_2a;
    orig_2a.width = 8;
    orig_2a.height = 6;
    orig_2a.pixels.assign(8u * 6u * 4u, 0u);
    // Transparent pixel (0,0) carries hidden BGR (11,22,33).
    orig_2a.pixels[0] = 33u;
    orig_2a.pixels[1] = 22u;
    orig_2a.pixels[2] = 11u;
    orig_2a.pixels[3] = 0x00u;
    // Active block (x=2..5, y=1..4) with boundary alpha values.
    const std::vector<std::uint8_t> sparse_pal = {0x00u, 0x7Fu, 0x80u, 0xFFu};
    for (int y = 1; y <= 4; ++y) {
        for (int x = 2; x <= 5; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 8u + x) * 4u;
            orig_2a.pixels[p + 0u] = static_cast<std::uint8_t>(20u + x * 10u);
            orig_2a.pixels[p + 1u] = static_cast<std::uint8_t>(50u + y * 15u);
            orig_2a.pixels[p + 2u] = 190u;
            orig_2a.pixels[p + 3u] = sparse_pal[static_cast<std::size_t>((x + y) % 3 + 1)];
        }
    }

    const Bytes raw_2a = make_sparse_2a_dwq(orig_2a, sparse_pal, 24u);
    const DwqRgbaImage decoded_2a = decode_dwq_masked_rgba(raw_2a.data(), raw_2a.size());
    EXPECT_EQ(decoded_2a.width, orig_2a.width);
    EXPECT_EQ(decoded_2a.height, orig_2a.height);
    EXPECT_EQ(decoded_2a.pixels, orig_2a.pixels);

    // Replace with 32-bpp RGBA PNG introducing new alpha values (0x33, 0xFE)
    // outside the original 4-entry sparse palette and expanded bounds (x=1..6, y=0..5).
    cg::Image repl_rgba;
    repl_rgba.width = 8;
    repl_rgba.height = 6;
    repl_rgba.channels = 4;
    repl_rgba.px.assign(8u * 6u * 4u, 0u);
    for (int y = 0; y <= 5; ++y) {
        for (int x = 1; x <= 6; ++x) {
            std::uint8_t* p = repl_rgba.at(x, y);
            p[0] = 240u;
            p[1] = 80u;
            p[2] = 40u;
            p[3] = (x == 1) ? 0x33u : ((x == 6) ? 0xFEu : 0xFFu);
        }
    }
    const Bytes repl_png = cg::encode_png(repl_rgba);
    const Bytes rebuilt_2a = build_entry_from_raw(raw_2a.data(), raw_2a.size(), repl_png);

    const EntryHeader hdr_2a = read_entry_header(rebuilt_2a, 0u);
    EXPECT_EQ(hdr_2a.type_tag, "BMP+MASK");
    EXPECT_EQ(packtype_char(hdr_2a), '2');
    EXPECT_EQ(hdr_2a.bpp, 24u);
    EXPECT_EQ(rd_i32_local(rebuilt_2a, 0x10u), 0);  // top
    EXPECT_EQ(rd_i32_local(rebuilt_2a, 0x14u), 1);  // left
    EXPECT_EQ(rd_i32_local(rebuilt_2a, 0x18u), 1);  // right (8 - 1 - 6)
    EXPECT_EQ(rd_i32_local(rebuilt_2a, 0x1Cu), 0);  // bottom

    // Mask BMP header rewritten for 256-entry grayscale palette.
    const std::size_t mask_off = GPK_HEADER_SIZE + hdr_2a.packed_size;
    ASSERT_GT(rebuilt_2a.size(), mask_off + 1078u);
    EXPECT_EQ(rebuilt_2a[mask_off], 'B');
    EXPECT_EQ(rebuilt_2a[mask_off + 1u], 'M');
    EXPECT_EQ(rd_u32_local(rebuilt_2a, mask_off + 2u), 1078u + 8u * 6u);  // bfSize
    EXPECT_EQ(rd_u32_local(rebuilt_2a, mask_off + 10u), 1078u);           // bfOffBits
    EXPECT_EQ(rd_u32_local(rebuilt_2a, mask_off + 46u), 256u);            // biClrUsed
    EXPECT_EQ(rd_u32_local(rebuilt_2a, mask_off + 50u), 0u);              // biClrImportant
    for (std::uint32_t pal_idx : {0u, 1u, 127u, 128u, 254u, 255u}) {
        const std::size_t p_off = mask_off + 54u + static_cast<std::size_t>(pal_idx) * 4u;
        EXPECT_EQ(rebuilt_2a[p_off + 0u], static_cast<std::uint8_t>(pal_idx));
        EXPECT_EQ(rebuilt_2a[p_off + 1u], static_cast<std::uint8_t>(pal_idx));
        EXPECT_EQ(rebuilt_2a[p_off + 2u], static_cast<std::uint8_t>(pal_idx));
        EXPECT_EQ(rebuilt_2a[p_off + 3u], 0u);
    }

    const DwqRgbaImage redecoded_2a = decode_dwq_masked_rgba(rebuilt_2a.data(), rebuilt_2a.size());
    ASSERT_EQ(redecoded_2a.width, 8);
    ASSERT_EQ(redecoded_2a.height, 6);
    ASSERT_EQ(redecoded_2a.pixels.size(), 8u * 6u * 4u);
    EXPECT_EQ(redecoded_2a.pixels[0], 33u);  // hidden BGR preserved on (0,0,0,0)
    EXPECT_EQ(redecoded_2a.pixels[1], 22u);
    EXPECT_EQ(redecoded_2a.pixels[2], 11u);
    EXPECT_EQ(redecoded_2a.pixels[3], 0u);
    EXPECT_EQ(redecoded_2a.pixels[(0u * 8u + 1u) * 4u + 3u], 0x33u);
    EXPECT_EQ(redecoded_2a.pixels[(5u * 8u + 6u) * 4u + 3u], 0xFEu);

    // 2. Passing 3-channel RGB BMP to build_entry_from_raw on a 2A entry must
    // preserve the original alpha mask verbatim rather than dropping it.
    cg::Image rgb_only = cg::drop_alpha(repl_rgba);
    const Bytes rebuilt_2a_from_rgb =
        build_entry_from_raw(raw_2a.data(), raw_2a.size(), cg::encode_bmp(rgb_only));
    const DwqRgbaImage from_rgb_decoded =
        decode_dwq_masked_rgba(rebuilt_2a_from_rgb.data(), rebuilt_2a_from_rgb.size());
    ASSERT_EQ(from_rgb_decoded.pixels.size(), orig_2a.pixels.size());
    for (std::size_t i = 0; i < 8u * 6u; ++i) {
        EXPECT_EQ(from_rgb_decoded.pixels[i * 4u + 3u], orig_2a.pixels[i * 4u + 3u]);
    }

    // 3. PACKTYPE=3A (8-bit palettized RLE-XOR color + 8-bit RLE-XOR mask).
    DwqRgbaImage orig_3a;
    orig_3a.width = 6;
    orig_3a.height = 4;
    orig_3a.pixels.assign(6u * 4u * 4u, 0u);
    for (int y = 1; y <= 2; ++y) {
        for (int x = 1; x <= 4; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 6u + x) * 4u;
            orig_3a.pixels[p + 0u] = 200u;
            orig_3a.pixels[p + 1u] = 40u;
            orig_3a.pixels[p + 2u] = 20u;
            orig_3a.pixels[p + 3u] = (x == 1) ? 0x80u : 0xFFu;
        }
    }
    const Bytes raw_3a = make_3a_dwq(orig_3a);
    const DwqRgbaImage decoded_3a = decode_dwq_masked_rgba(raw_3a.data(), raw_3a.size());
    EXPECT_EQ(decoded_3a.pixels, orig_3a.pixels);

    cg::Image repl_3a;
    repl_3a.width = 6;
    repl_3a.height = 4;
    repl_3a.channels = 4;
    repl_3a.px = orig_3a.pixels;
    repl_3a.at(0, 0)[0] = 50u;
    repl_3a.at(0, 0)[1] = 60u;
    repl_3a.at(0, 0)[2] = 220u;
    repl_3a.at(0, 0)[3] = 0x7Fu;
    const Bytes rebuilt_3a =
        build_entry_from_raw(raw_3a.data(), raw_3a.size(), cg::encode_png(repl_3a));
    const EntryHeader hdr_3a = read_entry_header(rebuilt_3a, 0u);
    EXPECT_EQ(hdr_3a.type_tag, "PACKBMP");
    EXPECT_EQ(packtype_char(hdr_3a), '3');
    EXPECT_EQ(hdr_3a.bpp, 8u);
    const DwqRgbaImage redecoded_3a = decode_dwq_masked_rgba(rebuilt_3a.data(), rebuilt_3a.size());
    EXPECT_EQ(redecoded_3a.pixels, repl_3a.px);

    // 4. build_entry updates 0x10..0x1F crop margins for 32-bpp RGBA BMP payloads too.
    EntryHeader bmp32_hdr{};
    bmp32_hdr.type_raw = {'B', 'M', 'P', ' ', ' ', ' ', ' ', ' ',
                          ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    bmp32_hdr.bpp = 32u;
    bmp32_hdr.packtype = {'P', 'A', 'C', 'K', 'T', 'Y', 'P', 'E',
                          '=', '5', 'A', ' ', ' ', ' ', ' ', ' '};
    const Bytes bmp32_payload = cg::encode_bmp(repl_rgba);
    const Bytes built_bmp32 = build_entry(bmp32_hdr, bmp32_payload);
    EXPECT_EQ(rd_i32_local(built_bmp32, 0x10u), 0);  // top
    EXPECT_EQ(rd_i32_local(built_bmp32, 0x14u), 1);  // left
    EXPECT_EQ(rd_i32_local(built_bmp32, 0x18u), 1);  // right
    EXPECT_EQ(rd_i32_local(built_bmp32, 0x1Cu), 0);  // bottom
}

TEST(GpkEntry, corrupt_mask_headers_dimensions_and_over2g_name_collisions_throw_cleanly) {
    DwqRgbaImage base_img;
    base_img.width = 4;
    base_img.height = 4;
    base_img.pixels.assign(4u * 4u * 4u, 0u);
    base_img.pixels[3] = 0xFFu;
    const std::vector<std::uint8_t> sparse_pal = {0x00u, 0xFFu};
    const Bytes valid_2a = make_sparse_2a_dwq(base_img, sparse_pal, 24u);
    const EntryHeader valid_hdr = read_entry_header(valid_2a, 0u);
    const std::size_t mask_off = GPK_HEADER_SIZE + valid_hdr.packed_size;

    // Valid baseline before corrupt inputs (valid -> error -> valid seam).
    const DwqRgbaImage pre_ok = decode_dwq_masked_rgba(valid_2a.data(), valid_2a.size());
    EXPECT_EQ(pre_ok.pixels, base_img.pixels);

    // Table-driven invalid MASK bfOffBits values (0, <54, 54 empty pal, misaligned 57/59, UINT32_MAX).
    for (std::uint32_t bad_off : {0u, 53u, 54u, 57u, 59u, 0xFFFFFFFFu}) {
        Bytes bad = valid_2a;
        bad[mask_off + 10u] = static_cast<std::uint8_t>(bad_off & 0xFFu);
        bad[mask_off + 11u] = static_cast<std::uint8_t>((bad_off >> 8u) & 0xFFu);
        bad[mask_off + 12u] = static_cast<std::uint8_t>((bad_off >> 16u) & 0xFFu);
        bad[mask_off + 13u] = static_cast<std::uint8_t>((bad_off >> 24u) & 0xFFu);
        EXPECT_THROW(decode_dwq_masked_rgba(bad.data(), bad.size()), std::runtime_error)
            << "bad_off=" << bad_off;
    }

    // Truncated mask section.
    EXPECT_THROW(decode_dwq_masked_rgba(valid_2a.data(), mask_off + 20u), std::runtime_error);

    // packed_size < bmp_needed (claims color BMP is smaller than 54 + 4*12).
    {
        Bytes bad_packed = valid_2a;
        const std::uint32_t tiny = 60u;
        bad_packed[0x20u] = static_cast<std::uint8_t>(tiny & 0xFFu);
        bad_packed[0x21u] = 0u;
        bad_packed[0x22u] = 0u;
        bad_packed[0x23u] = 0u;
        EXPECT_THROW(decode_dwq_masked_rgba(bad_packed.data(), bad_packed.size()),
                     std::runtime_error);
    }

    // INT32_MIN BMP height must throw cleanly (no signed abs UB).
    {
        Bytes bad_h = valid_2a;
        bad_h[GPK_HEADER_SIZE + 22u] = 0x00u;
        bad_h[GPK_HEADER_SIZE + 23u] = 0x00u;
        bad_h[GPK_HEADER_SIZE + 24u] = 0x00u;
        bad_h[GPK_HEADER_SIZE + 25u] = 0x80u;
        EXPECT_THROW(decode_dwq_masked_rgba(bad_h.data(), bad_h.size()), std::runtime_error);

        cg::Image repl;
        repl.width = 4;
        repl.height = 4;
        repl.channels = 4;
        repl.px = base_img.pixels;
        EXPECT_THROW(build_entry_from_raw(bad_h.data(), bad_h.size(), cg::encode_png(repl)),
                     std::runtime_error);
    }

    // height > 65535 exceeds DWQ u16 height field and must throw.
    {
        EntryHeader tall_hdr{};
        tall_hdr.width = 16u;
        tall_hdr.height = 65535u;
        tall_hdr.bpp = 24u;
        // Synthetic PNG header with width=16, height=65536 (0x00010000).
        Bytes tall_png = {
            0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au,
            0x00u, 0x00u, 0x00u, 0x0Du, 'I', 'H', 'D', 'R',
            0x00u, 0x00u, 0x00u, 0x10u,
            0x00u, 0x01u, 0x00u, 0x00u,
        };
        EXPECT_THROW(build_entry(tall_hdr, tall_png), std::runtime_error);
    }

    // GTB entry literally named "over2G!" without an EOF over2G! trailer must
    // parse cleanly as a standard 32-bit GTB (has_64bit == false).
    const test_util::ScopedTempDir temp_dir("mgi_ut_gtb_corrupt_over2g");
    {
        const std::string gtb_path = temp_dir.join_u8("named_over2g.gtb");
        const std::vector<GtbEntry> entries = {{"over2G!", 0u}, {"tail_entry", 64u}};
        write_gtb(gtb_path, entries, false);
        const Gtb parsed = read_gtb(gtb_path);
        EXPECT_FALSE(parsed.has_64bit);
        ASSERT_EQ(parsed.entries.size(), 2u);
        EXPECT_EQ(parsed.entries[0].name, "over2G!");
        EXPECT_EQ(parsed.entries[1].name, "tail_entry");
        EXPECT_EQ(parsed.entries[1].offset, 64ull);
    }

    // Corrupt GTB with huge entry count must throw cleanly without bad_alloc.
    {
        const std::string corrupt_gtb = temp_dir.join_u8("huge_count.gtb");
        write_file(corrupt_gtb, Bytes{0xFFu, 0xFFu, 0xFFu, 0x7Fu, 0u, 0u, 0u, 0u});
        EXPECT_THROW(read_gtb(corrupt_gtb), std::runtime_error);
    }

    // Valid input still succeeds cleanly after all error paths (seam check).
    const DwqRgbaImage post_ok = decode_dwq_masked_rgba(valid_2a.data(), valid_2a.size());
    EXPECT_EQ(post_ok.pixels, base_img.pixels);
}
